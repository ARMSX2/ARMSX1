/*
    ARMSX — RetroAchievements (rcheevos) integration. See frontend/achievements.h for the
    threading contract and the softcore-only stance.
*/

#include "achievements.h"

#include "ra_ua.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../psx/psx.h"
#include "../psx/dev/ram.h"
#include "../psx/dev/scratchpad.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/cdrom/disc.h"
#include "config.h"
}

extern "C" {
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"
}

#if defined(__ANDROID__)
#include <android/log.h>
#define ARMSX_ACH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ARMSX-RA", __VA_ARGS__)
#define ARMSX_ACH_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ARMSX-RA", __VA_ARGS__)
#define ARMSX_ACH_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ARMSX-RA", __VA_ARGS__)
#else
#define ARMSX_ACH_LOGI(...) ((void)0)
#define ARMSX_ACH_LOGW(...) ((void)0)
#define ARMSX_ACH_LOGE(...) ((void)0)
#endif

namespace {

// --- Constants -----------------------------------------------------------------------------

// RA's console id for the original PlayStation. NOT the PS2 id the ARMSX2 integration uses —
// getting this wrong makes every hash miss the database and every game look unsupported.
constexpr uint32_t kConsoleId = RC_CONSOLE_PLAYSTATION;
static_assert(kConsoleId == 12u, "RetroAchievements PlayStation console id must be 12");

// RA's PS1 memory map (rcheevos consoleinfo.c): main RAM is the first 2 MiB, then the 1 KiB
// scratchpad. Everything above that is not addressable by an achievement.
constexpr uint32_t kRaMainRamSize = 0x200000u;
constexpr uint32_t kRaScratchpadSize = 0x400u;
constexpr uint32_t kRaExposedSize = kRaMainRamSize + kRaScratchpadSize;

constexpr int kServerCallTimeoutMs = 30000;
constexpr int kLoginTimeoutMs = 45000;

// --- State ---------------------------------------------------------------------------------

// Everything rc_client touches is behind this. Recursive because rc_client invokes our event
// handler (and the read-memory callback) from inside calls we make while already holding it.
std::recursive_mutex g_lock;

rc_client_t* g_client = nullptr;
bool g_startup_done = false;
bool g_shutting_down = false;

psx_t* g_psx = nullptr;

armsx_ach_http_fn g_http_handler = nullptr;
void* g_http_user = nullptr;
armsx_ach_sound_fn g_sound_handler = nullptr;
void* g_sound_user = nullptr;
armsx_ach_notify_fn g_notify_handler = nullptr;
void* g_notify_user = nullptr;

std::string g_rich_presence;
std::string g_game_hash;
bool g_has_achievements = false;

// Deferred work the emulation-thread pump picks up: a token login restored from the store and a
// game load. Both do network / disc I/O that must not run on whichever thread asked for it.
bool g_pending_token_login = false;
bool g_pending_game_load = false;
bool g_game_load_in_flight = false;

std::string g_user_agent;

// --- Persisted store -----------------------------------------------------------------------
//
// <pref>/achievements.ini, one key=value per line. Deliberately NOT settings.toml: the RA auth
// token is not something to leave in the file a user pastes into a bug report, and the TOML
// loader in frontend/config.c is read-only anyway.
//
// There is no HardcoreMode key. Softcore is not a stored preference here, it is the only mode.

std::map<std::string, std::string> g_store;
bool g_store_loaded = false;

std::string StorePath() {
    const char* pref = psxe_cfg_get_pref_path();
    if (!pref || !*pref) {
        return {};
    }

    std::string path(pref);
    if (path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += "achievements.ini";
    return path;
}

void StoreLoad() {
    if (g_store_loaded) {
        return;
    }

    // Latch only once a pref path actually exists. Marking the store loaded while it is still
    // unresolved would pin an empty map for the rest of the process and silently lose the saved
    // login — the Android UI can poll before any pref path has been handed in.
    const std::string path = StorePath();
    if (path.empty()) {
        return;
    }
    g_store_loaded = true;

    std::ifstream file(path);
    if (!file) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }
        g_store[line.substr(0, split)] = line.substr(split + 1);
    }
}

void StoreSave() {
    const std::string path = StorePath();
    if (path.empty()) {
        return;
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        ARMSX_ACH_LOGW("could not write %s", path.c_str());
        return;
    }

    file << "# ARMSX RetroAchievements state. Contains an auth token - do not share.\n";
    for (const auto& entry : g_store) {
        file << entry.first << '=' << entry.second << '\n';
    }
}

std::string StoreGet(const char* key, const char* fallback = "") {
    StoreLoad();
    const auto it = g_store.find(key);
    return it == g_store.end() ? std::string(fallback) : it->second;
}

bool StoreGetBool(const char* key, bool fallback) {
    const std::string value = StoreGet(key, fallback ? "1" : "0");
    return value == "1" || value == "true";
}

