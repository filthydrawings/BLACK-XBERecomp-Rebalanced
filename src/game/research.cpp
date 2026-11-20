// Research logger, temporary: finds the guest fields that hold a weapon's clip size, range and damage and a
// character's health. Appends to logs\research.log beside the exe, a line === launch at each start.
//   F7: the weapon object and its data block, as words
//   F8: every weapon record beside the one in hand, and every character type with its hit zone shares
//   each change of the weapon in hand: its record
//   F6: the player, the weapon and its data, the words that changed since the last F6
//   the player's health as it starts: its health, the +0x3D0 it came from and the difficulty
//   each hit that lowers a character's health, at the stores of the damage functions: before, after, difficulty
// On screen, under the FPS counter: WEAPON LOGGING: ON, a note for a few seconds after F6 F7 F8, the player's
// health, the difficulty and the last character other than the player that was hit, its health over its health
// before its first hit.
//   F9: the next spread, recoil or other candidate field of the record in hand, times 5 (the previous one
//   restored first, all restored when the weapon in hand changes); the candidates are logged once
//   each reload: the frames from the clip's last drop to its refill, and the words that counted meanwhile
// Nothing in the game is changed, but F9's candidate.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "game/layout.h"
#include "game/research.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

// bytes read from each object, past the fields known so far
constexpr uint32_t kPlayerBytes = 0x1000;
constexpr uint32_t kWeaponBytes = 0x400;
constexpr uint32_t kDataBytes = 0x400;
constexpr int kDiffKey = VK_F6;
constexpr int kDumpKey = VK_F7;
constexpr int kTablesKey = VK_F8;

// character
constexpr uint32_t kCharacterHealth = 0x2F0;     // float
constexpr uint32_t kCharacterHealthFrom = 0x3D0;  // float, copied to the health as a character starts
constexpr size_t kMostCharacters = 512;

constexpr int kZonesLogged = 16;  // entries written the first time a table is seen
std::vector<uint32_t> g_zone_tables;

// the note a research key leaves on screen for a few seconds
constexpr auto kNoteTime = std::chrono::seconds(3);
const char* g_note;
std::chrono::steady_clock::time_point g_note_until;
void Note(const char* text) {
    g_note = text;
    g_note_until = std::chrono::steady_clock::now() + kNoteTime;
}

// character types sit in one array, by id
constexpr uint32_t kMostTypes = 128;
uint32_t g_type_base;  // type 0, from the last type a hit saw

// weapon records sit in one array; every record seen so far has these
constexpr uint32_t kMostRecords = 64;
bool IsWeaponRecord(uint32_t r) {
    return r && R32(r + 4) == 1 && G<float>(r + 0xC) == 0.5f && G<float>(r + 0x1C) == 1.0f;
}

// characters that were hit, with their health before the first hit
struct Character {
    uint32_t va;
    float first;
};
std::vector<Character> g_characters;
size_t g_last_hit = SIZE_MAX;  // the index of the last one hit other than the player

FILE* Log() {
    static FILE* file = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exe).parent_path() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        FILE* f = _wfopen((dir / "research.log").c_str(), L"a");
        if (f) std::fprintf(f, "\n=== launch\n");
        return f;
    }();
    return file;
}

// a key that went down since the last call, while the game window is in front
bool Pressed(int vk) {
    static bool held[256];
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
    bool pressed = down && !held[vk];
    held[vk] = down;
    return pressed;
}

float AsFloat(uint32_t word) {
    float f;
    std::memcpy(&f, &word, 4);
    return f;
}

// a word as hex, as a whole number and as a float
void Word(FILE* f, uint32_t offset, uint32_t word) {
    std::fprintf(f, "  +%03X  %08X  %11d  %g\n", offset, word, int32_t(word), AsFloat(word));
}

