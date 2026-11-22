// Groove Music Radio for FH3: your own music on the radio's Groove station.
//
// FH3 kept FH2's Xbox Music service (renamed Groove), which Microsoft shut
// down. This plugin turns the station on and answers the service's web calls
// itself, so the game's own Groove player plays your music (the music library,
// Spotify and music-app sources come from my XboxMusicRadio for FH2):
//   - availability 0x1946030(player) always says yes (Audio Options rows and
//     the station), and the station lock 0x8ac910(profile, name) says
//     "unlocked" for "Xbox Music" (its entry is detoured: it is also used
//     through a pointer);
//   - the six web calls (sign-in, playlists, tracks, song URL) return our own
//     task handles. The game's poller reads a handle's status every frame
//     through 0x1787070 (from 0x17bc7a8), which first needs the online
//     service context; that call is routed through us, and our handles
//     answer from the music library: each folder in your Music folder is a
//     playlist, and a song's URL is its file path, which the game's player
//     opens with Media Foundation.
//
// Build: see build.sh. Install: mods\Forza Horizon 3\GrooveMusicRadio\plugins\GrooveMusicRadio.dll next to
// azimuth.exe. sdk\ holds copies of Extra Options' fh3_plugin.h and FH3 ANNA's fh3anna.h.
#define GROOVE_MUSIC_RADIO_VERSION "1.2.0"

#include "fh3_plugin.h"
#include "fh3anna.h"
#include "library.hpp"
#include "music_file.h"
#include "reader.hpp"
#include "apps.hpp"
#include "live.hpp"
#include "spotify.hpp"

#include <knownfolders.h>
#include <mferror.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

PcmSource* make_live_source(LivePlayer* player);
bool live_now_playing(LivePlayer* player, std::wstring& title, std::wstring& artist);
bool live_title(const LivePlayer* player, std::wstring& title, std::wstring& artist);