int StoreGetInt(const char* key, int fallback) {
    const std::string value = StoreGet(key);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

void StoreSet(const char* key, const std::string& value) {
    StoreLoad();
    g_store[key] = value;
    StoreSave();
}

void StoreSetBool(const char* key, bool value) {
    StoreSet(key, value ? "1" : "0");
}

void StoreSetInt(const char* key, int value) {
    StoreSet(key, std::to_string(value));
}

void StoreErase(const char* key) {
    StoreLoad();
    g_store.erase(key);
    StoreSave();
}

// --- HTTP request queue --------------------------------------------------------------------
//
// rc_client hands us a request and a callback it wants invoked with the response. The transport
// blocks, so each request gets a detached worker; the finished response is parked here and
// handed back to rc_client from Poll(), which always runs on a thread holding g_lock. That
// keeps rcheevos strictly single-threaded, which is what RC_NO_THREADS assumes.

struct PendingResponse {
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    uint64_t generation = 0;
    armsx_ach_http_response response;
};

std::mutex g_http_mutex;
std::deque<PendingResponse> g_http_done;
int g_http_in_flight = 0;
// Bumped whenever the client is destroyed. callback_data points into rc_client's arena, so a
// response that lands after a shutdown must be dropped, not delivered — otherwise closing a game
// with a request still on the wire and then opening the RetroAchievements screen again would
// hand a fresh client's pump a callback into freed memory.
uint64_t g_http_generation = 0;

void HttpEnqueue(const std::string& url, const std::string& post_data, bool has_post,
                 const std::string& content_type, rc_client_server_callback_t callback,
                 void* callback_data) {
    armsx_ach_http_fn handler = nullptr;
    void* user = nullptr;
    uint64_t generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        handler = g_http_handler;
        user = g_http_user;
    }
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        generation = g_http_generation;
    }

    if (!handler) {
        // No transport installed (non-Android host, or too early). Answer immediately with a
        // client error so rc_client's state machine unwinds instead of waiting forever.
        PendingResponse pending;
        pending.callback = callback;
        pending.callback_data = callback_data;
        pending.generation = generation;
        pending.response.status_code = -1;
        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_done.push_back(std::move(pending));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        ++g_http_in_flight;
    }

    std::thread worker([handler, user, url, post_data, has_post, content_type, callback,
                        callback_data, generation]() {
        PendingResponse pending;
        pending.callback = callback;
        pending.callback_data = callback_data;
        pending.generation = generation;
        handler(url.c_str(), has_post ? post_data.c_str() : nullptr,
                content_type.empty() ? nullptr : content_type.c_str(),
                armsx_ach_user_agent(), kServerCallTimeoutMs, &pending.response, user);

        std::lock_guard<std::mutex> lock(g_http_mutex);
        g_http_done.push_back(std::move(pending));
        --g_http_in_flight;
    });
    worker.detach();
}

// Hand every finished response back to rc_client. Caller must hold g_lock.
void HttpPoll() {
    for (;;) {
        PendingResponse pending;
        {
            std::lock_guard<std::mutex> lock(g_http_mutex);
            if (g_http_done.empty()) {
                return;
            }
            pending = std::move(g_http_done.front());
            g_http_done.pop_front();
            if (pending.generation != g_http_generation) {
                continue; // outlived the client it belonged to
            }
        }

        if (!pending.callback) {
            continue;
        }

        const bool is_error = pending.response.status_code <= 0;
        const bool is_retryable = is_error && pending.response.status_code == -2; // HttpClient timeout

        rc_api_server_response_t server_response{};
        server_response.http_status_code = is_error
            ? (is_retryable ? RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR
                            : RC_API_SERVER_RESPONSE_CLIENT_ERROR)
            : pending.response.status_code;
        server_response.body = pending.response.body.c_str();
        server_response.body_length = pending.response.body.size();

        pending.callback(&server_response, pending.callback_data);
    }
}

bool HttpBusy() {
    std::lock_guard<std::mutex> lock(g_http_mutex);
    return g_http_in_flight > 0 || !g_http_done.empty();
}

// --- CHD cdreader --------------------------------------------------------------------------
//
// rcheevos' own cdreader understands .cue / .gdi / raw .bin / .iso, and the RA hash database was
// generated with it, so those formats deliberately keep using it untouched. It has no CHD
// support, which is the format most PS1 users actually have — this bridge routes .chd through
// ARMSX's own disc layer instead. Sector semantics mirror cdreader_determine_sector_size():
// libchdr always hands back a raw 2352-byte sector, so all this has to get right is whether the
// user data starts 16 or 24 bytes in, which "CD001" at offset 25 of sector 16 settles.

constexpr int kChdSectorSize = CD_SECTOR_SIZE;
// User data per sector, the same figure the stock cdreader uses (raw_data_size). Mode 1 puts it
// 16 bytes in, mode 2 form 1 24 bytes in; either way it is 2048 bytes long.
constexpr size_t kChdSectorDataSize = 2048;

struct ChdTrack {
    psx_disc_t* disc = nullptr;
    uint32_t first_sector = 0;
    int header_size = 16;
};

void DestroyDisc(psx_disc_t* disc) {
    if (!disc) {
        return;
    }

    // psx_disc_destroy() calls disc->destroy unconditionally, and a failed open leaves it null
    // (psx_disc_close is declared but has no implementation, so it is not an option here).
    if (disc->destroy) {
        psx_disc_destroy(disc);
    } else {
        free(disc);
    }
}

void* RC_CCONV ChdOpenTrack(const char* path, uint32_t track, const rc_hash_iterator_t* /*iterator*/) {
    // Only the first data track matters for the PS1 hash; rc_hash_psx never asks for another.
    if (track != 1 && track != RC_HASH_CDTRACK_FIRST_DATA) {
        return nullptr;
    }

    psx_disc_t* disc = psx_disc_create();
    if (!disc) {
        return nullptr;
    }

    if (psx_disc_open(disc, path) == CDT_ERROR) {
        DestroyDisc(disc);
        return nullptr;
    }

    ChdTrack* handle = new ChdTrack();
    handle->disc = disc;
    handle->first_sector = static_cast<uint32_t>(std::max(0, psx_disc_get_track_lba(disc, 1)));

    uint8_t sector[kChdSectorSize];
    if (psx_disc_read(disc, handle->first_sector + 16u, sector) == TS_DATA) {
        handle->header_size = std::memcmp(&sector[25], "CD001", 5) == 0 ? 24 : 16;
    }

    return handle;
}

size_t RC_CCONV ChdReadSector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    ChdTrack* handle = static_cast<ChdTrack*>(track_handle);
    if (!handle || !handle->disc || !buffer) {
        return 0;
    }

    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;

    while (requested_bytes > 0) {
        uint8_t raw[kChdSectorSize];
        if (psx_disc_read(handle->disc, sector, raw) != TS_DATA) {
            return total;
        }

        const size_t chunk = std::min(requested_bytes, kChdSectorDataSize);
        std::memcpy(out, raw + handle->header_size, chunk);
        out += chunk;
        total += chunk;
        requested_bytes -= chunk;
        ++sector;
    }

    return total;
}