std::vector<uint32_t> Read(uint32_t va, uint32_t bytes) {
    std::vector<uint32_t> words(bytes / 4);
    if (va) std::memcpy(words.data(), HostPtr(va), bytes);
    return words;
}

void Dump(FILE* f, const char* name, uint32_t va, uint32_t bytes) {
    std::fprintf(f, "%s at %08X\n", name, va);
    if (!va) return;
    std::vector<uint32_t> words = Read(va, bytes);
    for (uint32_t i = 0; i < words.size(); i++) Word(f, i * 4, words[i]);
}

// a snapshot of one object: what F6 compares against
struct Snapshot {
    const char* name;
    uint32_t va = 0;
    std::vector<uint32_t> words;
};

void Diff(FILE* f, Snapshot& s, uint32_t va, uint32_t bytes) {
    std::vector<uint32_t> now = Read(va, bytes);
    if (s.va != va || s.words.empty()) {
        std::fprintf(f, "%s at %08X: first snapshot\n", s.name, va);
    } else {
        std::fprintf(f, "%s at %08X: changed words, old then new\n", s.name, va);
        for (uint32_t i = 0; i < now.size(); i++) {
            if (now[i] == s.words[i]) continue;
            std::fprintf(f, "  +%03X  %11d -> %11d   %g -> %g\n", i * 4, int32_t(s.words[i]), int32_t(now[i]),
                         AsFloat(s.words[i]), AsFloat(now[i]));
        }
    }
    s.va = va;
    s.words = std::move(now);
}

uint32_t Difficulty() { return R32(R32(kGame) + kGameDifficulty); }

// a store of a damage function about to write a character's health: logged, and the character kept for the overlay
void Hit(const char* site, uint32_t character, float after) {
    float before = G<float>(character + kCharacterHealth);
    bool player = character == Player();
    auto known = std::find_if(g_characters.begin(), g_characters.end(),
                              [&](const Character& c) { return c.va == character; });
    bool first = known == g_characters.end();
    if (first && g_characters.size() < kMostCharacters) {
        g_characters.push_back({character, before});
        known = g_characters.end() - 1;
    }
    if (!player && known != g_characters.end()) g_last_hit = size_t(known - g_characters.begin());
    if (FILE* f = Log()) {
        std::fprintf(f, "hit %s: character %08X %g -> %g (%g), difficulty %u%s%s\n", site, character, before, after,
                     before - after, Difficulty(), first ? ", first hit" : "", player ? ", the player" : "");
        std::fflush(f);
    }
}

// a zone table, the first time it is seen: each entry's two shares and flags
void LogZones(FILE* f, uint32_t table) {
    if (!table || std::find(g_zone_tables.begin(), g_zone_tables.end(), table) != g_zone_tables.end()) return;
    g_zone_tables.push_back(table);
    std::fprintf(f, "zone table %08X:\n", table);
    for (int zone = 0; zone < kZonesLogged; zone++) {
        uint32_t entry = table + zone * kHitsEntrySize;
        std::fprintf(f, "  zone %2d  front %g  back %g  flags %u %u\n", zone, G<float>(entry), G<float>(entry + 4),
                     G<uint8_t>(entry + 8), G<uint8_t>(entry + 9));
    }
}