namespace {

const FH3PluginContext* host;
unsigned char* image;

enum : uint32_t {
    kAvailable = 0x1946030,
    kStationLocked = 0x8ac910,
    kStatusCall = 0x17bc7a8,  // poller: call 0x1787070 (task status)
    kStatus = 0x1787070,
    kCallPermissions = 0x1947081,     // 0x27b9560(result, task)
    kCallPlaylists = 0x1947971,       // 0x27b9030(result, task)
    kCallMorePlaylists = 0x1946d40,   // 0x27b87f0(token, result, task)
    kCallTracks = 0x19472df,          // 0x27b8ab0(id, result, task)
    kCallMoreTracks = 0x1947312,      // 0x27b8270(id, token, result, task)
    kCallMoreTracksShuffle = 0x19472b4, // 0x27b8530(id, token, result, task)
    kCallTracksShuffle = 0x1947269,   // 0x27b8d70(id, seed, result, task)
    kCallSongUrl = 0x194769f,         // 0x27b9290(id, client, context, result, task)
    // Xbox user privilege 0x18 (music) through 0x1070f90(privileges, index): Audio Options only lists
    // playlists, and the Groove player only starts, with it.
    kPrivilegeAudioOptions = 0x13b1fa2,
    kPrivilegePlayer = 0x112f1de,
    kCallOpenReader = 0x1991dab,      // the player's MFCreateSourceReaderFromURL(url, attributes, reader)
    kPlayer = 0x4e6d770,              // the XboxMusicPlayer (pointer)
    // Audio Options' X prompt, "Launch Groove Music": 0x112b4e0(help bar, 2, &label), and its action.
    kCallAudioPrompt = 0x13a9124,
    kAudioLaunchInvoke = 0x451b130 + 2 * 8,  // 0x13b5240
    kCreateUiString = 0x1214480,      // (&string): a new, empty UIString
    kAllocate = 0x1edbd00,            // (size): the game's heap
    kFreeUiString = 0x31eb20,         // a UIString whose count (+0x10) reached 0
    kStringTables = 0x4e7dda8,        // the loaded string tables (pointer); language code at +0x30
    kStringTablesVtable = 0x467d858,
};

void logf(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    host->log_utf8(line);
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- Station --------------------------------------------------------------------------------------------

bool string_is(const void* value, const char* text) {
    auto s = static_cast<const unsigned char*>(value);
    size_t size, capacity, length = std::strlen(text);
    std::memcpy(&size, s + 0x10, sizeof size);
    std::memcpy(&capacity, s + 0x18, sizeof capacity);
    if (size != length) return false;
    const char* chars = capacity > 15 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
    return std::memcmp(chars, text, length) == 0;
}

using StationLocked = unsigned char (*)(void*, const void*);
StationLocked original_locked;

// The Groove player's "download first" switch (player+0x6969, read by 0x196fd00): set, a song URL is
// fetched over HTTP by a download thread instead of opened through Media Foundation. The station lock
// check below normally sets it (from a developer setting, off) for "Xbox Music", but our detour skips
// that code, so it is cleared here, before every song.
void direct_streaming() {
    unsigned char* player;
    std::memcpy(&player, image + kPlayer, sizeof player);
    if (player) player[0x6969] = 0;
}

unsigned char station_locked(void* profile, const void* name) {
    if (name && string_is(name, "Xbox Music")) {
        direct_streaming();
        return 0;
    }
    return original_locked(profile, name);
}

bool detour_lock() {
    static const unsigned char prologue[16] = {0x48, 0x8b, 0xc4, 0x55, 0x57, 0x48, 0x83, 0xec,
                                               0x48, 0x80, 0x79, 0x58, 0x00, 0x48, 0x8b, 0xea};
    unsigned char* target = image + kStationLocked;
    if (std::memcmp(target, prologue, sizeof prologue) != 0) return false;
    auto trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(trampoline, prologue, sizeof prologue);
    unsigned char* at = trampoline + sizeof prologue;
    void* resume = target + sizeof prologue;
    const unsigned char jump_rip[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(at, jump_rip, 6);
    std::memcpy(at + 6, &resume, 8);
    DWORD protection;
    VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &protection);
    original_locked = reinterpret_cast<StationLocked>(trampoline);
    unsigned char jump[16] = {0xff, 0x25, 0, 0, 0, 0};
    void* hook = reinterpret_cast<void*>(&station_locked);
    std::memcpy(jump + 6, &hook, 8);
    jump[14] = jump[15] = 0x90;
    return host->patch_bytes(kStationLocked, prologue, jump, sizeof jump);
}

// ---- Replies (the service's reflection layouts, unchanged from FH2) ------------------------------------

constexpr size_t kPermSize = 2;
constexpr size_t kPlSize = 0x659C, kPlItems = 4, kPlItemSize = 0x3FC, kPlMax = 25;
constexpr size_t kPlItemTrackCount = 0, kPlItemId = 4, kPlItemName = 0x1FA;
constexpr size_t kPlTotal = 0x63A0, kPlTokenFlag = 0x63A4, kPlToken = 0x63A6;
constexpr size_t kTrSize = 0x297C0, kTrCollections = 0, kTrTotal = 4, kTrTracksFlag = 8, kTrCount = 0xC;
constexpr size_t kTrItems = 0x10, kTrItemSize = 0x1A78, kTrMax = 25, kTrTokenFlag = 0x295C8, kTrToken = 0x295CA;
constexpr size_t kSongId = 0, kSongName = 0x1F6, kSongArtistCount = 0x5F8, kSongArtists = 0x5FC, kSongTrackNumber = 0x1A74;
constexpr size_t kArtistName = 0, kArtistRole = 0x402;
constexpr size_t kUrlSize = 0x103E, kUrlUrl = 0, kUrlContentType = 0x1002;
constexpr size_t kIdChars = 0xFB, kPlNameChars = 0x101, kSongNameChars = 0x201, kRoleChars = 0xB;
constexpr size_t kUrlChars = 0x801, kContentTypeChars = 0x1E;
const wchar_t kSongPrefix[] = L"grm-song:";
const wchar_t kSongUrlPrefix[] = L"grm-song/";
// A live player (Spotify, a music app) is a playlist with one endless song.
const wchar_t kLivePrefix[] = L"grm-live:";
const wchar_t kLiveUrlPrefix[] = L"grm-live/";

LivePlayer* live_player(const std::wstring& id) {
    for (LivePlayer* p : live_players())
        if (id == p->id()) return p;
    return nullptr;
}

LivePlayer* live_player_at(const wchar_t* number) {
    std::vector<LivePlayer*> all = live_players();
    size_t i = wcstoul(number, nullptr, 10);
    return i < all.size() ? all[i] : nullptr;
}

size_t live_index(const LivePlayer* player) {
    std::vector<LivePlayer*> all = live_players();
    return (size_t)(std::find(all.begin(), all.end(), player) - all.begin());
}

void put(uint8_t* dst, size_t chars, const std::wstring& s) {
    size_t n = std::min(s.size(), chars - 1);
    std::memcpy(dst, s.data(), n * sizeof(wchar_t));
    std::memset(dst + n * sizeof(wchar_t), 0, sizeof(wchar_t));
}
void put32(uint8_t* dst, uint32_t v) { std::memcpy(dst, &v, sizeof v); }

uint32_t token_offset(const wchar_t* token) {
    if (!token) return 0;
    while (*token && !iswdigit(*token)) ++token;
    return (uint32_t)wcstoul(token, nullptr, 10);
}

const wchar_t* content_type(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".m4a" || ext == L".aac") return L"audio/mp4";
    if (ext == L".wma") return L"audio/x-ms-wma";
    if (ext == L".wav") return L"audio/wav";
    if (ext == L".flac") return L"audio/flac";
    return L"audio/mpeg";
}

enum class Kind { Permissions, Playlists, Tracks, SongUrl };
struct Request {
    Kind kind;
    uint8_t* result;
    std::wstring id;
    uint32_t offset = 0;
    uint32_t needs_gen = 1;
    bool shuffled = false;
    uint32_t seed = 0;
};

bool shuffle_enabled = true;  // [Music] Shuffle

// The game's shuffle seed (a number or any text) as 32 bits; FNV-1a.
uint32_t seed_of(const wchar_t* text) {
    uint32_t h = 2166136261u;
    for (; text && *text; ++text) h = (h ^ (uint32_t)*text) * 16777619u;
    return h;
}

// A "more tracks" token: "<next offset>" in folder order, "<next offset>.<seed hex>" when shuffled.
bool token_seed(const wchar_t* token, uint32_t& seed) {
    const wchar_t* dot = token ? std::wcschr(token, L'.') : nullptr;
    if (!dot) return false;
    seed = (uint32_t)wcstoul(dot + 1, nullptr, 16);
    return true;
}

// The same seed always gives the same order, so every page of a playlist agrees.
void shuffle_songs(std::vector<uint32_t>& songs, uint32_t seed) {
    uint64_t state = seed;
    auto next = [&state]() {  // splitmix64
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    };
    for (size_t i = songs.size(); i > 1; --i) std::swap(songs[i - 1], songs[(size_t)(next() % i)]);
}

HRESULT fill_permissions(const Request& r) {
    std::memset(r.result, 0, kPermSize);
    r.result[0] = 1;  // HasSubscription
    r.result[1] = 1;  // IsSubscriptionAvailableForPurchase
    return S_OK;
}

HRESULT fill_playlists(const Request& r) {
    std::vector<Playlist> lists = library_playlists();
    std::vector<Playlist> live;  // listed first; one "song" each so the game doesn't skip them as empty
    for (LivePlayer* p : live_players())
        if (p->listed()) live.push_back({p->id(), p->name(), {0}});
    lists.insert(lists.begin(), live.begin(), live.end());
    uint8_t* b = r.result;
    std::memset(b, 0, kPlSize);
    uint32_t total = (uint32_t)lists.size(), first = std::min(r.offset, total);
    uint32_t n = std::min<uint32_t>(total - first, kPlMax);
    put32(b, n);
    for (uint32_t i = 0; i < n; i++) {
        const Playlist& p = lists[first + i];
        uint8_t* item = b + kPlItems + i * kPlItemSize;
        put32(item + kPlItemTrackCount, (uint32_t)p.songs.size());
        put(item + kPlItemId, kIdChars, p.id);
        put(item + kPlItemName, kPlNameChars, p.name);
    }
    put32(b + kPlTotal, total);
    if (first + n < total) {
        b[kPlTokenFlag] = 1;
        put(b + kPlToken, kIdChars, L"p" + std::to_wstring(first + n));
    }
    if (!first) logf("Groove Music Radio: %u playlist(s) listed.", total);
    return S_OK;
}

HRESULT fill_live_track(const Request& r, LivePlayer* player) {
    std::wstring title, artist;
    // Names in scripts the radio font lacks (Chinese, Japanese, ...) show as boxes, as they did on Xbox.
    if (!live_now_playing(player, title, artist)) title = player->name();
    uint8_t* b = r.result;
    std::memset(b, 0, kTrSize);
    put32(b + kTrCollections, 1);
    put32(b + kTrTotal, 1);
    b[kTrTracksFlag] = 1;
    put32(b + kTrCount, r.offset ? 0 : 1);
    if (!r.offset) {
        uint8_t* item = b + kTrItems;
        put(item + kSongId, kIdChars, kLivePrefix + std::to_wstring(live_index(player)));
        // The song the app reported last (shown on the radio), else the player's name.
        put(item + kSongName, kSongNameChars, title);
        if (artist.empty()) artist = player->name();
        put32(item + kSongArtistCount, 1);
        put(item + kSongArtists + kArtistName, kSongNameChars, artist);
        put(item + kSongArtists + kArtistRole, kRoleChars, L"Main");
        put32(item + kSongTrackNumber, 1);
    }
    return S_OK;
}

HRESULT fill_tracks(const Request& r) {
    if (LivePlayer* player = live_player(r.id)) return fill_live_track(r, player);
    // The game's own default (and a folder renamed or removed since): all your music, the library's first
    // playlist ("All music" with several folders, else the only one).
    Playlist p;
    if (!library_playlist(r.id, p)) {
        std::vector<Playlist> lists = library_playlists();
        if (lists.empty()) {
            logf("Groove Music Radio: no music found for playlist \"%s\".", narrow(r.id).c_str());
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        p = lists.front();
    }
    if (r.shuffled) shuffle_songs(p.songs, r.seed);
    uint8_t* b = r.result;
    std::memset(b, 0, kTrSize);
    uint32_t total = (uint32_t)p.songs.size(), first = std::min(r.offset, total);
    uint32_t n = std::min<uint32_t>(total - first, kTrMax);
    put32(b + kTrCollections, 1);
    put32(b + kTrTotal, total);
    b[kTrTracksFlag] = 1;
    put32(b + kTrCount, n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t index = p.songs[first + i];
        Song s;
        library_song(index, s);
        uint8_t* item = b + kTrItems + i * kTrItemSize;
        put(item + kSongId, kIdChars, kSongPrefix + std::to_wstring(index));
        put(item + kSongName, kSongNameChars, s.title);
        if (!s.artist.empty()) {
            put32(item + kSongArtistCount, 1);
            put(item + kSongArtists + kArtistName, kSongNameChars, s.artist);
            put(item + kSongArtists + kArtistRole, kRoleChars, L"Main");
        }
        put32(item + kSongTrackNumber, first + i + 1);
    }
    if (first + n < total) {
        b[kTrTokenFlag] = 1;
        std::wstring token = std::to_wstring(first + n);
        if (r.shuffled) {
            wchar_t seed[16];
            swprintf(seed, 16, L".%08x", r.seed);
            token += seed;
        }
        put(b + kTrToken, kIdChars, token);
    }
    return S_OK;
}

HRESULT fill_song_url(const Request& r) {
    direct_streaming();
    size_t live = std::wcslen(kLivePrefix);
    if (!r.id.compare(0, live, kLivePrefix)) {
        LivePlayer* player = live_player_at(r.id.c_str() + live);
        if (!player) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        std::memset(r.result, 0, kUrlSize);
        put(r.result + kUrlUrl, kUrlChars, kLiveUrlPrefix + r.id.substr(live) + L"/audio.mp3");
        put(r.result + kUrlContentType, kContentTypeChars, L"audio/mpeg");
        if (!player->available())
            logf("Groove Music Radio: %s: %s.", narrow(player->name()).c_str(), player->missing_hint());
        else {
            std::wstring title, artist;
            if (live_title(player, title, artist))
                logf("Groove Music Radio: playing %s - %s (%s).", narrow(artist).c_str(), narrow(title).c_str(),
                     narrow(player->name()).c_str());
            else
                logf("Groove Music Radio: playing %s.", narrow(player->name()).c_str());
        }
        return S_OK;
    }
    Song s;
    size_t plen = std::wcslen(kSongPrefix);
    if (r.id.compare(0, plen, kSongPrefix) != 0 ||
        !library_song((uint32_t)wcstoul(r.id.c_str() + plen, nullptr, 10), s)) {
        logf("Groove Music Radio: unknown song id \"%s\".", narrow(r.id).c_str());
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (s.path.size() >= kUrlChars || GetFileAttributesW(s.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logf("Groove Music Radio: can't play %s.", narrow(s.path).c_str());
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    // The player only opens URLs naming a Groove stream ("audio.mp3", "audio.m4a", "audio.mp4" or
    // "transcode/mp3", checked by 0x19a70f0); open_song turns this one back into the file's path.
    std::memset(r.result, 0, kUrlSize);
    put(r.result + kUrlUrl, kUrlChars, kSongUrlPrefix + r.id.substr(plen) + L"/audio.mp3");
    put(r.result + kUrlContentType, kContentTypeChars, content_type(s.path));
    logf("Groove Music Radio: playing %s - %s.", narrow(s.artist).c_str(), narrow(s.title).c_str());
    return S_OK;
}

HRESULT fill(const Request& r) {
    switch (r.kind) {
    case Kind::Permissions: return fill_permissions(r);
    case Kind::Playlists: return fill_playlists(r);
    case Kind::Tracks: return fill_tracks(r);
    case Kind::SongUrl: return fill_song_url(r);
    }
    return E_UNEXPECTED;
}

// ---- Our task handles --------------------------------------------------------------------------------
// A handle's vtable: [0] AddRef(tag), [1] Release(tag), [2] status, [4] Close(tag). Status 2 and 3 mean
// "running"; anything else completes the call: the owner's success callback reads the reply.

std::mutex task_lock;
struct FakeTask {
    const void* const* vtable;
    volatile LONG refs;
    bool done, closed;
    HRESULT hr;
    Request request;
};
int64_t task_add_ref(FakeTask* t, const char*) { return InterlockedIncrement(&t->refs); }
int64_t task_release(FakeTask* t, const char*) {
    LONG n = InterlockedDecrement(&t->refs);
    if (n == 0) delete t;
    return n;
}
int32_t task_status(FakeTask* t) {
    std::lock_guard<std::mutex> lock(task_lock);
    if (t->done || t->closed) return t->hr;
    if (library_done() < t->request.needs_gen) return 2;  // still scanning
    t->hr = fill(t->request);
    t->done = true;
    return t->hr;
}
int64_t task_close(FakeTask* t, const char*) {
    std::lock_guard<std::mutex> lock(task_lock);
    t->closed = true;
    if (!t->done) t->hr = E_ABORT;
    return 0;
}
int64_t task_other(FakeTask*) { return 0; }
const void* const task_vtable[] = {
    (const void*)&task_add_ref, (const void*)&task_release, (const void*)&task_status,
    (const void*)&task_other,   (const void*)&task_close,   (const void*)&task_other,
    (const void*)&task_other,   (const void*)&task_other,
};

HRESULT start(Request request, void** task) {
    if (!task) return E_POINTER;
    *task = new FakeTask{task_vtable, 1, false, false, S_OK, std::move(request)};
    return S_OK;
}

using StatusFn = int32_t (*)(void*);
int32_t status(void* handle) {
    if (handle && *static_cast<void**>(handle) == (void*)task_vtable)
        return task_status(static_cast<FakeTask*>(handle));
    return reinterpret_cast<StatusFn>(image + kStatus)(handle);
}

unsigned char music_privilege(void*, int) { return 1; }

using OpenReader = HRESULT(WINAPI*)(const wchar_t*, void*, void**);
OpenReader open_reader;
// A music file, decoded (MP3, FLAC, WAV) to the player's 44.1 kHz stereo PCM.
std::atomic<uint32_t> skip_generation{0};  // raised by Next and Previous Song: the song playing ends

class FileSource final : public PcmSource {
public:
    explicit FileSource(MusicFile* track) : track_(track), generation_(skip_generation.load()) {}
    ~FileSource() override { music_close(track_); }
    size_t read(Frame* out, size_t room) override {
        return skip_generation.load() == generation_ ? music_read(track_, out, room) : 0;
    }

private:
    MusicFile* track_;
    uint32_t generation_;
};

// The player's one MFCreateSourceReaderFromURL call (0x1991dab): our song URLs get our own reader.
HRESULT WINAPI open_song(const wchar_t* url, void* attributes, void** reader) {
    size_t live = std::wcslen(kLiveUrlPrefix);
    if (url && !std::wcsncmp(url, kLiveUrlPrefix, live)) {
        if (!reader) return E_POINTER;
        LivePlayer* player = live_player_at(url + live);
        if (!player) return MF_E_NOT_FOUND;
        *reader = make_source_reader(make_live_source(player));
        return S_OK;
    }
    size_t prefix = std::wcslen(kSongUrlPrefix);
    if (!url || std::wcsncmp(url, kSongUrlPrefix, prefix)) return open_reader(url, attributes, reader);
    if (!reader) return E_POINTER;
    *reader = nullptr;
    Song song;
    if (!library_song((uint32_t)wcstoul(url + prefix, nullptr, 10), song)) return MF_E_NOT_FOUND;
    MusicFile* track = music_open(song.path.c_str());
    if (!track) {
        logf("Groove Music Radio: can't decode %s (MP3, FLAC and WAV play).", narrow(song.path).c_str());
        return MF_E_UNSUPPORTED_FORMAT;
    }
    *reader = make_source_reader(new FileSource(track));
    return S_OK;
}

// ---- Audio Options: X - Open Music Folder ----------------------------------------------------------
// The page's X prompt launched the Groove app (0x13b5240, the invoke of the X action's callback,
// vtable 0x451b130, used only here). The label is swapped for ours (0x112b4e0 keeps the reference it
// is given) and the invoke opens the music folder instead.
std::wstring music_dir;
using SetPrompt = void (*)(void*, int, void**);
SetPrompt set_prompt;

const char* game_language() {
    static char code[4];
    unsigned char* tables = *reinterpret_cast<unsigned char**>(image + kStringTables);
    if (!tables || *reinterpret_cast<void**>(tables) != image + kStringTablesVtable) return "";
    size_t length, capacity;
    std::memcpy(&length, tables + 0x30 + 16, sizeof length);
    std::memcpy(&capacity, tables + 0x30 + 24, sizeof capacity);
    if (capacity != 15 || length < 2 || length > 3) return "";
    std::memcpy(code, tables + 0x30, length);
    code[length] = 0;
    return code;
}

const wchar_t* open_folder_label() {
    static const struct { const char* code; const wchar_t* text; } labels[] = {
        {"DE", L"Musikordner öffnen"},      {"FR", L"Ouvrir le dossier Musique"},
        {"IT", L"Apri cartella Musica"},    {"ES", L"Abrir carpeta de música"},
        {"MX", L"Abrir carpeta de música"}, {"BR", L"Abrir pasta de músicas"},
        {"NL", L"Muziekmap openen"},        {"PL", L"Otwórz folder z muzyką"},
        {"CZ", L"Otevřít složku s hudbou"}, {"HU", L"Zenemappa megnyitása"},
        {"RU", L"Открыть папку с музыкой"}, {"JP", L"ミュージック フォルダーを開く"},
        {"KO", L"음악 폴더 열기"},          {"CHS", L"打开音乐文件夹"},
        {"CHT", L"開啟音樂資料夾"},
    };
    const char* code = game_language();
    for (const auto& l : labels)
        if (!std::strcmp(code, l.code)) return l.text;
    return L"Open Music Folder";
}

void* make_ui_string(const wchar_t* text) {
    void* string = nullptr;
    reinterpret_cast<void** (*)(void**)>(image + kCreateUiString)(&string);
    unsigned char* s = static_cast<unsigned char*>(string);
    if (!s) return nullptr;
    size_t length = std::wcslen(text);
    auto copy = static_cast<wchar_t*>(reinterpret_cast<void* (*)(size_t)>(image + kAllocate)((length + 1) * 2));
    if (copy) {
        std::memcpy(copy, text, (length + 1) * sizeof(wchar_t));
        *reinterpret_cast<wchar_t**>(s + 0x28) = copy;
        *reinterpret_cast<uint32_t*>(s + 0x30) = (uint32_t)length;
    }
    return s;
}

void release_ui_string(void* string) {
    auto s = static_cast<unsigned char*>(string);
    if (s && --*reinterpret_cast<int*>(s + 0x10) == 0)
        reinterpret_cast<void (*)(void*)>(image + kFreeUiString)(s);
}

unsigned char open_music_folder(void*) {
    CreateDirectoryW(music_dir.c_str(), nullptr);  // a configured folder that isn't there yet
    auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", music_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    logf(r > 32 ? "Groove Music Radio: opened %s." : "Groove Music Radio: can't open %s.",
         narrow(music_dir).c_str());
    return 1;
}

void audio_prompt(void* help_bar, int button, void** label) {
    if (void* text = make_ui_string(open_folder_label())) {
        release_ui_string(*label);
        *label = text;
    }
    set_prompt(help_bar, button, label);
}

HRESULT get_permissions(uint8_t* result, void** task) { return start({Kind::Permissions, result, L"", 0, 0}, task); }
HRESULT get_playlists(uint8_t* result, void** task) {
    return start({Kind::Playlists, result, L"", 0, library_rescan()}, task);
}
HRESULT more_playlists(const wchar_t* token, uint8_t* result, void** task) {
    return start({Kind::Playlists, result, L"", token_offset(token), 1}, task);
}
std::mutex playing_lock;
std::wstring playing_list;  // the playlist the game asked tracks for last

void set_playing_list(const wchar_t* id) {
    std::lock_guard<std::mutex> lock(playing_lock);
    playing_list = id ? id : L"";
}

std::wstring current_list() {
    std::lock_guard<std::mutex> lock(playing_lock);
    return playing_list;
}

HRESULT get_tracks(const wchar_t* id, uint8_t* result, void** task) {
    set_playing_list(id);
    return start({Kind::Tracks, result, id ? id : L"", 0, 1}, task);
}
HRESULT get_tracks_shuffled(const wchar_t* id, const wchar_t* seed, uint8_t* result, void** task) {
    set_playing_list(id);
    Request r{Kind::Tracks, result, id ? id : L"", 0, 1};
    if (shuffle_enabled) {
        r.shuffled = true;
        // No seed from the game: a new order each time.
        r.seed = seed && *seed ? seed_of(seed) : (uint32_t)GetTickCount64();
        logf("Groove Music Radio: shuffling \"%s\" (seed \"%s\").", narrow(r.id).c_str(),
             seed ? narrow(seed).c_str() : "");
    }
    return start(std::move(r), task);
}
HRESULT more_tracks(const wchar_t* id, const wchar_t* token, uint8_t* result, void** task) {
    Request r{Kind::Tracks, result, id ? id : L"", token_offset(token), 1};
    r.shuffled = token_seed(token, r.seed);
    return start(std::move(r), task);
}
HRESULT get_song_url(const wchar_t* id, const wchar_t*, const wchar_t*, uint8_t* result, void** task) {
    return start({Kind::SongUrl, result, id ? id : L"", 0, 1}, task);
}

// ---- ANNA: the Groove Music menu ---------------------------------------------------------------------
// Listed while the radio plays the Groove station: the radio module (0x881250 on the module manager
// [0x4d85c00]) keeps the station's name at [[radio + 0x60] + 0x38], "Xbox Music" for Groove.
// A playlist is chosen as Audio Options does it (0x13c29a0): the Main/XboxMusic setting is saved
// (0x1a2ca70 makes the value, 0x33b570 stores it in 0x11608a0's settings) and the player gets the
// playlist (0x1947720(player, &id, shuffle), started by 0x19459d0).
// The player's queue ([player + 0x6660]) holds its tracks at +0x80 (16-byte entries; an entry's state
// at +0x118: 0 needs its URL, 1 asked, 2 ready, 3 playing) and the current one at +0x198. A song whose
// stream ends moves it on by one (0x1945940), which resets the entry it leaves to 0. Next Song ends the
// file playing; Previous Song first parks the queue on the entry before the previous song (state 1, so
// the player leaves it alone), so that step lands on the previous song.
constexpr size_t kQueueTracks = 0x80, kQueueCurrent = 0x198, kEntryState = 0x118;

struct GameWString {
    union {
        wchar_t text[8];
        const wchar_t* pointer;
    };
    size_t size, capacity;
};
struct GameString {
    union {
        char text[16];
        const char* pointer;
    };
    size_t size, capacity;
};

// A std::wstring the game owns, made by its own assign (0x2bb1f0): the player's 0x1947720 takes the
// string it is given and frees its buffer with the game's allocator.
GameWString game_wstring(const std::wstring& value) {
    GameWString w{};
    w.capacity = 7;
    reinterpret_cast<void* (*)(void*, const wchar_t*, size_t)>(image + 0x2bb1f0)(&w, value.c_str(), value.size());
    return w;
}

void free_game_wstring(GameWString& w) {
    if (w.capacity >= 8)
        reinterpret_cast<void (*)(void*, const wchar_t*, size_t)>(image + 0x1bb6e70)(&w, w.pointer, w.capacity + 1);
    w = GameWString{};
    w.capacity = 7;
}

bool groove_station_on() {
    void* manager = *reinterpret_cast<void**>(image + 0x4d85c00);
    if (!manager) return false;
    struct {
        unsigned char* object;
        unsigned char* control;
    } radio{};
    reinterpret_cast<void* (*)(void*, void*)>(image + 0x881250)(manager, &radio);
    bool on = false;
    if (radio.object) {
        auto settings = *reinterpret_cast<unsigned char**>(radio.object + 0x60);
        on = settings && string_is(settings + 0x38, "Xbox Music");
    }
    if (radio.control && InterlockedDecrement(reinterpret_cast<volatile LONG*>(radio.control + 8)) == 0) {
        (*reinterpret_cast<void (***)(void*)>(radio.control))[0](radio.control);
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(radio.control + 12)) == 0)
            (*reinterpret_cast<void (***)(void*)>(radio.control))[1](radio.control);
    }
    return on;
}

// The playlists in Audio Options order: the music apps listed, then the music folder's.
std::vector<Playlist> anna_playlists() {
    std::vector<Playlist> lists;
    for (LivePlayer* p : live_players())
        if (p->listed()) lists.push_back({p->id(), p->name(), {}});
    for (Playlist& p : library_playlists()) lists.push_back(std::move(p));
    return lists;
}

unsigned char* track_queue() {
    unsigned char* player = *reinterpret_cast<unsigned char**>(image + kPlayer);
    return player ? *reinterpret_cast<unsigned char**>(player + 0x6660) : nullptr;
}

void set_entry_state(unsigned char* queue, int index, uint32_t state) {
    unsigned char* entry = *reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(queue + kQueueTracks) + 16 * index);
    if (entry) *reinterpret_cast<uint32_t*>(entry + kEntryState) = state;
}

void switch_playlist(const Playlist& list) {
    GameWString id = game_wstring(list.id);
    alignas(16) unsigned char value[64] = {};
    reinterpret_cast<void* (*)(void*, const void*)>(image + 0x1a2ca70)(value, &id);  // copies id
    GameString name{};
    std::memcpy(name.text, "Main/XboxMusic", 15);
    name.size = 14;
    name.capacity = 15;
    if (void* settings = reinterpret_cast<void* (*)()>(image + 0x11608a0)())
        reinterpret_cast<void (*)(void*, const void*, void*)>(image + 0x33b570)(settings, &name, value);
    reinterpret_cast<void (*)(void*)>(image + 0x1a2dff0)(value);
    unsigned char* player = *reinterpret_cast<unsigned char**>(image + kPlayer);
    if (!player) return free_game_wstring(id);
    unsigned char* queue = track_queue();
    unsigned char shuffle = queue ? queue[0xd8] : 1;
    reinterpret_cast<void (*)(void*, GameWString*, unsigned char)>(image + 0x1947720)(player, &id, shuffle);  // takes id
    free_game_wstring(id);  // emptied by the call already; kept for safety
    reinterpret_cast<void (*)(void*)>(image + 0x19459d0)(player);
    logf("Groove Music Radio: ANNA chose the playlist %s.", narrow(list.name).c_str());
}

void next_song() {
    if (LivePlayer* p = live_player(current_list())) {
        logf("Groove Music Radio: ANNA: next song on %s.", narrow(p->name()).c_str());
        return p->next();
    }
    host->log_utf8("Groove Music Radio: ANNA: next song.");
    skip_generation++;
}

void previous_song() {
    if (LivePlayer* p = live_player(current_list())) return p->previous();
    unsigned char* queue = track_queue();
    if (!queue) return;
    auto first = *reinterpret_cast<unsigned char**>(queue + kQueueTracks);
    auto last = *reinterpret_cast<unsigned char**>(queue + kQueueTracks + 8);
    int count = first ? (int)((last - first) / 16) : 0;
    int at = *reinterpret_cast<int*>(queue + kQueueCurrent);
    logf("Groove Music Radio: ANNA: previous song (queue at %d of %d).", at, count);
    if (at < 1 || at >= count) return;  // the first song has none before it
    if (at == 1) {
        // No entry before the first song to park on: swap the first two, so the step from the entry
        // playing (now first) lands on the first song (now second).
        unsigned char swap[16];
        std::memcpy(swap, first, 16);
        std::memcpy(first, first + 16, 16);
        std::memcpy(first + 16, swap, 16);
        *reinterpret_cast<int*>(queue + kQueueCurrent) = 0;
        set_entry_state(queue, 1, 0);
    } else {
        set_entry_state(queue, at, 0);       // the song playing, for when the queue reaches it again
        set_entry_state(queue, at - 1, 0);   // the song to play
        set_entry_state(queue, at - 2, 1);   // parked on: left alone until the step
        *reinterpret_cast<int*>(queue + kQueueCurrent) = at - 2;
    }
    skip_generation++;
}

const wchar_t* anna_text(int which) {
    // 0: "Playlist: ", 1: Previous Song, 2: Next Song
    static const struct {
        const char* code;
        const wchar_t* text[3];
    } texts[] = {
        {"DE", {L"Playlist: ", L"Vorheriger Titel", L"N\u00e4chster Titel"}},
        {"FR", {L"Playlist : ", L"Titre pr\u00e9c\u00e9dent", L"Titre suivant"}},
        {"IT", {L"Playlist: ", L"Brano precedente", L"Brano successivo"}},
        {"ES", {L"Lista: ", L"Canci\u00f3n anterior", L"Canci\u00f3n siguiente"}},
        {"MX", {L"Lista: ", L"Canci\u00f3n anterior", L"Canci\u00f3n siguiente"}},
        {"BR", {L"Playlist: ", L"M\u00fasica anterior", L"Pr\u00f3xima m\u00fasica"}},
        {"NL", {L"Afspeellijst: ", L"Vorig nummer", L"Volgend nummer"}},
        {"PL", {L"Playlista: ", L"Poprzedni utw\u00f3r", L"Nast\u0119pny utw\u00f3r"}},
        {"CZ", {L"Playlist: ", L"P\u0159edchoz\u00ed skladba", L"Dal\u0161\u00ed skladba"}},
        {"HU", {L"Lej\u00e1tsz\u00e1si lista: ", L"El\u0151z\u0151 sz\u00e1m", L"K\u00f6vetkez\u0151 sz\u00e1m"}},
        {"RU", {L"\u041f\u043b\u0435\u0439\u043b\u0438\u0441\u0442: ",
                L"\u041f\u0440\u0435\u0434\u044b\u0434\u0443\u0449\u0430\u044f \u043f\u0435\u0441\u043d\u044f",
                L"\u0421\u043b\u0435\u0434\u0443\u044e\u0449\u0430\u044f \u043f\u0435\u0441\u043d\u044f"}},
        {"JP", {L"\u30d7\u30ec\u30a4\u30ea\u30b9\u30c8: ", L"\u524d\u306e\u66f2", L"\u6b21\u306e\u66f2"}},
        {"KO", {L"\uc7ac\uc0dd \ubaa9\ub85d: ", L"\uc774\uc804 \uace1", L"\ub2e4\uc74c \uace1"}},
        {"CHS", {L"\u64ad\u653e\u5217\u8868\uff1a", L"\u4e0a\u4e00\u9996", L"\u4e0b\u4e00\u9996"}},
        {"CHT", {L"\u64ad\u653e\u6e05\u55ae\uff1a", L"\u4e0a\u4e00\u9996", L"\u4e0b\u4e00\u9996"}},
    };
    static const wchar_t* const english[3] = {L"Playlist: ", L"Previous Song", L"Next Song"};
    const char* code = game_language();
    for (const auto& t : texts)
        if (!std::strcmp(code, t.code)) return t.text[which];
    return english[which];
}

// The menu's entries (fh3anna.h): Groove Music > Playlist: <name> > <playlists>, Previous Song, Next Song.
// Every callback runs on the game's thread, while ANNA builds or answers her list.
constexpr int kPlaylistSlots = 24;
std::string anna_label;  // label_now's text: ANNA copies it straight away

const std::vector<Playlist>& anna_lists() {  // the playlists, read again at most twice a second
    static std::vector<Playlist> lists;
    static ULONGLONG read_at;
    ULONGLONG now = GetTickCount64();
    if (!read_at || now - read_at > 500) {
        lists = anna_playlists();
        read_at = now;
    }
    return lists;
}

int groove_shown(void*, const char*) { return groove_station_on(); }

const char* playlist_label(void*, const char*) {
    std::wstring current = current_list(), name = L"-";
    for (const Playlist& list : anna_lists())
        if (list.id == current) name = list.name;
    anna_label = narrow(anna_text(0) + name);
    return anna_label.c_str();
}

int slot_shown(void* user, const char*) { return (size_t)(intptr_t)user < anna_lists().size(); }

const char* slot_label(void* user, const char*) {
    const std::vector<Playlist>& lists = anna_lists();
    size_t at = (size_t)(intptr_t)user;
    if (at >= lists.size()) return "";
    anna_label = narrow((lists[at].id == current_list() ? L"• " : L"") + lists[at].name);
    return anna_label.c_str();
}

void slot_choose(void* user, const char*) {
    std::vector<Playlist> lists = anna_playlists();
    size_t at = (size_t)(intptr_t)user;
    if (at < lists.size()) switch_playlist(lists[at]);
}

const char* text_label(void* user, const char*) {
    anna_label = narrow(anna_text((int)(intptr_t)user));
    return anna_label.c_str();
}

void previous_choose(void*, const char*) { previous_song(); }
void next_choose(void*, const char*) { next_song(); }

bool add_anna_entries(const FH3AnnaApi* anna);

void report_anna(bool added) {
    host->log_utf8(added ? "Groove Music Radio: ANNA lists Groove Music while the radio plays the Groove station."
                         : "Groove Music Radio: couldn't add the Groove Music entries to ANNA's menu.");
}

bool add_anna_entries(const FH3AnnaApi* anna) {
    FH3AnnaEntry groove = {sizeof groove};
    groove.key = "groove";
    groove.label = "Groove Music";
    groove.shown = groove_shown;
    FH3AnnaEntry playlist = {sizeof playlist};
    playlist.key = "groove.playlist";
    playlist.parent = "groove";
    playlist.label = "Playlist";
    playlist.label_now = playlist_label;
    FH3AnnaEntry previous = {sizeof previous};
    previous.key = "groove.previous";
    previous.parent = "groove";
    previous.label = "Previous Song";
    previous.label_now = text_label;
    previous.user = (void*)(intptr_t)1;
    previous.on_choose = previous_choose;
    previous.stays_open = 1;
    FH3AnnaEntry next = previous;
    next.key = "groove.next";
    next.label = "Next Song";
    next.user = (void*)(intptr_t)2;
    next.on_choose = next_choose;
    bool added = anna->add_entry(&groove) && anna->add_entry(&playlist) && anna->add_entry(&previous) &&
                 anna->add_entry(&next);
    static char keys[kPlaylistSlots][32];
    for (int i = 0; added && i < kPlaylistSlots; ++i) {
        std::snprintf(keys[i], sizeof keys[i], "groove.playlist.%d", i);
        FH3AnnaEntry slot = {sizeof slot};
        slot.key = keys[i];
        slot.parent = "groove.playlist";
        slot.label = "-";
        slot.label_now = slot_label;
        slot.shown = slot_shown;
        slot.user = (void*)(intptr_t)i;
        slot.on_choose = slot_choose;
        slot.stays_open = 1;  // the bullet moves to the playlist chosen
        added = anna->add_entry(&slot);
    }
    return added;
}

const char kDefaultIni[] =
    "; Groove Music Radio: your music on the Groove station. Changes apply when the game starts.\r\n"
    "[Music]\r\n"
    "; The folder with your music; each folder in it is a playlist. Empty: your Windows Music folder.\r\n"
    "; Environment variables work: %USERPROFILE%\\Desktop\\Songs\r\n"
    "Folder=\r\n"
    "; 1: titles and artists from the files' tags; 0: from the file names (\"Artist - Title.mp3\").\r\n"
    "ReadTags=1\r\n"
    "; 1: when the game shuffles, a playlist's songs play in a mixed order; 0: always in folder order.\r\n"
    "Shuffle=1\r\n"
    "\r\n"
    "[spotify]\r\n"
    "; Spotify Soloist (Spotify Premium): put soloist.exe in this folder and your Soloist API key below\r\n"
    "; (developer.spotify.com/dashboard > Spotify Soloist API Key; keep it private). \"Spotify\" is then a\r\n"
    "; playlist in Audio Options: choose it, and in the Spotify app play on the device below.\r\n"
    "enabled=1\r\n"
    "api_key=\r\n"
    "device=FH3 Radio\r\n"
    "; Soloist's starting volume (0-100).\r\n"
    "volume=100\r\n"
    "; 1: turn Soloist's own sound down so it's only heard on the radio.\r\n"
    "quiet_speakers=1\r\n"
    "; 1: when the radio switches to Spotify, move Spotify playback to the device above and play.\r\n"
    "auto_activate=1\r\n"
    "\r\n"
    "; Music apps: each is a playlist in Audio Options; choose it and play something in the app. While the radio\r\n"
    "; plays it, the app's own sound is turned down and put back after (quiet_speakers=0: not). app= names its\r\n"
    "; program (\"chrome.exe\"; empty: the usual ones). Apps without Windows media controls are driven by their\r\n"
    "; global shortcuts: set them here as they are in the app (\"Ctrl+Alt+Right\").\r\n"
    "[youtube]\r\n"
    "; music.youtube.com in a browser, or installed from the browser as an app\r\n"
    "enabled=1\r\n"
    "app=\r\n"
    "quiet_speakers=1\r\n"
    "\r\n"
    "[netease]\r\n"
    "; NetEase Cloud Music (cloudmusic.exe)\r\n"
    "enabled=1\r\n"
    "app=\r\n"
    "quiet_speakers=1\r\n"
    "play_pause_key=Ctrl+Alt+P\r\n"
    "next_key=Ctrl+Alt+Right\r\n"
    "\r\n"
    "[qqmusic]\r\n"
    "; QQ Music (QQMusic.exe)\r\n"
    "enabled=1\r\n"
    "app=\r\n"
    "quiet_speakers=1\r\n"
    "play_pause_key=Ctrl+Alt+F5\r\n"
    "next_key=Ctrl+Alt+Right\r\n";

void write_default_ini(const std::wstring& ini) {
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, kDefaultIni, sizeof kDefaultIni - 1, &written, nullptr);
    CloseHandle(f);
}