void RC_CCONV ChdCloseTrack(void* track_handle) {
    ChdTrack* handle = static_cast<ChdTrack*>(track_handle);
    if (!handle) {
        return;
    }

    DestroyDisc(handle->disc);
    delete handle;
}

uint32_t RC_CCONV ChdFirstTrackSector(void* track_handle) {
    const ChdTrack* handle = static_cast<const ChdTrack*>(track_handle);
    return handle ? handle->first_sector : 0u;
}

bool PathIsChd(const char* path) {
    if (!path) {
        return false;
    }
    const std::size_t length = std::strlen(path);
    if (length < 4) {
        return false;
    }
    const char* ext = path + length - 4;
    return (ext[0] == '.') &&
           (ext[1] == 'c' || ext[1] == 'C') &&
           (ext[2] == 'h' || ext[2] == 'H') &&
           (ext[3] == 'd' || ext[3] == 'D');
}

std::string HashDiscImage(const char* path) {
    if (!path || !*path) {
        return {};
    }

    rc_hash_iterator_t iterator;
    rc_hash_initialize_iterator(&iterator, path, nullptr, 0);

    if (PathIsChd(path)) {
        iterator.callbacks.cdreader.open_track = nullptr;
        iterator.callbacks.cdreader.open_track_iterator = ChdOpenTrack;
        iterator.callbacks.cdreader.read_sector = ChdReadSector;
        iterator.callbacks.cdreader.close_track = ChdCloseTrack;
        iterator.callbacks.cdreader.first_track_sector = ChdFirstTrackSector;
    }

    char hash[33] = {};
    const int ok = rc_hash_generate(hash, kConsoleId, &iterator);
    rc_hash_destroy_iterator(&iterator);

    if (!ok || !hash[0]) {
        ARMSX_ACH_LOGW("could not hash %s", path);
        return {};
    }

    return std::string(hash);
}

// --- Notice queue --------------------------------------------------------------------------
//
// The one route out of this module to something the user can see. rc_client's callbacks (game
// loaded, achievement triggered, login rejected) run from HttpPoll(), i.e. on whichever thread
// happened to be pumping — the emulation thread most of the time, but the Java IO thread driving
// armsx_ach_login() whenever a login is in flight, and the caller of armsx_ach_shutdown()
// otherwise. Calling the host from there would mean a JNI call from an arbitrary thread while
// g_lock is held, so instead every notice is parked here and DrainNotices() delivers it from the
// emulation-thread pump with no lock held. Same shape as the psxe_host_* request parking in
// frontend/main.cpp.

struct PendingNotice {
    std::string text;
    int duration_ms = 5000;
};

std::mutex g_notice_mutex;
std::deque<PendingNotice> g_notice_queue;
std::chrono::steady_clock::time_point g_notice_next_at{};

// The host banner has a single slot, so a burst delivered in one go would leave only the last one
// readable. Notices are spaced out instead — by their own duration, capped so a long queue of
// unlocks does not run minutes behind the game.
constexpr int kNoticeMaxSpacingMs = 3500;

// A hard cap so a disconnect storm cannot grow the queue without bound. Sixteen is far more than
// will ever be readable anyway.
constexpr size_t kNoticeQueueMax = 16;

// Park a note. Caller holds g_lock (the store reads below need it).
void QueueNotice(std::string text) {
    if (text.empty()) {
        return;
    }

    PendingNotice notice;
    notice.duration_ms = std::clamp(StoreGetInt("NotificationsDuration", 5), 3, 30) * 1000;
    notice.text = std::move(text);

    std::lock_guard<std::mutex> lock(g_notice_mutex);
    if (g_notice_queue.size() >= kNoticeQueueMax) {
        return; // keep the oldest; they are the ones the user has been waiting to see
    }
    g_notice_queue.push_back(std::move(notice));
}

// Achievement/gameplay chatter, suppressed by the user's "notifications" option. Failures that
// mean achievements are silently not being tracked deliberately go through QueueNotice() instead
// — turning notifications off asks for less noise, not for a broken session to stay hidden.
void QueueAchievementNotice(std::string text) {
    if (!StoreGetBool("Notifications", true)) {
        return;
    }
    QueueNotice(std::move(text));
}

// Hand parked notices to the host. Must be called with g_lock NOT held.
void DrainNotices() {
    armsx_ach_notify_fn handler = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        handler = g_notify_handler;
        user = g_notify_user;
    }

    PendingNotice notice;
    {
        std::lock_guard<std::mutex> lock(g_notice_mutex);
        if (g_notice_queue.empty()) {
            return;
        }
        if (!handler) {
            // No host installed (non-Android, or before EnsureAchievementsReady). Drop them
            // rather than let the queue sit full and swallow later ones.
            g_notice_queue.clear();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < g_notice_next_at) {
            return; // the previous banner is still up
        }

        notice = std::move(g_notice_queue.front());
        g_notice_queue.pop_front();
        g_notice_next_at = now + std::chrono::milliseconds(
            std::min(notice.duration_ms, kNoticeMaxSpacingMs));
    }

    handler(notice.text.c_str(), notice.duration_ms, user);
}

void ClearNotices() {
    std::lock_guard<std::mutex> lock(g_notice_mutex);
    g_notice_queue.clear();
    g_notice_next_at = {};
}

// --- rc_client callbacks -------------------------------------------------------------------

