#ifndef ARMSX_ACHIEVEMENTS_H
#define ARMSX_ACHIEVEMENTS_H

/*
    ARMSX — RetroAchievements (rcheevos) integration.

    SOFTCORE ONLY. Hardcore is wired shut at every layer: the client is created with
    rc_client_set_hardcore_enabled(client, 0) and nothing in this module ever calls it with a
    non-zero argument, the persisted store has no hardcore key, and the JNI toggle is a no-op
    that logs and returns. Until the emulator has had a lot more testing an unlock earned here
    must not be able to claim hardcore credit on a user's RA account.

    LAYOUT
    ------
    Everything rc_client touches lives behind one mutex (the emulation thread runs
    armsx_ach_frame_update(), the Android UI thread polls armsx_ach_get_json(), and a Java IO
    thread drives armsx_ach_login()). rcheevos is built with RC_NO_THREADS, so serialising here
    is the whole story.

    HTTP is not implemented in this file. The host installs a blocking transport with
    armsx_ach_set_http_handler() — on Android that is kr.co.iefriends.pcsx2.HttpClient via JNI —
    and this module runs it on its own worker threads, parking the responses until a pump
    (armsx_ach_frame_update, or the login loop) hands them back to rc_client on a thread that
    holds the lock.

    Memory reads go through the PS1 map RA publishes for RC_CONSOLE_PLAYSTATION:
    0x000000-0x1FFFFF is main RAM, 0x200000-0x2003FF is the scratchpad at 0x1F800000.
*/

#include <cstdint>
#include <string>

struct psx_t;

/* One HTTP transaction. The handler blocks until it has an answer and fills this in;
   status_code is the HTTP status, or negative for a transport failure (-1 generic, -2 timeout)
   matching kr.co.iefriends.pcsx2.HttpClient's sentinels. */
struct armsx_ach_http_response {
    int status_code = 0;
    std::string content_type;
    std::string body;
};

typedef void (*armsx_ach_http_fn)(const char* url, const char* post_data, const char* content_type,
                                  const char* user_agent, int timeout_ms,
                                  armsx_ach_http_response* out, void* user);

/* Fire-and-forget playback of an unlock sound. Only invoked when the user has picked one
   (armsx_ach_set_unlock_sound); ARMSX1 bundles no RA sounds of its own. */
typedef void (*armsx_ach_sound_fn)(const char* path, void* user);

/* What a notice is about. The host uses this to style the toast (accent colour, fallback icon)
   and nothing else — every kind carries the same fields, and an unknown kind must render as
   ARMSX_ACH_NOTICE_INFO rather than be dropped. */
enum {
    ARMSX_ACH_NOTICE_INFO = 0,        /* connection state, and anything uncategorised */
    ARMSX_ACH_NOTICE_LOGIN = 1,       /* signed in — [image_url] is the user's avatar */
    ARMSX_ACH_NOTICE_GAME = 2,        /* disc identified — [image_url] is the game's box art */
    ARMSX_ACH_NOTICE_UNLOCK = 3,      /* achievement earned — [image_url] is its badge */
    ARMSX_ACH_NOTICE_MASTERY = 4,     /* whole set (or a subset) completed — game box art */
    ARMSX_ACH_NOTICE_LEADERBOARD = 5, /* leaderboard attempt started/failed/submitted */
    ARMSX_ACH_NOTICE_ERROR = 6        /* something the user needs to act on */
};

/* One toast for the host's on-screen notification stack: signing in, the game summary at boot,
   an unlock, a leaderboard attempt, a sign-in that expired.

   [key] identifies the toast for replacement, and is never null or empty. A notice whose key
   matches one already on screen REPLACES it in place rather than stacking — this is what keeps a
   game that starts and submits six leaderboards in the same frame (they exist) from burying an
   unlock under a wall of its own attempt notices. Unlocks carry a per-achievement key so they
   always stack; leaderboards carry a per-leaderboard one so a start/submit pair collapses.

   [title] is always non-null and non-empty. [detail] is the second line and may be empty, never
   null. [image_url] is an https RetroAchievements image (avatar / box art / badge) and may be
   empty, never null — the host downloads and caches it; it must never block on that download
   before showing the text. [duration_ms] is the user's configured notification duration for this
   category (achievement notifications and leaderboard notifications are configured separately).

   Called ONLY from the notice pump (armsx_ach_frame_update / the login loop) with no lock held,
   never from an rc_client callback — those run on whichever thread happened to be draining HTTP,
   which on Android is not necessarily one that can talk to Java. The host may therefore assume a
   thread that is safe to make a JNI call from, but must NOT assume the UI thread. */
typedef void (*armsx_ach_notify_fn)(int kind, const char* key, const char* title,
                                    const char* detail, const char* image_url, int duration_ms,
                                    void* user);

void armsx_ach_set_http_handler(armsx_ach_http_fn handler, void* user);
void armsx_ach_set_sound_handler(armsx_ach_sound_fn handler, void* user);
void armsx_ach_set_notify_handler(armsx_ach_notify_fn handler, void* user);

/* The User-Agent every RA request carries. See frontend/ra_ua.h — it is a placeholder until
   the RA team issues ARMSX1 a client. */
const char* armsx_ach_user_agent(void);

/* Create the client and restore a saved login, if any. Safe to call repeatedly; the first call
   wins. Requires the pref path to be resolved (psxe_cfg_set_pref_path on Android). */
void armsx_ach_startup(void);
void armsx_ach_shutdown(void);

/* Session lifecycle, called from the emulation thread. [psx] is published for the memory reader
   and cleared by armsx_ach_session_ended(); the game hash is computed off the mounted disc on
   the next armsx_ach_frame_update() so a slow CHD open never lands inside a boot. */
void armsx_ach_session_started(psx_t* psx);
void armsx_ach_session_ended(void);

/* Emulation-thread pump: drains finished HTTP requests, runs rc_client_do_frame() (or
   rc_client_idle() when the session is not stepping) and refreshes rich presence. */
void armsx_ach_frame_update(bool stepping);

/* UI-thread queries. Never return null; the JSON one always returns a parseable object. */
std::string armsx_ach_get_json(void);
std::string armsx_ach_get_rich_presence(void);

/* Blocking password login. Returns an empty string on success or a human-readable error.
   Runs the HTTP round trip to completion — call off the UI thread. */
std::string armsx_ach_login(const char* username, const char* password);
void armsx_ach_logout(void);

/* Presentation options, mirrored back out through armsx_ach_get_json(). Keys match the Kotlin
   side: notifications, leaderboardNotifications, overlays, lbOverlays, soundEffects,
   encoreMode, spectatorMode, unofficialTestMode / notificationsDuration, leaderboardsDuration,
   notificationPosition, overlayPosition. */
void armsx_ach_set_option(const char* key, bool enabled);
void armsx_ach_set_option_int(const char* key, int value);
void armsx_ach_set_unlock_sound(const char* path);

/* Point the client at a loopback proxy (RA host override) and back. */
void armsx_ach_set_host_override(const char* host);
void armsx_ach_clear_host_override(void);

/* RA hash for a disc image that is not mounted, for the library's progress lookup. Empty when
   the image cannot be read or has no PS1 executable. Does disc I/O — call off the UI thread. */
std::string armsx_ach_hash_for_path(const char* image_path);

/* Always false. Kept so callers have something to ask instead of assuming. */
bool armsx_ach_hardcore_active(void);

#endif
