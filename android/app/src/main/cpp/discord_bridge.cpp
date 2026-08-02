// SPDX-FileCopyrightText: 2026 ARMSX contributors
// SPDX-License-Identifier: MIT
//
// Licensed MIT, deliberately, and NOT under ARMSX's own terms.
//
// This file is on the helper side of the Discord process boundary: it links, or belongs to the
// process that links, Discord's proprietary Social SDK. Keeping this side permissive is what lets
// the emulator side (DiscordPresence.kt and the Friends UI) consume the shared IPC definitions
// without the two becoming one program. Ported from ARMSX2, where the same boundary is a hard
// licensing requirement; ARMSX is proprietary so the GPL argument does not apply here, but the
// boundary is kept for the runtime property it buys — see below.
//
// Copyright (c) 2026 ARMSX contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// Discord Social SDK bridge: rich presence, and which friends are in ARMSX right now.
//
// DELIBERATELY FREE-STANDING. This file includes no ARMSX header and links against no ARMSX code:
// only JNI and the Discord SDK. It builds into its own libarmsx_discord.so, loaded only in the
// :discord process; libarmsx has no DT_NEEDED on the SDK and never loads it.
//
// Why that still matters in a proprietary app: the service is bound-only and never started, so the
// last unbind destroys the process. With the feature off, the proprietary library is not merely
// idle — it is not loaded at all. And an SDK crash takes down a helper process, not the emulator
// mid-game. Both are worth having on a handheld.
//
// If you are tempted to call into the emulator from here: don't. Send a message to the app process
// and let it do that.
//
// Shape of this file, and why:
//
// Kotlin POLLS this, rather than the bridge calling up into Java. The SDK fires its callbacks on
// its own threads, so pushing would mean AttachCurrentThread plus a global ref whose lifetime has
// to outlive both the Activity and the SDK — for a feature the SDK itself only runs while the app
// is foregrounded. Polling a snapshot behind a mutex has none of that, and "once a second while a
// screen is open" is not a budget worth optimising.
//
// Everything is behind ARMSX_HAS_DISCORD. A tree without the SDK staged still builds; every entry
// point below has a stub that reports "unavailable", so the Kotlin side needs no build-flavour
// awareness at all.
//
// NOTE ON SYMBOL NAMES: the JNI symbols below spell com_armsx2 because the Java package really is
// com.armsx2.discord — ARMSX lifted ARMSX2's UI wholesale and kept the package, while the
// application id is com.nanodata.armsx. Renaming these to com_armsx would unbind them from
// DiscordNative and every call would throw UnsatisfiedLinkError at first use.

#include <jni.h>

#include <android/log.h>

#define DTAG "ARMSXDiscord"
#define DLOGI(...) __android_log_print(ANDROID_LOG_INFO, DTAG, __VA_ARGS__)
#define DLOGW(...) __android_log_print(ANDROID_LOG_WARN, DTAG, __VA_ARGS__)

#ifdef ARMSX_HAS_DISCORD

// discordpp.h is a single-header library: everything below its DISCORDPP_IMPLEMENTATION guard is
// the C++ wrapper's bodies, and the shipped .so exports ONLY the C API (Discord_*). Without this
// define the file compiles perfectly and then fails at link with an undefined symbol for every
// single call, which reads like a missing library rather than a missing macro. This must stay the
// one and only translation unit that defines it; a second would be duplicate-symbol errors instead.
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Supplied by build.sh from android/app/src/main/assets/discord_creds.env, so the application id
// and the callback scheme have exactly one source and cannot drift from the manifest — which
// derives its intent-filter scheme from the same file, via build.gradle.
#ifndef ARMSX_DISCORD_APPLICATION_ID
#error "ARMSX_DISCORD_APPLICATION_ID must be defined (see build.sh android)"
#endif
#ifndef ARMSX_DISCORD_CALLBACK_SCHEME
#error "ARMSX_DISCORD_CALLBACK_SCHEME must be defined (see build.sh android)"
#endif

namespace
{
	// Connection state, mirrored to Kotlin as an int so the UI can render without a second call.
	// Kept deliberately coarse: the UI only distinguishes "can I press Connect", "wait", and "done".
	enum class BridgeStatus : int
	{
		Disabled = 0,
		Disconnected = 1,
		Authorizing = 2,
		Connecting = 3,
		Connected = 4,
		Failed = 5,
	};