// every record of the weapon array around one: its known fields, then all its words in hex
void LogWeaponTable(FILE* f, uint32_t data) {
    if (!IsWeaponRecord(data)) {
        std::fprintf(f, "weapon table: the record in hand %08X does not look like one\n", data);
        return;
    }
    uint32_t first = data;
    for (uint32_t left = kMostRecords; left && IsWeaponRecord(first - kWeaponRecordSize); left--)
        first -= kWeaponRecordSize;
    std::fprintf(f, "weapon table from %08X, in hand %08X:\n", first, data);
    uint32_t r = first;
    for (uint32_t i = 0; i < kMostRecords && IsWeaponRecord(r); i++, r += kWeaponRecordSize) {
        std::fprintf(f, "record %2u %08X%s: clip %d ammo type %d  +14 %g +18 %g +44 %g +48 %g +68 %g  +160 %d +164 %d +168 %d\n",
                     i, r, r == data ? " (in hand)" : "", int32_t(R32(r + kDataClip)), int32_t(R32(r + 0x64)),
                     G<float>(r + kDataRange), G<float>(r + 0x18), G<float>(r + 0x44), G<float>(r + 0x48),
                     G<float>(r + kDataRecoil),
                     int32_t(R32(r + 0x160)), int32_t(R32(r + 0x164)), int32_t(R32(r + 0x168)));
        std::fprintf(f, "  words");
        for (uint32_t at = 0; at < kWeaponRecordSize; at += 4) std::fprintf(f, " %08X", R32(r + at));
        std::fprintf(f, "\n");
    }
}

// every character type by id from the one a hit saw: its zone shares, front/back, and kill flags
void LogTypes(FILE* f) {
    if (!g_type_base) {
        std::fprintf(f, "character types: hit an enemy first\n");
        return;
    }
    std::fprintf(f, "character types from %08X:\n", g_type_base);
    for (uint32_t id = 0; id < kMostTypes; id++) {
        uint32_t type = g_type_base + id * kTypeSize;
        if (R32(type + kTypeId) != id) continue;
        uint32_t zones = R32(type + kTypeHits);
        uint32_t table = zones ? R32(zones) : 0;
        std::fprintf(f, "type %3u %08X zones %08X:", id, type, table);
        for (int zone = 0; table && zone < kZonesLogged; zone++) {
            uint32_t entry = table + zone * kHitsEntrySize;
            std::fprintf(f, " %d=%g/%g%s%s", zone, G<float>(entry), G<float>(entry + 4), G<uint8_t>(entry + 8) ? "k" : "",
                         G<uint8_t>(entry + 9) ? "K" : "");
        }
        std::fprintf(f, "\n");
    }
}

// F9: one candidate field of the weapon record in hand at a time, times 5, to see what it changes in play. The
// candidates: the spread and recoil ones seen so far, then every other float between 0.001 and 50 that differs
// between the uzi's record and the sniper's. A record is shared by every weapon of its kind, enemies' too.
constexpr int kExaggerateKey = VK_F9;
constexpr double kExaggerate = 5.0;
constexpr uint32_t kUziRecord = 3;
constexpr uint32_t kSniperRecord = 2;
constexpr uint32_t kFixedCandidates[] = {0x84, 0x88, 0xA4, 0x16C, 0x170, 0x174};
constexpr float kLeastCandidate = 0.001f;
constexpr float kMostCandidate = 50.0f;

struct Candidate {
    uint32_t offset;
    bool is_float;
};
std::vector<Candidate> g_candidates;  // [0] is off; found once the records are mapped
size_t g_exaggerated;                 // the candidate applied, 0 none
uint32_t g_exaggerated_record;        // the record it is applied to
uint32_t g_original;                  // its word before
std::string g_exaggerate_line = "EXAGGERATE OFF";

// a word that reads as an ordinary float, not a small whole number or a pointer
bool LooksFloat(uint32_t word) {
    float v = std::fabs(AsFloat(word));
    return v >= 1e-6f && v <= 1e7f;
}

bool Candidates(float v) {
    v = std::fabs(v);
    return v >= kLeastCandidate && v <= kMostCandidate;
}

// the name of the record at data, from its place in the array
const char* RecordName(uint32_t data) {
    if (data < kWeaponRecordsAt) return "?";
    uint32_t at = data - kWeaponRecordsAt;
    if (at % kWeaponRecordSize || at / kWeaponRecordSize >= kWeaponRecords) return "?";
    return kWeaponNames[at / kWeaponRecordSize];
}

// a word as the candidate reads it
double CandidateValue(const Candidate& c, uint32_t word) { return c.is_float ? AsFloat(word) : int32_t(word); }