uint32_t RC_CCONV ClientReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes,
                                   rc_client_t* /*client*/) {
    if (!g_psx || !buffer || num_bytes == 0) {
        return 0;
    }

    if (static_cast<uint64_t>(address) + num_bytes > kRaExposedSize) {
        return 0;
    }

    // A read may not straddle the RAM/scratchpad boundary — RA's map places them next to each
    // other, the hardware does not.
    if (address < kRaMainRamSize) {
        if (address + num_bytes > kRaMainRamSize) {
            return 0;
        }

        const psx_ram_t* ram = g_psx->ram;
        if (!ram || !ram->buf || ram->size < kRaMainRamSize) {
            return 0;
        }

        std::memcpy(buffer, ram->buf + address, num_bytes);
        return num_bytes;
    }

    const psx_scratchpad_t* scratchpad = g_psx->scratchpad;
    if (!scratchpad || !scratchpad->buf) {
        return 0;
    }

    std::memcpy(buffer, scratchpad->buf + (address - kRaMainRamSize), num_bytes);
    return num_bytes;
}

void RC_CCONV ClientServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                               void* callback_data, rc_client_t* /*client*/) {
    if (!request || !request->url) {
        return;
    }

    HttpEnqueue(request->url,
                request->post_data ? request->post_data : "",
                request->post_data != nullptr,
                request->content_type ? request->content_type : "",
                callback, callback_data);
}

void RC_CCONV ClientMessage(const char* message, const rc_client_t* /*client*/) {
    ARMSX_ACH_LOGI("%s", message ? message : "");
    (void)message;
}

void PlayUnlockSound() {
    if (!StoreGetBool("SoundEffects", true)) {
        return;
    }

    const std::string path = StoreGet("UnlockSoundPath");
    if (path.empty() || !g_sound_handler) {
        return;
    }

    g_sound_handler(path.c_str(), g_sound_user);
}

void RC_CCONV ClientEventHandler(const rc_client_event_t* event, rc_client_t* client) {
    if (!event) {
        return;
    }

    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
            if (event->achievement) {
                ARMSX_ACH_LOGI("unlocked: %s (%u pts)",
                               event->achievement->title ? event->achievement->title : "?",
                               event->achievement->points);

                std::string text = "Achievement unlocked: ";
                text += (event->achievement->title && *event->achievement->title)
                            ? event->achievement->title
                            : "(untitled)";
                if (event->achievement->points > 0) {
                    text += " (";
                    text += std::to_string(event->achievement->points);
                    text += event->achievement->points == 1 ? " point)" : " points)";
                }
                QueueAchievementNotice(std::move(text));
            }
            PlayUnlockSound();
            break;
        }

        case RC_CLIENT_EVENT_GAME_COMPLETED: {
            ARMSX_ACH_LOGI("game completed");

            // "Completed", not "Mastered". On RetroAchievements those are two different
            // accomplishments and mastery is the hardcore one — which this build cannot earn.
            const rc_client_game_t* game = client ? rc_client_get_game_info(client) : nullptr;
            std::string text = "Completed ";
            text += (game && game->title && *game->title) ? game->title : "this game";

            rc_client_user_game_summary_t summary{};
            if (client) {
                rc_client_get_user_game_summary(client, &summary);
            }
            if (summary.num_core_achievements > 0) {
                text += " — all ";
                text += std::to_string(summary.num_core_achievements);
                text += " achievements earned";
            } else {
                text += " — every achievement earned";
            }
            QueueAchievementNotice(std::move(text));
            PlayUnlockSound();
            break;
        }

        case RC_CLIENT_EVENT_SUBSET_COMPLETED: {
            std::string text = "Completed ";
            text += (event->subset && event->subset->title && *event->subset->title)
                        ? event->subset->title
                        : "this achievement set";
            ARMSX_ACH_LOGI("%s", text.c_str());
            QueueAchievementNotice(std::move(text));
            PlayUnlockSound();
            break;
        }

        case RC_CLIENT_EVENT_SERVER_ERROR: {
            const char* message = event->server_error && event->server_error->error_message
                                      ? event->server_error->error_message
                                      : nullptr;
            ARMSX_ACH_LOGE("server error: %s", message ? message : "(unknown)");
            // Ungated: this is a request rcheevos has given up on, so something the user did
            // (most often an unlock) did not reach their account.
            std::string text = "RetroAchievements error";
            if (message && *message) {
                text += ": ";
                text += message;
            }
            QueueNotice(std::move(text));
            break;
        }

        case RC_CLIENT_EVENT_DISCONNECTED:
            ARMSX_ACH_LOGW("disconnected from RetroAchievements");
            QueueAchievementNotice("RetroAchievements is offline — unlocks will be sent when the connection returns");
            break;

        case RC_CLIENT_EVENT_RECONNECTED:
            ARMSX_ACH_LOGI("reconnected to RetroAchievements");
            QueueAchievementNotice("RetroAchievements reconnected — pending unlocks sent");
            break;

        default:
            // Deliberately not handled: the challenge and progress indicators
            // (RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_* /
            // RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_*) are persistent on-screen widgets,
            // not transient notes, and the host banner is a single transient slot — routing a
            // show/hide pair through it would flash a banner every time a challenge armed. The
            // leaderboard events are unhandled because leaderboards are not implemented at all on
            // this core (no submission, no tracker, no scoreboard). RC_CLIENT_EVENT_RESET only
            // fires when hardcore is switched on, which this module cannot do.
            break;
    }
}

// --- Client lifecycle ----------------------------------------------------------------------

// Push every persisted option into the client. Hardcore is set here, once, to 0 — this is the
// ONLY rc_client_set_hardcore_enabled call in the module and its argument is a literal.
void ApplyClientOptions() {
    if (!g_client) {
        return;
    }

    rc_client_set_hardcore_enabled(g_client, 0);
    rc_client_set_encore_mode_enabled(g_client, StoreGetBool("EncoreMode", false) ? 1 : 0);
    rc_client_set_spectator_mode_enabled(g_client, StoreGetBool("SpectatorMode", false) ? 1 : 0);
    rc_client_set_unofficial_enabled(g_client, StoreGetBool("UnofficialTestMode", false) ? 1 : 0);
}