	struct Friend
	{
		std::string name;
		std::string game;   // title they are playing; empty means sitting in the library
		std::string serial; // for cover art on our side; empty when they are in the library
		std::string avatar; // their Discord avatar
	};

	// Record/field separators for the friends list handed to Kotlin. Deliberately control
	// characters: a display name or a game title can legitimately contain almost any printable
	// character, and a friend called "a|b" must not turn into two rows.
	constexpr char kFieldSep = '\x1f';
	constexpr char kRecordSep = '\x1e';

	struct State
	{
		std::mutex mutex;
		std::shared_ptr<discordpp::Client> client;

		std::atomic<int> status{static_cast<int>(BridgeStatus::Disconnected)};

		// Handed to Kotlin once so it can persist it and skip the browser next launch. Cleared on
		// read: it is a credential, and there is no reason for it to sit in native memory after the
		// one consumer has it.
		std::string fresh_token;
		std::string error;
		std::vector<Friend> friends;
		// The signed-in account, so the UI can show whose Discord this is.
		std::string self_name;
		std::string self_avatar;

		// Last presence asked for, replayed after a (re)connect. Presence set before Connect is
		// cleared by the connect itself, which the SDK header calls out explicitly, so the only safe
		// way to keep it is to remember it and set it again once connected.
		std::string want_serial;
		std::string want_title;
		std::string want_cover;
		// RetroAchievements rich presence — the game's own "what you are doing" line, which RA
		// updates as you play ("Wumpa Island, 12/45"). Empty when RA is off or the game has no set.
		std::string want_ra;
	};

	State& S()
	{
		static State s;
		return s;
	}

	constexpr uint64_t kApplicationId = ARMSX_DISCORD_APPLICATION_ID;
	// Must match the intent filter in AndroidManifest.xml AND the redirect URI registered in the
	// Discord developer portal. The SDK does not derive it for us; the shape is the one its own
	// header documents for mobile, "discord-APP_ID:/authorize/callback" (one slash, no authority).
	//
	// Built by literal concatenation, so the scheme half is the SAME token the manifest's intent
	// filter gets from discord_creds.env and the two cannot drift.
	const char* kRedirectUri = ARMSX_DISCORD_CALLBACK_SCHEME ":/authorize/callback";
	// Presence and the friends list. Deliberately no voice scopes: we do not ship voice, and the
	// consent screen should not ask for anything we cannot use.
	const char* kScopes = "sdk.social_layer_presence sdk.social_layer";
	// LargeImage takes "an identifier OR URL" (discordpp.h), and we use both kinds.
	//
	// Our own logo is an Art Asset uploaded to the application in the developer portal, referenced
	// by its key: upload the ARMSX mark as "armsx" under Rich Presence -> Art Assets. Referencing a
	// URL in the repo instead looked equivalent in ARMSX2 and was not — it pinned the presence to
	// whatever mark was committed at the time.
	const char* kIdleImageKey = "armsx";

	// ★ An unuploaded key does NOT "simply render as no image", which is what this file used to
	// claim and build on. Discord REJECTS THE ENTIRE UpdateRichPresence — "Unable to resolve large
	// image asset: armsx" — so one missing image meant no rich presence at all: connected, signed
	// in, and publishing nothing. The key stays (upload it and the logo returns with no code
	// change), but it is no longer allowed to take the whole status down with it. Cleared the first
	// time Discord rejects an update over it, after which every push omits it.
	std::atomic<bool> g_asset_key_resolves{true};

	const char* StatusName(BridgeStatus s)
	{
		switch (s)
		{
			case BridgeStatus::Disabled: return "Disabled";
			case BridgeStatus::Disconnected: return "Disconnected";
			case BridgeStatus::Authorizing: return "Authorizing";
			case BridgeStatus::Connecting: return "Connecting";
			case BridgeStatus::Connected: return "Connected";
			case BridgeStatus::Failed: return "Failed";
		}
		return "?";
	}

	void SetStatus(BridgeStatus s)
	{
		S().status.store(static_cast<int>(s), std::memory_order_release);
		DLOGI("status -> %s", StatusName(s));
	}