void FindCandidates(FILE* f) {
    uint32_t uzi = WeaponRecord(kUziRecord);
    uint32_t sniper = WeaponRecord(kSniperRecord);
    g_candidates = {{0, false}};
    for (uint32_t offset : kFixedCandidates)
        g_candidates.push_back({offset, LooksFloat(R32(uzi + offset)) || LooksFloat(R32(sniper + offset))});
    for (uint32_t offset = 0; offset < kWeaponRecordSize; offset += 4) {
        uint32_t a = R32(uzi + offset);
        uint32_t b = R32(sniper + offset);
        if (std::find(std::begin(kFixedCandidates), std::end(kFixedCandidates), offset) != std::end(kFixedCandidates))
            continue;
        if (a == b || !LooksFloat(a) || !LooksFloat(b) || !Candidates(AsFloat(a)) || !Candidates(AsFloat(b))) continue;
        g_candidates.push_back({offset, true});
    }
    std::fprintf(f, "exaggerate candidates (F9), %zu, the record's value in the uzi and the sniper:\n",
                 g_candidates.size() - 1);
    for (size_t i = 1; i < g_candidates.size(); i++) {
        const Candidate& c = g_candidates[i];
        std::fprintf(f, "  %2zu  +%03X  %-5s  uzi %g  sniper %g\n", i, c.offset, c.is_float ? "float" : "int",
                     CandidateValue(c, R32(uzi + c.offset)), CandidateValue(c, R32(sniper + c.offset)));
    }
}

// the candidate applied gets its word back
void RestoreExaggerated(FILE* f) {
    if (g_exaggerated && g_exaggerated_record) {
        const Candidate& c = g_candidates[g_exaggerated];
        uint32_t at = g_exaggerated_record + c.offset;
        std::fprintf(f, "exaggerate: +%03X of %08X (%s) restored, %g -> %g\n", c.offset, g_exaggerated_record,
                     RecordName(g_exaggerated_record), CandidateValue(c, R32(at)), CandidateValue(c, g_original));
        W32(at, g_original);
    }
    g_exaggerated = 0;
    g_exaggerated_record = 0;
    g_exaggerate_line = "EXAGGERATE OFF";
}

// the next candidate on the record in hand, after the last one is restored
void ExaggerateNext(FILE* f, uint32_t data, uint32_t frame) {
    size_t next = g_exaggerated + 1 < g_candidates.size() ? g_exaggerated + 1 : 0;
    RestoreExaggerated(f);
    if (!next || !data) {
        std::fprintf(f, "\n=== frame %u: F9, exaggerate off\n", frame);
        return;
    }
    const Candidate& c = g_candidates[next];
    uint32_t at = data + c.offset;
    g_original = R32(at);
    if (c.is_float) G<float>(at) = float(AsFloat(g_original) * kExaggerate);
    else W32(at, uint32_t(int32_t(int32_t(g_original) * kExaggerate)));
    g_exaggerated = next;
    g_exaggerated_record = data;
    char line[64];
    std::snprintf(line, sizeof line, "EXAGGERATE +0x%03X x%g (%zu/%zu) %s", c.offset, kExaggerate, next,
                  g_candidates.size() - 1, RecordName(data));
    g_exaggerate_line = line;
    std::fprintf(f, "\n=== frame %u: F9, %s: record %08X +%03X %g -> %g\n", frame, line, data, c.offset,
                 CandidateValue(c, g_original), CandidateValue(c, R32(at)));
}