bool CreateClient() {
    if (g_client) {
        return true;
    }

    g_client = rc_client_create(ClientReadMemory, ClientServerCall);
    if (!g_client) {
        ARMSX_ACH_LOGE("rc_client_create() failed");
        return false;
    }

    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, ClientMessage);
    rc_client_set_event_handler(g_client, ClientEventHandler);
    ApplyClientOptions();

    const std::string host = StoreGet("Host");
    if (!host.empty()) {
        ARMSX_ACH_LOGI("using host override %s", host.c_str());
        rc_client_set_host(g_client, host.c_str());
    }

    return true;
}

void RC_CCONV LoadGameCallback(int result, const char* error_message, rc_client_t* client,
                               void* /*userdata*/) {
    g_game_load_in_flight = false;

    if (result != RC_OK) {
        g_has_achievements = false;
        const char* detail = error_message && *error_message ? error_message : rc_error_str(result);

        if (result == RC_NO_GAME_LOADED) {
            // An unrecognised disc. NOTHING is shown here, on purpose, and this is the same
            // choice ARMSX2 makes (Achievements::ClientLoadGameCallback writes a console line for
            // RC_NO_GAME_LOADED and returns). Most PS1 discs a user actually owns are not in the
            // RA database — homebrew, demos, unlisted regional pressings — so a banner on every
            // one of those boots would be a recurring notice about something the user cannot act
            // on, and it would be indistinguishable from a real failure. The log line stays.
            ARMSX_ACH_LOGI("no achievement set for this disc: %s", detail);
        } else if (result == RC_LOGIN_REQUIRED) {
            // The sign-in re-arms the load; there is nothing to report yet, and TokenLoginCallback
            // speaks up if that sign-in is the thing that failed.
            ARMSX_ACH_LOGI("disc identification deferred until sign-in completes");
        } else {
            // A real failure — network, server, a malformed response. Worth saying: without it
            // the achievements panel is simply empty for a game that does have a set, which
            // reads exactly like "achievements are broken".
            ARMSX_ACH_LOGW("could not load achievements: %s", detail);
            std::string text = "RetroAchievements: could not load this game's achievements";
            if (detail && *detail) {
                text += " — ";
                text += detail;
            }
            QueueNotice(std::move(text));
        }
        return;
    }

    g_has_achievements = rc_client_has_achievements(client) != 0;

    const rc_client_game_t* game = rc_client_get_game_info(client);
    ARMSX_ACH_LOGI("loaded '%s' (id %u), achievements=%s",
                   game && game->title ? game->title : "?",
                   game ? game->id : 0u,
                   g_has_achievements ? "yes" : "no");

    // The boot summary, the thing a RetroAchievements user expects to see the moment a game
    // starts. Wording follows ARMSX2's DisplayAchievementSummary(): the title, then how much of
    // the set is already done, or "no achievements" for a game that is in the database with an
    // empty set.
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);

    std::string text = (game && game->title && *game->title) ? game->title : "This game";
    // Only ever appended when hardcore is genuinely on. It is wired shut in this module, so
    // asking is how the text stays honest instead of hard-coding an assumption in a second place.
    if (armsx_ach_hardcore_active()) {
        text += " (Hardcore Mode)";
    }
    if (summary.num_core_achievements > 0) {
        text += " — ";
        text += std::to_string(summary.num_unlocked_achievements);
        text += " of ";
        text += std::to_string(summary.num_core_achievements);
        text += " achievements earned, ";
        text += std::to_string(summary.points_unlocked);
        text += " of ";
        text += std::to_string(summary.points_core);
        text += " points";
    } else {
        text += " — this game has no achievements";
    }
    QueueAchievementNotice(std::move(text));
}

void RC_CCONV TokenLoginCallback(int result, const char* error_message, rc_client_t* /*client*/,
                                 void* /*userdata*/) {
    if (result != RC_OK) {
        ARMSX_ACH_LOGW("saved login rejected: %s", error_message ? error_message : rc_error_str(result));
        // A stale token would otherwise be retried on every boot forever.
        StoreErase("Token");
        // Ungated by the "notifications" option: the account is signed out from here on and every
        // achievement earned this session goes nowhere. Silently erasing the token and carrying on
        // is exactly the failure the user cannot diagnose — the panel just shows the login form
        // again with no explanation of when or why.
        QueueNotice("RetroAchievements sign-in expired — sign in again from the Achievements screen");
        return;
    }

    // Nothing is shown for a successful token restore. It lands a frame or two before the game
    // summary below re-arms and fires, and the host banner has one slot — a "signed in" note here
    // would be overwritten by the summary before it could be read. The summary is the proof that
    // the sign-in worked.
    ARMSX_ACH_LOGI("signed in from saved token");
    if (g_psx) {
        g_pending_game_load = true;
    }
}

struct PasswordLoginState {
    bool done = false;
    bool ok = false;
    std::string error;
};

void RC_CCONV PasswordLoginCallback(int result, const char* error_message, rc_client_t* client,
                                    void* userdata) {
    PasswordLoginState* state = static_cast<PasswordLoginState*>(userdata);
    if (!state) {
        return;
    }

    state->done = true;

    if (result != RC_OK) {
        state->ok = false;
        state->error = error_message && *error_message ? error_message : rc_error_str(result);
        return;
    }

    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (!user || !user->token) {
        state->ok = false;
        state->error = "RetroAchievements did not return a login token.";
        return;
    }

    state->ok = true;
    StoreSet("Username", user->username ? user->username : "");
    StoreSet("DisplayName", user->display_name ? user->display_name : (user->username ? user->username : ""));
    StoreSet("Token", user->token);
    StoreSetInt("LastScore", static_cast<int>(user->score));
    StoreSetInt("LastScoreSoftcore", static_cast<int>(user->score_softcore));

    char url[512];
    if (rc_client_user_get_image_url(user, url, sizeof(url)) == RC_OK) {
        StoreSet("AvatarUrl", url);
    }
}