	// Push the remembered game to Discord. Caller must NOT hold the mutex: UpdateRichPresence can
	// invoke its callback inline.
	void PushPresence()
	{
		std::shared_ptr<discordpp::Client> client;
		std::string serial, title, cover, ra;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
			serial = S().want_serial;
			title = S().want_title;
			cover = S().want_cover;
			ra = S().want_ra;
		}
		if (!client)
			return;

		discordpp::Activity activity;
		// Name and applicationId are overwritten by the SDK — the header says so, and it is why
		// Discord will read "Playing ARMSX" with the game underneath rather than the other way
		// round. Details is therefore where the game has to go.
		activity.SetType(discordpp::ActivityTypes::Playing);
		if (!title.empty())
		{
			activity.SetDetails(title);
			// Second line of the Discord status. RetroAchievements' own rich presence when there is
			// one -- that is worth reading; a disc serial is not, and it was needless detail to
			// publish about someone. Falls back to the serial so this stays the in-a-game marker: a
			// friend's client decides "library or game" purely from whether State is set, so a game
			// with no serial AND no RA still has to set something.
			activity.SetState(!ra.empty() ? ra : (serial.empty() ? std::string("-") : serial));
		}
		else
		{
			// No State at all. That absence is what tells a friend's client we are in the library,
			// which beats matching on this string — it is English, and it is a display string.
			activity.SetDetails(std::string("In the library"));
		}

		// The game's own cover while playing, our logo when idle. The cover URL is whatever the
		// library is already showing (xlenore's psx-covers, keyed by serial), so a game with no
		// cover published simply falls back to the logo rather than showing a broken image. Mixing
		// the two forms is fine: Discord resolves each asset field independently, so a URL in the
		// large slot and an asset key in the small one both render.
		//
		// Every use of the key is conditional on it having resolved: an application with no "armsx"
		// asset uploaded publishes an image-less presence rather than none at all.
		const bool use_key = g_asset_key_resolves.load(std::memory_order_acquire);
		discordpp::ActivityAssets assets;
		if (!cover.empty())
			assets.SetLargeImage(cover);
		else if (use_key)
			assets.SetLargeImage(std::string(kIdleImageKey));
		// LargeText carries the SERIAL, because State no longer can -- a friend's client reads this
		// to look up cover art. Nothing else may be appended: it is parsed, not just shown. (Hover
		// text on the cover is a reasonable home for a serial anyway.)
		assets.SetLargeText(serial.empty() ? (title.empty() ? std::string("ARMSX") : title) : serial);
		if (!cover.empty() && use_key)
		{
			// Small badge in the corner keeps ARMSX identifiable when the big image is a cover.
			assets.SetSmallImage(std::string(kIdleImageKey));
			assets.SetSmallText(std::string("ARMSX"));
		}
		activity.SetAssets(std::move(assets));