// Reloads: the weapon object and the player each frame, the last kHistory frames kept. A word of the weapon object
// seen dropping by one, as a shot does, that holds still and then rises to at most the clip is the clip refilling:
// logged with the frames since its last drop and the words that counted between that drop and the refill.
constexpr size_t kHistory = 600;
constexpr uint32_t kStillFrames = 10;  // the clip's word holds still this long before a refill
constexpr size_t kMostCounters = 40;   // logged a side
struct Frame {
    uint32_t frame = 0;
    std::chrono::steady_clock::time_point time;
    std::vector<uint32_t> weapon;
    std::vector<uint32_t> player;
};
std::vector<Frame> g_frames(kHistory);  // a ring
size_t g_frames_kept;                   // frames in it, to kHistory
size_t g_frame_next;                    // where the next goes
uint32_t g_tracked;                     // the weapon object they are of
std::vector<uint8_t> g_drops;           // per weapon word: seen to drop by one
std::vector<uint32_t> g_dropped_at;     // per weapon word: the frame it last dropped
std::vector<uint32_t> g_still;          // per weapon word: frames since it changed

// the frame kept that many frames back, 0 the newest
const Frame& Kept(size_t back) { return g_frames[(g_frame_next + kHistory - 1 - back) % kHistory]; }

// the words of one side that changed in at least half of the last since frames
void LogCounters(FILE* f, const char* side, std::vector<uint32_t> Frame::*words, size_t since) {
    if (!since) return;
    size_t count = (Kept(0).*words).size();
    size_t logged = 0;
    for (size_t i = 0; i < count && logged < kMostCounters; i++) {
        size_t changes = 0;
        size_t before = 0;  // changes in the frames kept before those
        bool up = false;
        bool down = false;
        size_t first = 0;
        for (size_t back = 0; back + 1 < g_frames_kept; back++) {
            uint32_t now = (Kept(back).*words)[i];
            uint32_t was = (Kept(back + 1).*words)[i];
            if (now == was) continue;
            if (back >= since) {
                before++;
                continue;
            }
            changes++;
            first = back + 1;
            bool rise = LooksFloat(now) || LooksFloat(was) ? AsFloat(now) > AsFloat(was) : int32_t(now) > int32_t(was);
            (rise ? up : down) = true;
        }
        if (changes * 2 < since) continue;
        uint32_t start = (Kept(since).*words)[i];
        uint32_t end = (Kept(0).*words)[i];
        std::fprintf(f, "  %s +%03X  changed in %zu of %zu frames, from %zu before the refill%s%s, %d -> %d  %g -> %g\n",
                     side, uint32_t(i * 4), changes, since, first, up && down ? ", up and down" : up ? ", up" : ", down",
                     before ? "" : ", still before", int32_t(start), int32_t(end), AsFloat(start), AsFloat(end));
        logged++;
    }
}

void LogRefill(FILE* f, uint32_t data, size_t word, int32_t was, int32_t now, uint32_t frame) {
    uint32_t since_drop = frame - g_dropped_at[word];
    size_t since = std::min<size_t>(since_drop, g_frames_kept - 1);
    double seconds = std::chrono::duration<double>(Kept(0).time - Kept(since).time).count();
    std::fprintf(f,
                 "\n=== frame %u: reload, weapon %08X (%s) +%03X %d -> %d, clip %d: %u frames since its last drop "
                 "(%zu kept, %.2f s)\n",
                 frame, g_tracked, RecordName(data), uint32_t(word * 4), was, now, int32_t(R32(data + kDataClip)),
                 since_drop, since, seconds);
    LogCounters(f, "weapon", &Frame::weapon, since);
    LogCounters(f, "player", &Frame::player, since);
}