// Kick a game load for whatever disc is mounted. Caller holds g_lock and must be on the
// emulation thread — this reads the disc image.
void BeginLoadGame() {
    g_pending_game_load = false;

    if (!g_client || !g_psx) {
        return;
    }

    if (!rc_client_get_user_info(g_client)) {
        return; // not signed in yet; the token/password login re-arms this
    }

    psx_cdrom_t* cdrom = psx_get_cdrom(g_psx);
    const char* disc_path = cdrom ? psx_cdrom_get_disc_path(cdrom) : nullptr;
    if (!disc_path || !*disc_path) {
        return; // BIOS or .exe boot — nothing to identify
    }

    const std::string hash = HashDiscImage(disc_path);
    if (hash.empty()) {
        return;
    }

    if (hash == g_game_hash && (g_has_achievements || g_game_load_in_flight)) {
        return; // same disc, already loaded
    }

    g_game_hash = hash;
    g_has_achievements = false;
    g_game_load_in_flight = true;
    ARMSX_ACH_LOGI("identifying disc %s (hash %s)", disc_path, hash.c_str());
    rc_client_begin_load_game(g_client, hash.c_str(), LoadGameCallback, nullptr);
}

// --- JSON ----------------------------------------------------------------------------------

void AppendJsonString(std::string& dst, const char* src) {
    dst += '"';
    if (src) {
        for (const char* p = src; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
                case '"':  dst += "\\\""; break;
                case '\\': dst += "\\\\"; break;
                case '\n': dst += "\\n";  break;
                case '\r': dst += "\\r";  break;
                case '\t': dst += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char escape[8];
                        std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                        dst += escape;
                    } else {
                        dst += static_cast<char>(c);
                    }
                    break;
            }
        }
    }
    dst += '"';
}

void AppendJsonBool(std::string& dst, const char* key, bool value) {
    dst += ",\"";
    dst += key;
    dst += "\":";
    dst += value ? "true" : "false";
}

void AppendJsonInt(std::string& dst, const char* key, long long value) {
    dst += ",\"";
    dst += key;
    dst += "\":";
    dst += std::to_string(value);
}

} // namespace

// ------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------

void armsx_ach_set_http_handler(armsx_ach_http_fn handler, void* user) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_http_handler = handler;
    g_http_user = user;
}

void armsx_ach_set_sound_handler(armsx_ach_sound_fn handler, void* user) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_sound_handler = handler;
    g_sound_user = user;
}

void armsx_ach_set_notify_handler(armsx_ach_notify_fn handler, void* user) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_notify_handler = handler;
    g_notify_user = user;
}

const char* armsx_ach_user_agent(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_user_agent.empty()) {
        g_user_agent = ARMSX1_RA_UA_NAME "/" ARMSX1_RA_UA_VERSION
#if defined(__ANDROID__)
            " (Android)";
#else
            " (ARMSX)";
#endif
    }
    return g_user_agent.c_str();
}

void armsx_ach_startup(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_startup_done) {
        return;
    }

    g_shutting_down = false;
    if (!CreateClient()) {
        return;
    }

    // The saved login lives next to settings.toml, so there is nothing to restore until the host
    // has handed a pref path in. Leave startup un-latched in that case and let the next call
    // (the Android host makes one from every RetroAchievements JNI entry point) finish the job.
    if (StorePath().empty()) {
        return;
    }
    g_startup_done = true;

    if (!rc_client_get_user_info(g_client) &&
        !StoreGet("Username").empty() && !StoreGet("Token").empty()) {
        g_pending_token_login = true;
    }

    ARMSX_ACH_LOGI("startup, user agent '%s'", armsx_ach_user_agent());
}

void armsx_ach_shutdown(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_startup_done) {
        return;
    }

    g_shutting_down = true;
    g_psx = nullptr;

    // Let anything already on the wire land so an unlock submitted on the last frame is not
    // dropped, then tear the client down.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (HttpBusy() && std::chrono::steady_clock::now() < deadline) {
        HttpPoll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    HttpPoll();

    if (g_client) {
        rc_client_destroy(g_client);
        g_client = nullptr;
    }

    // Anything still on the wire now belongs to a client that no longer exists. Retire the
    // generation so a late worker's response is dropped instead of being handed to whatever
    // client comes next.
    {
        std::lock_guard<std::mutex> lock(g_http_mutex);
        ++g_http_generation;
        g_http_done.clear();
    }

    g_startup_done = false;
    g_has_achievements = false;
    g_game_hash.clear();
    g_rich_presence.clear();
    ClearNotices();
}

void armsx_ach_session_started(psx_t* psx) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_psx = psx;
    g_game_hash.clear();
    g_has_achievements = false;
    g_rich_presence.clear();
    g_pending_game_load = psx != nullptr;
    // A note parked while no session was running (the RA screen is reachable from the library, so
    // the pump that drains them may not have run for a long time) must not surface over the boot
    // of an unrelated game. This runs before any of this session's own notices are queued.
    ClearNotices();
}

void armsx_ach_session_ended(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_psx = nullptr;
    g_pending_game_load = false;
    g_game_load_in_flight = false;
    g_has_achievements = false;
    g_game_hash.clear();
    g_rich_presence.clear();
    // Anything still parked belongs to the game that just closed — an unlock banner arriving over
    // the library after the fact would be about a session that no longer exists.
    ClearNotices();

    if (g_client) {
        rc_client_unload_game(g_client);
    }
}

void armsx_ach_frame_update(bool stepping) {
    // Every rc_client callback that has something to tell the user parks it instead of saying it
    // (see the notice queue), so this pump is where it actually reaches the screen. Scoped so the
    // drain below runs with g_lock released: the host handler makes a JNI call, and no part of
    // this module should be able to stall behind it.
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (!g_client || g_shutting_down) {
            return;
        }

        HttpPoll();

        if (g_pending_token_login) {
            g_pending_token_login = false;
            const std::string user = StoreGet("Username");
            const std::string token = StoreGet("Token");
            if (!user.empty() && !token.empty()) {
                rc_client_begin_login_with_token(g_client, user.c_str(), token.c_str(),
                                                 TokenLoginCallback, nullptr);
            }
        }

        if (g_pending_game_load && !g_game_load_in_flight) {
            BeginLoadGame();
        }

        if (stepping && g_has_achievements) {
            rc_client_do_frame(g_client);
        } else {
            rc_client_idle(g_client);
        }

        char buffer[512];
        const size_t length = rc_client_get_rich_presence_message(g_client, buffer, sizeof(buffer));
        g_rich_presence.assign(buffer, length);
    }

    DrainNotices();
}