std::wstring ini_text(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[1024] = L"";
    GetPrivateProfileStringW(section, key, fallback, value, 1024, ini.c_str());
    return value;
}

int ini_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return (int)GetPrivateProfileIntW(section, key, fallback, ini.c_str());
}

void log_line(const char* line) { logf("Groove Music Radio: %s", line); }

void start_live_players(const std::wstring& ini, const std::wstring& folder) {
    if (ini_int(ini, L"spotify", L"enabled", 1)) {
        SpotifyOptions o;
        o.dir = folder;
        o.api_key = ini_text(ini, L"spotify", L"api_key", L"");
        o.device = ini_text(ini, L"spotify", L"device", L"FH3 Radio");
        o.volume = ini_int(ini, L"spotify", L"volume", 100);
        o.quiet_speakers = ini_int(ini, L"spotify", L"quiet_speakers", 1) != 0;
        o.auto_activate = ini_int(ini, L"spotify", L"auto_activate", 1) != 0;
        spotify_start(o, log_line);
    }
    struct App {
        const wchar_t *section, *id, *name;
        std::vector<std::wstring> exes, app_ids;
        bool browser;
        const wchar_t* install_path;
        const char* hint;
    };
    const App apps[] = {
        {L"youtube", L"grm:youtube", L"YouTube Music",
         {L"msedge.exe", L"chrome.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"firefox.exe"},
         {L"youtube", L"cinhimbnkkaeohfgghhklpknlkffjgod"}, true, L"",
         "open music.youtube.com (or the YouTube Music app) and play something"},
        {L"netease", L"grm:netease", L"NetEase Cloud Music", {L"cloudmusic.exe"}, {L"cloudmusic", L"netease"},
         false, L"%ProgramFiles%\\Netease\\CloudMusic\\cloudmusic.exe", "start NetEase Cloud Music and play something"},
        {L"qqmusic", L"grm:qqmusic", L"QQ Music", {L"qqmusic.exe"}, {L"qqmusic"}, false,
         L"%ProgramFiles(x86)%\\Tencent\\QQMusic\\QQMusic.exe", "start QQ Music and play something"},
    };
    bool any_quiet = false;
    for (const App& a : apps) {
        if (!ini_int(ini, a.section, L"enabled", 1)) continue;
        AppOptions o;
        o.id = a.id;
        o.name = a.name;
        o.section = a.section;
        o.exes = a.exes;
        std::wstring app = ini_text(ini, a.section, L"app", L"");
        for (auto& c : app) c = (wchar_t)towlower(c);
        if (!app.empty()) o.exes = {app};
        o.app_ids = a.app_ids;
        o.browser = a.browser;
        o.always_listed = a.browser;
        wchar_t path[MAX_PATH] = L"";
        if (a.install_path[0] && ExpandEnvironmentStringsW(a.install_path, path, MAX_PATH)) o.install_path = path;
        o.play_pause_key = ini_text(ini, a.section, L"play_pause_key", L"");
        o.next_key = ini_text(ini, a.section, L"next_key", L"");
        o.missing_hint = a.hint;
        o.quiet_speakers = ini_int(ini, a.section, L"quiet_speakers", 1) != 0;
        o.ini = ini;
        any_quiet = any_quiet || o.quiet_speakers;
        app_start(o, log_line);
    }
    if (any_quiet) apps_start_restore_watcher(ini, log_line);
}

std::wstring music_folder(const std::wstring& ini) {
    wchar_t configured[MAX_PATH] = L"", expanded[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"Music", L"Folder", L"", configured, MAX_PATH, ini.c_str());
    if (configured[0] && ExpandEnvironmentStringsW(configured, expanded, MAX_PATH)) return expanded;
    PWSTR known = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &known))) folder = known;
    CoTaskMemFree(known);
    return folder;
}

}  // namespace