		client->UpdateRichPresence(std::move(activity), [use_key](discordpp::ClientResult result) {
			if (result.Successful())
				return;
			const std::string error = result.Error();
			DLOGW("presence update failed: %s", error.c_str());
			// "Unable to resolve <large|small> image asset: armsx" — the key is not uploaded to this
			// application. Latch it off and publish once more without it, so the status still
			// appears. use_key is captured, so the retry (which runs with it false) cannot loop.
			if (use_key && error.find("image asset") != std::string::npos)
			{
				g_asset_key_resolves.store(false, std::memory_order_release);
				DLOGW("art asset '%s' does not resolve for this application; republishing without "
					  "the logo — upload it under Rich Presence -> Art Assets to restore it",
					kIdleImageKey);
				PushPresence();
			}
		});
	}

	void RefreshFriends()
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client)
			return;

		std::vector<Friend> found;
		// OnlinePlayingGame is the SDK's own grouping for "friend, online, in THIS application" —
		// exactly the set worth surfacing, and it means we never enumerate or display the rest of
		// someone's friends list.
		for (const auto& rel : client->GetRelationshipsByGroup(discordpp::RelationshipGroupType::OnlinePlayingGame))
		{
			const auto user = rel.User();
			if (!user.has_value())
				continue;

			Friend f;
			f.name = user->DisplayName();
			if (f.name.empty())
				continue;
			f.avatar = user->AvatarUrl(discordpp::UserHandle::AvatarType::Webp,
				discordpp::UserHandle::AvatarType::Png);

			// We author the activity these friends are publishing, so this reads back exactly what
			// PushPresence wrote: Details is the game title, State is their RA presence (or the
			// serial as a fallback), LargeText is the serial, and no State at all means they are
			// sitting in the library.
			if (const auto activity = user->GameActivity(); activity.has_value())
			{
				const auto state = activity->State();
				if (state.has_value() && !state->empty())
				{
					f.game = activity->Details().value_or(std::string());
					std::string large;
					if (const auto a = activity->Assets(); a.has_value())
						large = a->LargeText().value_or(std::string());
					const std::string candidate = !large.empty() ? large : *state;
					if (candidate != "-" && candidate != f.game && candidate != "ARMSX")
						f.serial = candidate;
				}
			}
			found.push_back(std::move(f));
		}

		std::lock_guard<std::mutex> lock(S().mutex);
		S().friends = std::move(found);
	}

	// Who we are signed in as. Refreshed alongside the friends list rather than once at connect:
	// GetCurrentUserV2 returns nullopt until the SDK has the account, and that is not guaranteed to
	// have happened the instant Ready fires.
	void RefreshSelf()
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client)
			return;

		const auto user = client->GetCurrentUserV2();
		if (!user.has_value())
			return;

		std::string name = user->DisplayName();
		std::string avatar = user->AvatarUrl(discordpp::UserHandle::AvatarType::Webp,
			discordpp::UserHandle::AvatarType::Png);

		std::lock_guard<std::mutex> lock(S().mutex);
		S().self_name = std::move(name);
		S().self_avatar = std::move(avatar);
	}

	void WireCallbacks(const std::shared_ptr<discordpp::Client>& client)
	{
		client->SetStatusChangedCallback([](discordpp::Client::Status status,
											 discordpp::Client::Error error, int32_t code) {
			DLOGI("sdk status=%d error=%d code=%d", static_cast<int>(status), static_cast<int>(error), code);
			if (status == discordpp::Client::Status::Ready)
			{
				SetStatus(BridgeStatus::Connected);
				// Both of these are only meaningful once Ready, and the presence in particular is
				// wiped by the connect, so this is the earliest correct moment for either.
				PushPresence();
				RefreshFriends();
				RefreshSelf();
			}
			else if (status == discordpp::Client::Status::Disconnected)
			{
				SetStatus(BridgeStatus::Disconnected);
				if (error != discordpp::Client::Error::None)
				{
					std::lock_guard<std::mutex> lock(S().mutex);
					S().error = discordpp::Client::ErrorToString(error) + " (" + std::to_string(code) + ")";
				}
			}
			else
			{
				SetStatus(BridgeStatus::Connecting);
			}
		});

		// Any change to who is online or what they are playing re-derives the list. The SDK gives no
		// finer-grained "this friend changed" for the group query, and the list is tiny.
		client->SetRelationshipCreatedCallback([](uint64_t, bool) { RefreshFriends(); });
		client->SetRelationshipDeletedCallback([](uint64_t, bool) { RefreshFriends(); });
		client->SetRelationshipGroupsUpdatedCallback([](uint64_t) { RefreshFriends(); });
	}

	void ConnectWithToken(const std::string& token)
	{
		std::shared_ptr<discordpp::Client> client;
		{
			std::lock_guard<std::mutex> lock(S().mutex);
			client = S().client;
		}
		if (!client || token.empty())
			return;

		SetStatus(BridgeStatus::Connecting);
		client->UpdateToken(discordpp::AuthorizationTokenType::Bearer, token,
			[client](discordpp::ClientResult result) {
				if (!result.Successful())
				{
					DLOGW("UpdateToken failed: %s", result.Error().c_str());
					{
						std::lock_guard<std::mutex> lock(S().mutex);
						S().error = result.Error();
					}
					SetStatus(BridgeStatus::Failed);
					return;
				}
				client->Connect();
			});
	}

	std::string JStr(JNIEnv* env, jstring s)
	{
		if (!s)
			return {};
		const char* c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		if (c)
			env->ReleaseStringUTFChars(s, c);
		return out;
	}
} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_armsx2_discord_DiscordNative_available(JNIEnv*, jclass)
{
	return JNI_TRUE;
}