std::string armsx_ach_get_json(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);

    std::string out;
    out.reserve(4096);
    out += '{';

    const rc_client_user_t* user = g_client ? rc_client_get_user_info(g_client) : nullptr;

    // The library's RA tab is opened with no VM at all — and on Android there is no emulation
    // loop then, so no client has ever run. Fall back to what the last successful login wrote so
    // the panel still shows the signed-in account instead of the login form.
    std::string display_name;
    bool logged_in = false;
    if (user && user->display_name) {
        display_name = user->display_name;
        logged_in = true;
    } else {
        std::string saved_user = StoreGet("DisplayName");
        if (saved_user.empty()) {
            saved_user = StoreGet("Username");
        }
        if (!saved_user.empty() && !StoreGet("Token").empty()) {
            display_name = saved_user;
            logged_in = true;
        }
    }

    out += "\"active\":";
    out += (g_has_achievements ? "true" : "false");
    AppendJsonBool(out, "loggedIn", logged_in);
    // Softcore only. This is reported false unconditionally, not read back from anywhere, so no
    // future settings edit can flip it by accident.
    AppendJsonBool(out, "hardcore", false);
    out += ",\"userName\":";
    AppendJsonString(out, display_name.c_str());

    AppendJsonInt(out, "score", user ? static_cast<long long>(user->score)
                                     : StoreGetInt("LastScore", -1));
    AppendJsonInt(out, "softcoreScore", user ? static_cast<long long>(user->score_softcore)
                                             : StoreGetInt("LastScoreSoftcore", -1));

    std::string avatar_url;
    if (user) {
        char url[512];
        if (rc_client_user_get_image_url(user, url, sizeof(url)) == RC_OK) {
            avatar_url = url;
        }
    }
    if (avatar_url.empty()) {
        avatar_url = StoreGet("AvatarUrl");
    }
    out += ",\"avatarUrl\":";
    AppendJsonString(out, avatar_url.c_str());

    AppendJsonBool(out, "notifications", StoreGetBool("Notifications", true));
    AppendJsonBool(out, "leaderboardNotifications", StoreGetBool("LeaderboardNotifications", true));
    AppendJsonBool(out, "overlays", StoreGetBool("Overlays", true));
    AppendJsonBool(out, "lbOverlays", StoreGetBool("LBOverlays", true));
    AppendJsonBool(out, "soundEffects", StoreGetBool("SoundEffects", true));
    AppendJsonBool(out, "encoreMode", StoreGetBool("EncoreMode", false));
    AppendJsonBool(out, "spectatorMode", StoreGetBool("SpectatorMode", false));
    AppendJsonBool(out, "unofficialTestMode", StoreGetBool("UnofficialTestMode", false));
    AppendJsonInt(out, "notificationsDuration", StoreGetInt("NotificationsDuration", 5));
    AppendJsonInt(out, "leaderboardsDuration", StoreGetInt("LeaderboardsDuration", 10));
    AppendJsonInt(out, "notificationPosition", StoreGetInt("NotificationPosition", 1));
    AppendJsonInt(out, "overlayPosition", StoreGetInt("OverlayPosition", 8));

    out += ",\"items\":[";
    if (g_client && g_has_achievements) {
        // LOCK_STATE grouping so every bucket carries the real subset_id (PROGRESS grouping
        // misfiles in-progress achievements into subset 0).
        rc_client_achievement_list_t* list = rc_client_create_achievement_list(
            g_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
            RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);

        if (list) {
            static const uint32_t bucket_order[] = {
                RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
                RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED,
                RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED,
                RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE,
                RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
                RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL,
                RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED,
            };

            bool first = true;
            for (uint32_t wanted : bucket_order) {
                for (uint32_t b = 0; b < list->num_buckets; ++b) {
                    const rc_client_achievement_bucket_t& bucket = list->buckets[b];
                    if (bucket.bucket_type != wanted) {
                        continue;
                    }
                    for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
                        const rc_client_achievement_t* achievement = bucket.achievements[a];
                        if (!achievement) {
                            continue;
                        }
                        if (!first) {
                            out += ',';
                        }
                        first = false;

                        out += "{\"id\":";
                        out += std::to_string(achievement->id);
                        out += ",\"title\":";
                        AppendJsonString(out, achievement->title);
                        out += ",\"description\":";
                        AppendJsonString(out, achievement->description);
                        AppendJsonInt(out, "points", achievement->points);
                        AppendJsonBool(out, "unlocked",
                                       achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
                        // rc_client's SOFTCORE/HARDCORE bitmask. Softcore-only means bit 1 is the
                        // only one that can ever be set here, which is exactly what the Kotlin
                        // side wants to see.
                        AppendJsonInt(out, "unlockedMask", static_cast<int>(achievement->unlocked));
                        AppendJsonBool(out, "primed",
                                       achievement->bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE);
                        AppendJsonInt(out, "bucket", static_cast<int>(bucket.bucket_type));
                        AppendJsonInt(out, "subsetId", bucket.subset_id);
                        AppendJsonInt(out, "type", static_cast<int>(achievement->type));
                        AppendJsonInt(out, "unlockTime", static_cast<long long>(achievement->unlock_time));
                        out += ",\"measuredProgress\":";
                        AppendJsonString(out, achievement->measured_progress);
                        {
                            char buffer[32];
                            std::snprintf(buffer, sizeof(buffer), "%.1f", achievement->measured_percent);
                            out += ",\"measuredPercent\":";
                            out += buffer;
                            std::snprintf(buffer, sizeof(buffer), "%.1f", achievement->rarity);
                            out += ",\"rarity\":";
                            out += buffer;
                        }
                        {
                            char url[256];
                            const int rc = rc_client_achievement_get_image_url(
                                achievement, achievement->state, url, sizeof(url));
                            out += ",\"iconUrl\":";
                            AppendJsonString(out, rc == RC_OK ? url : "");
                        }
                        out += '}';
                    }
                }
            }
            rc_client_destroy_achievement_list(list);
        }
    }
    out += ']';

    out += ",\"subsets\":[";
    if (g_client && g_has_achievements) {
        rc_client_subset_list_t* subsets = rc_client_create_subset_list(g_client);
        if (subsets) {
            bool first = true;
            for (uint32_t s = 0; s < subsets->num_subsets; ++s) {
                const rc_client_subset_t* subset = subsets->subsets[s];
                if (!subset) {
                    continue;
                }
                if (!first) {
                    out += ',';
                }
                first = false;
                out += "{\"id\":";
                out += std::to_string(subset->id);
                out += ",\"title\":";
                AppendJsonString(out, subset->title);
                AppendJsonInt(out, "numAchievements", subset->num_achievements);
                out += '}';
            }
            rc_client_destroy_subset_list(subsets);
        }
    }
    out += "]}";

    return out;
}