extern "C" __declspec(dllexport) BOOL WINAPI FH3PluginInitialize(const FH3PluginContext* context) {
    if (!context || context->api_version < 1) return FALSE;  // version 2 only adds the ANNA menu
    host = context;
    image = static_cast<unsigned char*>(context->game_image);
    std::wstring ini = std::wstring(context->mod_directory) + L"\\GrooveMusicRadio.ini";

    static const unsigned char available_original[3] = {0x40, 0x53, 0x48};
    static const unsigned char available_yes[3] = {0xb0, 0x01, 0xc3};  // mov al,1; ret
    if (!host->patch_bytes(kAvailable, available_original, available_yes, 3) || !detour_lock()) {
        host->log_utf8("Groove Music Radio: game code differs; station not enabled.");
        return TRUE;
    }
    struct Hook { uint32_t site; void* callback; };
    const Hook hooks[] = {
        {kStatusCall, (void*)&status},
        {kCallPermissions, (void*)&get_permissions},
        {kCallPlaylists, (void*)&get_playlists},
        {kCallMorePlaylists, (void*)&more_playlists},
        {kCallTracks, (void*)&get_tracks},
        {kCallMoreTracks, (void*)&more_tracks},
        {kCallMoreTracksShuffle, (void*)&more_tracks},
        {kCallTracksShuffle, (void*)&get_tracks_shuffled},
        {kCallSongUrl, (void*)&get_song_url},
        {kPrivilegeAudioOptions, (void*)&music_privilege},
        {kPrivilegePlayer, (void*)&music_privilege},
    };
    for (const Hook& h : hooks)
        if (!host->hook_call(h.site, h.callback)) {
            logf("Groove Music Radio: couldn't hook 0x%x; station on, no music.", h.site);
            return TRUE;
        }
    open_reader = (OpenReader)host->hook_call(kCallOpenReader, (void*)&open_song);
    if (!open_reader) {
        host->log_utf8("Groove Music Radio: couldn't hook the song player; station on, no music.");
        return TRUE;
    }
    write_default_ini(ini);
    LibraryOptions options;
    options.folder = music_folder(ini);
    options.read_tags = ini_int(ini, L"Music", L"ReadTags", 1) != 0;
    shuffle_enabled = ini_int(ini, L"Music", L"Shuffle", 1) != 0;
    library_start(options, log_line);
    music_dir = options.folder;
    if (!music_dir.empty()) {
        if (host->hook_pointer(kAudioLaunchInvoke, (void*)&open_music_folder))
            set_prompt = (SetPrompt)host->hook_call(kCallAudioPrompt, (void*)&audio_prompt);
        if (!set_prompt) host->log_utf8("Groove Music Radio: couldn't change Audio Options' X button.");
    }
    start_live_players(ini, context->mod_directory);
    // ANNA's menu. Plugins load in name order, so ANNA is usually there already; a loadorder.txt may put
    // her after this plugin, so then she is waited for on a thread of its own (add_entry takes any thread).
    if (const FH3AnnaApi* anna = fh3anna_get(1)) {
        report_anna(add_anna_entries(anna));
    } else if (HANDLE waiter = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                   const FH3AnnaApi* anna = fh3anna_wait(1, 60000);
                   if (anna) report_anna(add_anna_entries(anna));
                   else host->log_utf8("Groove Music Radio: no ANNA plugin; no Groove Music menu in ANNA.");
                   return 0;
               }, nullptr, 0, nullptr)) {
        CloseHandle(waiter);
    }
    logf("Groove Music Radio " GROOVE_MUSIC_RADIO_VERSION ": Groove station on; music from %s.",
         narrow(options.folder).c_str());
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