void TrackReloads(FILE* f, uint32_t weapon, uint32_t data, uint32_t frame) {
    if (weapon != g_tracked) {
        g_tracked = weapon;
        g_frames_kept = 0;
        g_drops.assign(kWeaponBytes / 4, 0);
        g_dropped_at.assign(kWeaponBytes / 4, frame);
        g_still.assign(kWeaponBytes / 4, 0);
    }
    if (!weapon || !data) return;
    Frame& slot = g_frames[g_frame_next];
    slot.frame = frame;
    slot.time = std::chrono::steady_clock::now();
    slot.weapon = Read(weapon, kWeaponBytes);
    slot.player = Read(Player(), kPlayerBytes);
    g_frame_next = (g_frame_next + 1) % kHistory;
    g_frames_kept = std::min(g_frames_kept + 1, kHistory);
    if (g_frames_kept < 2) return;
    int32_t clip = int32_t(R32(data + kDataClip));
    const std::vector<uint32_t>& now = Kept(0).weapon;
    const std::vector<uint32_t>& was = Kept(1).weapon;
    for (size_t i = 0; i < now.size(); i++) {
        int32_t a = int32_t(was[i]);
        int32_t b = int32_t(now[i]);
        if (b == a - 1 && b >= 0 && a <= clip) {
            g_drops[i] = 1;
            g_dropped_at[i] = frame;
        }
        if (g_drops[i] && b > a && a >= 0 && b <= clip && g_still[i] >= kStillFrames) LogRefill(f, data, i, a, b, frame);
        g_still[i] = a == b ? g_still[i] + 1 : 0;
    }
}

}  // namespace

// the damage functions' health stores, each before its write: the character's register and the new health's.
// 0x1DFA0F 0x1DFB65 sub_001DF830 and sub_001DFA90; 0x1ED293 0x1ED5B2 0x1ED92E 0x1EDAF0 0x1E7A68 the others; the
// rest clamp a killing hit to 0
#define RESEARCH_HIT(address, base, value) \
    void Hit##address(X86Context& ctx, uint8_t*) { Hit(#address, ctx.base, ctx.xmm[value].f[0]); }
RESEARCH_HIT(1DFA0F, edi, 1)
RESEARCH_HIT(1DFB65, edi, 1)
RESEARCH_HIT(1ED293, esi, 0)
RESEARCH_HIT(1ED5B2, esi, 0)
RESEARCH_HIT(1ED92E, edi, 0)
RESEARCH_HIT(1EDAF0, ebx, 0)
RESEARCH_HIT(1E7A68, ebx, 0)
RESEARCH_HIT(1DF97D, edi, 0)
RESEARCH_HIT(1DFB7D, edi, 2)
RESEARCH_HIT(1ED27C, esi, 1)
RESEARCH_HIT(1ED59A, esi, 2)
RESEARCH_HIT(1ED91D, edi, 1)
RESEARCH_HIT(1EDA94, ebx, 1)
RESEARCH_HIT(1E795B, ebx, 0)
RESEARCH_HIT(1ED7CB, edi, 0)
#undef RESEARCH_HIT

// sub_001E74B0 at 0x1E77F9, after call 0x1CC1A0: xmm0 a hit's damage, ebx the character hit, esi the attacker and
// [ebp+0x18] the zone hit. The damage is 100 times the zone's front or back share in the character type's table
void HitDamage(X86Context& ctx, uint8_t*) {
    FILE* f = Log();
    if (!f) return;
    uint32_t character = ctx.ebx;
    uint32_t type = R32(character + kCharacterType);
    uint32_t table = type ? R32(R32(type + kTypeHits)) : 0;
    if (type) g_type_base = type - R32(type + kTypeId) * kTypeSize;
    uint32_t held = R32(Player() + kPlayerWeapon);
    uint32_t record = held ? R32(held + kWeaponData) : 0;
    int zone = int8_t(G<uint8_t>(ctx.ebp + 0x18));
    uint32_t entry = table + uint32_t(zone) * kHitsEntrySize;
    std::fprintf(f, "damage %g: character %08X type %08X id %u zone %d front %g back %g, attacker %08X%s, weapon %08X, difficulty %u\n",
                 ctx.xmm[0].f[0], character, type, type ? R32(type + kTypeId) : 0, zone,
                 table ? G<float>(entry) : 0.0f, table ? G<float>(entry + 4) : 0.0f, ctx.esi,
                 ctx.esi == Player() ? " (the player)" : "", record, Difficulty());
    LogZones(f, table);
    std::fflush(f);
}