/// Create the client and, when a saved token is passed, go straight to connecting. Idempotent.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_start(JNIEnv* env, jclass, jstring saved_token)
{
	const std::string token = JStr(env, saved_token);
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		if (!S().client)
		{
			S().client = std::make_shared<discordpp::Client>();
			S().client->SetApplicationId(kApplicationId);
			WireCallbacks(S().client);
			// The SDK's own diagnostics. Without this the only evidence of a failed handshake is the
			// UI sitting on "Connecting" forever.
			S().client->AddLogCallback([](std::string message, discordpp::LoggingSeverity severity) {
				DLOGI("[sdk sev=%d] %s", static_cast<int>(severity), message.c_str());
			}, discordpp::LoggingSeverity::Verbose);
			DLOGI("client created for application %llu", (unsigned long long)kApplicationId);
		}
	}
	if (!token.empty())
		ConnectWithToken(token);
	else
		SetStatus(BridgeStatus::Disconnected);
}

/// Full browser authorization. The SDK drives the handoff through its own AuthenticationActivity,
/// which is why that activity has to be declared in our manifest with the discord-<id> scheme.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_authorize(JNIEnv*, jclass)
{
	std::shared_ptr<discordpp::Client> client;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		client = S().client;
		S().error.clear();
	}
	if (!client)
		return;

	// Clear any authorize the SDK still thinks is running. One whose callback never arrived stays in
	// flight forever, and the SDK then ignores every subsequent Authorize() silently — no launch, no
	// error, the UI just sits on "Connecting".
	client->AbortAuthorize();

	SetStatus(BridgeStatus::Authorizing);
	DLOGI("Authorize() scopes='%s' redirect='%s'", kScopes, kRedirectUri);

	// PKCE. The verifier never leaves the device and the challenge is what goes to Discord, which is
	// what lets a public client finish the exchange with no client secret — there is no secret in
	// this app, and there must never be one.
	auto verifier = client->CreateAuthorizationCodeVerifier();

	discordpp::AuthorizationArgs args;
	args.SetClientId(kApplicationId);
	args.SetScopes(kScopes);
	args.SetCodeChallenge(verifier.Challenge());

	client->Authorize(args, [client, verifier = verifier.Verifier()](
							 discordpp::ClientResult result, std::string code, std::string /*redirect*/) {
		DLOGI("authorize callback: ok=%d code_len=%zu", result.Successful() ? 1 : 0, code.size());
		if (!result.Successful() || code.empty())
		{
			DLOGW("authorize failed: %s", result.Error().c_str());
			{
				std::lock_guard<std::mutex> lock(S().mutex);
				S().error = result.Successful() ? "No authorization code returned" : result.Error();
			}
			SetStatus(BridgeStatus::Failed);
			return;
		}

		client->GetToken(kApplicationId, code, verifier, kRedirectUri,
			[client](discordpp::ClientResult token_result, std::string token, std::string /*refresh*/,
				discordpp::AuthorizationTokenType, int32_t, std::string) {
				DLOGI("token callback: ok=%d token_len=%zu", token_result.Successful() ? 1 : 0, token.size());
				if (!token_result.Successful() || token.empty())
				{
					DLOGW("token exchange failed: %s", token_result.Error().c_str());
					{
						std::lock_guard<std::mutex> lock(S().mutex);
						S().error = token_result.Error();
					}
					SetStatus(BridgeStatus::Failed);
					return;
				}
				{
					std::lock_guard<std::mutex> lock(S().mutex);
					S().fresh_token = token;
				}
				ConnectWithToken(token);
			});
	});
}

/// Non-null exactly once per successful authorization, so Kotlin can persist it. Cleared on read.
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_takeToken(JNIEnv* env, jclass)
{
	std::string token;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		token.swap(S().fresh_token);
	}
	return token.empty() ? nullptr : env->NewStringUTF(token.c_str());
}

JNIEXPORT jint JNICALL
Java_com_armsx2_discord_DiscordNative_status(JNIEnv*, jclass)
{
	return S().status.load(std::memory_order_acquire);
}

JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_error(JNIEnv* env, jclass)
{
	std::lock_guard<std::mutex> lock(S().mutex);
	return S().error.empty() ? nullptr : env->NewStringUTF(S().error.c_str());
}