std::string armsx_ach_get_rich_presence(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_rich_presence;
}

std::string armsx_ach_login(const char* username, const char* password) {
    if (!username || !*username || !password || !*password) {
        return "Enter your RetroAchievements username and password.";
    }

    PasswordLoginState state;

    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (!CreateClient()) {
            return "Could not create the RetroAchievements client.";
        }
        g_shutting_down = false;

        if (!rc_client_begin_login_with_password(g_client, username, password,
                                                 PasswordLoginCallback, &state)) {
            return "Could not start the login request.";
        }
    }

    // Pump the request here rather than waiting for the emulation loop: this call has to work
    // from the library, where no VM (and therefore no loop) exists. The lock is taken in short
    // bursts so a running game keeps its frame pace.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLoginTimeoutMs);
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            HttpPoll();
            if (state.done) {
                break;
            }
            rc_client_idle(g_client);
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return "Timed out talking to RetroAchievements.";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!state.ok) {
        return state.error.empty() ? std::string("Login failed.") : state.error;
    }

    ApplyClientOptions();
    if (g_psx) {
        g_pending_game_load = true; // the emulation thread picks this up and hashes the disc
    }

    return {};
}

void armsx_ach_logout(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);

    if (g_client) {
        rc_client_logout(g_client);
    }

    StoreErase("Token");
    StoreErase("Username");
    StoreErase("DisplayName");
    StoreErase("AvatarUrl");
    StoreErase("LastScore");
    StoreErase("LastScoreSoftcore");

    g_has_achievements = false;
    g_game_hash.clear();
    g_rich_presence.clear();
    g_pending_token_login = false;
}

void armsx_ach_set_option(const char* key, bool enabled) {
    if (!key || !*key) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_lock);

    const std::string name(key);
    if (name == "notifications") {
        StoreSetBool("Notifications", enabled);
    } else if (name == "leaderboardNotifications") {
        StoreSetBool("LeaderboardNotifications", enabled);
    } else if (name == "overlays") {
        StoreSetBool("Overlays", enabled);
    } else if (name == "lbOverlays") {
        StoreSetBool("LBOverlays", enabled);
    } else if (name == "soundEffects") {
        StoreSetBool("SoundEffects", enabled);
    } else if (name == "encoreMode") {
        StoreSetBool("EncoreMode", enabled);
        if (g_client) {
            rc_client_set_encore_mode_enabled(g_client, enabled ? 1 : 0);
        }
    } else if (name == "spectatorMode") {
        StoreSetBool("SpectatorMode", enabled);
        if (g_client) {
            rc_client_set_spectator_mode_enabled(g_client, enabled ? 1 : 0);
        }
    } else if (name == "unofficialTestMode") {
        StoreSetBool("UnofficialTestMode", enabled);
        if (g_client) {
            rc_client_set_unofficial_enabled(g_client, enabled ? 1 : 0);
        }
    }
    // "hardcore" is deliberately not a key here. Nothing the UI can send turns it on.
}

void armsx_ach_set_option_int(const char* key, int value) {
    if (!key || !*key) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_lock);

    const std::string name(key);
    if (name == "notificationsDuration") {
        StoreSetInt("NotificationsDuration", std::clamp(value, 3, 30));
    } else if (name == "leaderboardsDuration") {
        StoreSetInt("LeaderboardsDuration", std::clamp(value, 3, 30));
    } else if (name == "notificationPosition") {
        StoreSetInt("NotificationPosition", std::clamp(value, 0, 9));
    } else if (name == "overlayPosition") {
        StoreSetInt("OverlayPosition", std::clamp(value, 0, 8));
    }
}

void armsx_ach_set_unlock_sound(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (path && *path) {
        StoreSet("UnlockSoundPath", path);
    } else {
        StoreErase("UnlockSoundPath");
    }
}

void armsx_ach_set_host_override(const char* host) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!host || !*host) {
        armsx_ach_clear_host_override();
        return;
    }

    StoreSet("Host", host);
    if (g_client) {
        rc_client_set_host(g_client, host);
    }
    ARMSX_ACH_LOGI("host override -> %s", host);
}

void armsx_ach_clear_host_override(void) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    StoreErase("Host");
    if (g_client) {
        // "" (not null — rc_client_set_host dereferences the argument) puts it back on
        // retroachievements.org.
        rc_client_set_host(g_client, "");
    }
    ARMSX_ACH_LOGI("host override cleared");
}

std::string armsx_ach_hash_for_path(const char* image_path) {
    return HashDiscImage(image_path);
}

bool armsx_ach_hardcore_active(void) {
    // Softcore only, by construction. Not a stored value and not derived from the client, so
    // there is nothing here that a later change could accidentally make true.
    return false;
}