// sub_001DEC70 at 0x1DF229, mov [ebx+0x2F0], ecx: the player's health as it starts, from its +0x3D0
void CharacterHealthStart(X86Context& ctx, uint8_t*) {
    uint32_t character = ctx.ebx;
    if (FILE* f = Log()) {
        std::fprintf(f, "character %08X starts: health %g, +3D0 %g, difficulty %u%s\n", character, AsFloat(ctx.ecx),
                     G<float>(character + kCharacterHealthFrom), Difficulty(),
                     character == Player() ? ", the player" : "");
        std::fflush(f);
    }
}

namespace research {

void OverlayLines(std::vector<std::string>& lines) {
    if (Log()) lines.push_back("WEAPON LOGGING: ON");
    if (g_note && std::chrono::steady_clock::now() < g_note_until) lines.push_back(g_note);
    if (Log()) lines.push_back(g_exaggerate_line);
    if (!R32(kWorld)) return;
    char text[64];
    uint32_t player = Player();
    std::snprintf(text, sizeof text, "DIFFICULTY %u", Difficulty());
    lines.push_back(text);
    std::snprintf(text, sizeof text, "PLAYER %.0f / %.0f", double(G<float>(player + kCharacterHealth)),
                  double(G<float>(player + kCharacterHealthFrom)));
    lines.push_back(text);
    if (g_last_hit < g_characters.size()) {
        const Character& hit = g_characters[g_last_hit];
        std::snprintf(text, sizeof text, "HIT %08X %.0f / %.0f", hit.va, double(G<float>(hit.va + kCharacterHealth)),
                      double(hit.first));
        lines.push_back(text);
    }
}

void Tick() {
    FILE* f = Log();
    if (!f || !R32(kWorld)) return;
    uint32_t player = Player();
    uint32_t weapon = R32(player + kPlayerWeapon);
    uint32_t data = weapon ? R32(weapon + kWeaponData) : 0;
    static uint32_t frame;
    frame++;

    if (Pressed(kDumpKey)) {
        Note("F7: WEAPON WRITTEN");
        std::fprintf(f, "\n=== frame %u: F7, weapon %08X\n", frame, weapon);
        Dump(f, "weapon", weapon, kWeaponBytes);
        Dump(f, "weapon data", data, kDataBytes);
    }

    if (g_candidates.empty() && WeaponRecordsMapped()) FindCandidates(f);
    static uint32_t in_hand;
    if (data != in_hand) {
        RestoreExaggerated(f);
        std::fprintf(f, "weapon in hand: record %08X, clip %d\n", data, data ? int32_t(R32(data + kDataClip)) : 0);
        in_hand = data;
    }
    if (Pressed(kTablesKey)) {
        Note("F8: TABLES WRITTEN");
        std::fprintf(f, "\n=== frame %u: F8\n", frame);
        LogWeaponTable(f, data);
        LogTypes(f);
    }

    if (Pressed(kExaggerateKey) && !g_candidates.empty()) {
        ExaggerateNext(f, data, frame);
        static char note[80];
        std::snprintf(note, sizeof note, "F9: %s", g_exaggerate_line.c_str());
        Note(note);
    }
    TrackReloads(f, weapon, data, frame);

    static Snapshot snapshots[] = {{"player"}, {"weapon"}, {"weapon data"}};
    if (Pressed(kDiffKey)) {
        Note("F6: CHANGES WRITTEN");
        static int count;
        std::fprintf(f, "\n=== F6 #%d, frame %u\n", ++count, frame);
        Diff(f, snapshots[0], player, kPlayerBytes);
        Diff(f, snapshots[1], weapon, kWeaponBytes);
        Diff(f, snapshots[2], data, kDataBytes);
    }
    std::fflush(f);
}

}  // namespace research