/// Remember and publish what is being played. Empty title = back in the library.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_setPlaying(
	JNIEnv* env, jclass, jstring serial, jstring title, jstring cover, jstring ra)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().want_serial = JStr(env, serial);
		S().want_title = JStr(env, title);
		S().want_cover = JStr(env, cover);
		S().want_ra = JStr(env, ra);
	}
	if (S().status.load(std::memory_order_acquire) == static_cast<int>(BridgeStatus::Connected))
		PushPresence();
}

/// The signed-in account as "name<FS>avatar". Empty until connected.
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_self(JNIEnv* env, jclass)
{
	// Re-read on demand: the account can land after the first Ready, and this is a cheap accessor
	// the UI already polls.
	RefreshSelf();
	std::string joined;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		if (!S().self_name.empty())
		{
			joined = S().self_name;
			joined.push_back(kFieldSep);
			joined += S().self_avatar;
		}
	}
	return env->NewStringUTF(joined.c_str());
}

/// Friends currently in ARMSX. Empty string is a legitimate answer and means nobody is on —
/// distinct from not-connected, which the caller reads from status().
JNIEXPORT jstring JNICALL
Java_com_armsx2_discord_DiscordNative_friends(JNIEnv* env, jclass)
{
	std::string joined;
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		for (const auto& f : S().friends)
		{
			if (!joined.empty())
				joined.push_back(kRecordSep);
			joined += f.name;
			joined.push_back(kFieldSep);
			joined += f.game;
			joined.push_back(kFieldSep);
			joined += f.serial;
			joined.push_back(kFieldSep);
			joined += f.avatar;
		}
	}
	return env->NewStringUTF(joined.c_str());
}

/// Drain the SDK's callback queue. Every callback wired above is dispatched from whoever calls
/// this, so it must be the thread that created the client — DiscordService posts it on the helper
/// process's main thread every 50 ms, which is also where MSG_START runs.
///
/// A plain discordpp::RunCallbacks(). ARMSX2 has to resolve Discord_RunCallbacks out of the SDK's
/// .so by hand, because upstream PCSX2 statically links 3rdparty/discord-rpc, which exports a
/// function of exactly that name and wins as an archive member — a silent no-op that delivered zero
/// callbacks. ARMSX vendors no discord-rpc, and this .so links nothing but the SDK and liblog, so
/// there is no symbol to collide with. Check that again before adding any library to this target.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_pump(JNIEnv*, jclass)
{
	discordpp::RunCallbacks();
}

/// Sign out. Drops the client entirely so no stale presence survives.
JNIEXPORT void JNICALL
Java_com_armsx2_discord_DiscordNative_stop(JNIEnv*, jclass)
{
	{
		std::lock_guard<std::mutex> lock(S().mutex);
		S().client.reset();
		S().friends.clear();
		S().fresh_token.clear();
		S().error.clear();
	}
	SetStatus(BridgeStatus::Disconnected);
}

} // extern "C"

#else // !ARMSX_HAS_DISCORD

// SDK not staged. Every entry point still exists so the Kotlin side is identical either way; it
// simply reports Disabled and does nothing. String returns follow the same contract as the rest of
// this port: "" for absent, never null, except takeToken/error where null IS the "nothing" value
// the callers test for.
extern "C" {
JNIEXPORT jboolean JNICALL Java_com_armsx2_discord_DiscordNative_available(JNIEnv*, jclass) { return JNI_FALSE; }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_start(JNIEnv*, jclass, jstring) {}
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_authorize(JNIEnv*, jclass) {}
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_takeToken(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT jint JNICALL Java_com_armsx2_discord_DiscordNative_status(JNIEnv*, jclass) { return 0; }
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_error(JNIEnv*, jclass) { return nullptr; }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_setPlaying(JNIEnv*, jclass, jstring, jstring, jstring, jstring) {}
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_friends(JNIEnv* env, jclass) { return env->NewStringUTF(""); }
JNIEXPORT jstring JNICALL Java_com_armsx2_discord_DiscordNative_self(JNIEnv* env, jclass) { return env->NewStringUTF(""); }
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_pump(JNIEnv*, jclass) {}
JNIEXPORT void JNICALL Java_com_armsx2_discord_DiscordNative_stop(JNIEnv*, jclass) {}
}

#endif // ARMSX_HAS_DISCORD
