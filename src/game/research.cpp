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
//   each reload, from the weapon's reload state to its magazine refilling: the frames and seconds, and each frame
//   the player's animation and the words of its controller and clip that changed
//   F11: reload animations at twice their speed, on or off, to see whether reloads shorten with them
//   each change of the weapon in hand's state or the player's animation
//   F5 / Shift+F5: the weapon in hand becomes the next / previous weapon of the game, full clip and spare ammo
//   every file the game opens, weapon files in black.log too
//   each stage's pickup list and loaded file slots, in black.log too
//   F12: the weapon in hand's pickup model loaded from another level when the stage lacks it (BorrowPickupModel)
// Nothing in the game is changed, but F9's candidate, F11's reload speed and F5's weapon.
#include <algorithm>
#include <chrono>
#include <cctype>
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

#include "core/log.h"
#include "game/layout.h"
#include "game/rebalance.h"
#include "game/weapon_kinds.h"
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

bool g_reload_speed_on;  // F11, below

// Reloads: from the weapon in hand going to kWeaponReloading to its magazine's rounds rising, each frame the player's
// animation: its name, its clip, and the words of the controller and the clip that changed since the frame before,
// to find what plays the reload and how fast. The clip's words are written whole as it starts and when it changes.
constexpr uint32_t kControllerBytes = 0xC0;
constexpr uint32_t kClipBytes = 0x100;
constexpr uint32_t kNameBytes = 26;  // the controller's name, to its second copy at +0x76
constexpr uint32_t kMostReloadFrames = 1200;
struct Reload {
    bool on = false;
    uint32_t weapon = 0;
    uint32_t data = 0;
    uint32_t start = 0;  // its frame
    std::chrono::steady_clock::time_point time;
    uint16_t rounds = 0;  // as it started
    uint32_t clip = 0;
    std::vector<uint32_t> controller;
    std::vector<uint32_t> clip_words;
};
Reload g_reload;

uint16_t Rounds(uint32_t weapon) {
    uint32_t magazine = weapon ? R32(weapon + kWeaponMagazine) : 0;
    return magazine ? G<uint16_t>(magazine + kMagazineRounds) : 0;
}

// the rounds left to load for the weapon's ammo type
uint16_t Spare(uint32_t weapon, uint32_t data) {
    uint32_t spare = weapon ? R32(weapon + kWeaponSpareAmmo) : 0;
    return spare && data ? G<uint16_t>(spare + R32(data + kDataAmmoType) * 2) : 0;
}

// the words that differ, on the line begun
void Changes(FILE* f, const char* what, std::vector<uint32_t>& was, const std::vector<uint32_t>& now) {
    for (size_t i = 0; i < now.size() && i < was.size(); i++) {
        if (now[i] == was[i]) continue;
        std::fprintf(f, "  %s+%02X %g->%g", what, uint32_t(i * 4), AsFloat(was[i]), AsFloat(now[i]));
        if (!LooksFloat(now[i]) && !LooksFloat(was[i])) std::fprintf(f, " (%d->%d)", int32_t(was[i]), int32_t(now[i]));
    }
    was = now;
}

void TrackReloads(FILE* f, uint32_t weapon, uint32_t data, uint32_t frame) {
    static uint32_t last_state;
    uint32_t state = weapon ? R32(weapon + kWeaponState) : 0;
    uint32_t ac = R32(Player() + kCharacterAnimation);
    uint32_t clip = ac ? R32(ac + kAnimClip) : 0;
    std::string name = ac ? ReadCString(ac + kAnimName, kNameBytes) : "";
    uint16_t rounds = Rounds(weapon);
    Reload& r = g_reload;
    if (!r.on && weapon && state == kWeaponReloading && last_state != kWeaponReloading) {
        r = {true, weapon, data, frame, std::chrono::steady_clock::now(), rounds, clip, Read(ac, kControllerBytes),
             Read(clip, kClipBytes)};
        std::fprintf(f, "\n=== frame %u: reload starts, weapon %08X (%s), rounds %u of %d, spare %u, animation %08X "
                     "\"%s\" clip %08X\n",
                     frame, weapon, RecordName(data), rounds, int32_t(R32(data + kDataClip)), Spare(weapon, data), ac,
                     name.c_str(), clip);
        Dump(f, "clip", clip, kClipBytes);
    } else if (r.on && weapon != r.weapon) {
        std::fprintf(f, "reload: the weapon in hand changed after %u frames, left\n", frame - r.start);
        r.on = false;
    } else if (r.on) {
        uint32_t frames = frame - r.start;
        std::fprintf(f, "  %4u state %u rounds %u \"%s\"", frames, state, rounds, name.c_str());
        Changes(f, "ac", r.controller, Read(ac, kControllerBytes));
        if (clip != r.clip) {
            std::fprintf(f, "\n  clip %08X -> %08X\n", r.clip, clip);
            Dump(f, "clip", clip, kClipBytes);
            r.clip = clip;
            r.clip_words = Read(clip, kClipBytes);
        } else {
            Changes(f, "clip", r.clip_words, Read(clip, kClipBytes));
            std::fprintf(f, "\n");
        }
        if (rounds > r.rounds) {
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.time).count();
            std::fprintf(f, "=== reload done: %s, %u frames, %.2f s, rounds %u -> %u, spare %u, state %u, reload x2 %s\n",
                         RecordName(r.data), frames, seconds, r.rounds, rounds, Spare(weapon, data), state,
                         g_reload_speed_on ? "on" : "off");
            r.on = false;
        } else if (frames > kMostReloadFrames) {
            std::fprintf(f, "reload: no refill after %u frames, left\n", frames);
            r.on = false;
        }
    }
    last_state = state;
}

// F11: the player's reload and weapon switch animations at kReloadSpeed whatever the settings (rebalance::OverrideAnimationSpeeds), to
// compare with the game's
constexpr int kReloadSpeedKey = VK_F11;
constexpr float kReloadSpeed = 2.0f;

// each change of the weapon in hand's state or the player's animation: what a reload plays, the shotgun's shell by
// shell too, whose reload is no kWeaponReloading
void LogStates(FILE* f, uint32_t weapon, uint32_t frame) {
    static uint32_t last_weapon;
    static uint32_t last_state;
    static std::string last_name;
    uint32_t state = weapon ? R32(weapon + kWeaponState) : 0;
    uint32_t ac = R32(Player() + kCharacterAnimation);
    uint32_t clip = ac ? R32(ac + kAnimClip) : 0;
    std::string name = ac ? ReadCString(ac + kAnimName, kNameBytes) : "";
    if (weapon == last_weapon && state == last_state && name == last_name) return;
    uint32_t data = weapon ? R32(weapon + kWeaponData) : 0;
    std::fprintf(f, "frame %u: %s state %u -> %u, rounds %u, animation \"%s\" -> \"%s\", clip %08X frame %g speed %g\n",
                 frame, RecordName(data), last_state, state, Rounds(weapon), last_name.c_str(), name.c_str(), clip,
                 clip ? G<float>(clip + kAnimClipFrame) : 0.0f, clip ? G<float>(clip + kAnimClipSpeed) : 0.0f);
    last_weapon = weapon;
    last_state = state;
    last_name = name;
}


// F5: the weapon in hand becomes the next weapon of the game's weapon table, Shift+F5 the one before, the way the
// game's own give-weapon event does it (sub_001E8830): the character takes the weapon's model (sub_001C5CD0), the
// weapon object lets go of its magazines (sub_001C2A30) and is set up again as the new kind (sub_001C9FA0). Then a
// full clip and kSpareClips clips of spare ammo. The weapon records hold the rebalanced stats, so the weapon given
// has them. The key only asks: the swap waits for the frame's end (BetweenFrames, at D3DDevice_Swap), as one made in
// the middle of the weapon's own update left it using what was let go. A kind whose model this level has not loaded
// is passed over, found as sub_001C5CD0 finds it: its name from the kind's hash (sub_000C09F0), then the level's
// resources (sub_001F83E0). The player has no third person model holder (+0x258 is 0, which sub_001C5CD0 writes
// through: the crash of the first two versions), so the model is only taken for a character that has one.
constexpr int kWeaponKey = VK_F5;
constexpr uint32_t kCharacterModels = 0x258;  // -> +0xC the model of the weapon in hand; 0 for the player
constexpr uint32_t kTakeModel = 0x1C5CD0;    // ebx the character, (hash, hash)
constexpr uint32_t kLetGo = 0x1C2A30;        // ebx the weapon
constexpr uint32_t kSetUp = 0x1C9FA0;        // al the kind, ecx the character, edx the weapon, (0)
constexpr int kSpareClips = 4;
int g_give_step;  // the swap asked for: 1 the next, -1 the one before, 0 none

// the kinds whose data is one of the weapon records, in the table's order
std::vector<uint32_t> GunKinds() {
    std::vector<uint32_t> guns;
    for (uint32_t i = 0; i < weapon_kinds::Count(); i++)
        if (std::strcmp(RecordName(R32(weapon_kinds::Entry(i) + 8)), "?") != 0) guns.push_back(i);
    return guns;
}

// "WEAPON: ak47 (6/16)" for the weapon in hand, empty without one
std::string WeaponLine(uint32_t weapon) {
    if (!weapon) return "";
    std::vector<uint32_t> guns = GunKinds();
    auto at = std::find(guns.begin(), guns.end(), uint32_t(G<uint8_t>(weapon + weapon_kinds::kWeaponKind)));
    char line[64];
    std::snprintf(line, sizeof line, "WEAPON: %s (%zu/%zu)", RecordName(R32(weapon + kWeaponData)),
                  at == guns.end() ? size_t(0) : size_t(at - guns.begin()) + 1, guns.size());
    return line;
}

void GiveWeapon(FILE* f, X86Context& ctx, int step, uint32_t frame) {
    uint32_t player = Player();
    uint32_t weapon = R32(player + kPlayerWeapon);
    std::vector<uint32_t> guns = GunKinds();
    std::fprintf(f, "\n=== frame %u: F5 swap, weapon %08X, %zu kinds\n", frame, weapon, guns.size());
    std::fflush(f);
    if (!weapon || guns.empty()) return;
    RestoreExaggerated(f);
    size_t n = guns.size();
    uint32_t now = G<uint8_t>(weapon + weapon_kinds::kWeaponKind);
    auto at = std::find(guns.begin(), guns.end(), now);
    size_t from = at == guns.end() ? (step > 0 ? n - 1 : 0) : size_t(at - guns.begin());
    const char* had = RecordName(R32(weapon + kWeaponData));
    X86Context saved = ctx;
    // the next kind whose model the level has loaded
    uint32_t kind = now;
    uint32_t entry = 0;
    for (size_t tries = 1; tries <= n; tries++) {
        uint32_t next = guns[(from + n + step * int(tries)) % n];
        if (next == now) break;
        entry = weapon_kinds::Entry(next);
        std::fprintf(f, "  kind %u %s: looking for its model\n", next, RecordName(R32(entry + 8)));
        std::fflush(f);
        uint32_t model = weapon_kinds::Model(ctx, entry);
        std::fprintf(f, "  kind %u: model %08X%s\n", next, model, model ? "" : ", not in this level, borrowed");
        std::fflush(f);
        kind = next;
        break;
    }
    if (kind == now) {
        ctx = saved;
        std::fprintf(f, "  no other weapon this level has loaded\n");
        std::fflush(f);
        return;
    }
    if (R32(player + kCharacterModels) && weapon_kinds::Model(ctx, entry)) {
        std::fprintf(f, "  taking its model\n");
        std::fflush(f);
        ctx.ebx = player;
        CallGuest(ctx, kTakeModel, {R32(entry), R32(entry + 4)});
    }
    std::fprintf(f, "  letting go of %s\n", had);
    std::fflush(f);
    ctx.ebx = weapon;
    CallGuest(ctx, kLetGo, {});
    std::fprintf(f, "  setting up kind %u\n", kind);
    std::fflush(f);
    ctx.eax = kind;
    ctx.ecx = player;
    ctx.edx = weapon;
    CallGuest(ctx, kSetUp, {0});
    ctx = saved;
    uint32_t data = R32(weapon + kWeaponData);
    int32_t clip = data ? int32_t(R32(data + kDataClip)) : 0;
    uint32_t magazine = R32(weapon + kWeaponMagazine);
    if (magazine && clip > 0) G<uint16_t>(magazine + kMagazineRounds) = uint16_t(clip);
    uint32_t spare = R32(weapon + kWeaponSpareAmmo);
    if (spare && data && clip > 0) {
        uint16_t& left = G<uint16_t>(spare + R32(data + kDataAmmoType) * 2);
        left = std::max<uint16_t>(left, uint16_t(std::min(clip * kSpareClips, 0xFFFF)));
    }
    std::fprintf(f, "  done: %s (record %08X), magazine %08X rounds %u, spare %u\n", RecordName(data), data, magazine,
                 Rounds(weapon), Spare(weapon, data));
    std::fflush(f);
}

// At each stage's start, once its pickups are set: the pickup list (each entry's weapon kind, its model's name and
// whether this stage has the model) and the 17 slots of loaded files, to see what borrowing a pickup model needs
constexpr uint32_t kPickups = 0x2D0D64;     // -> +kPickupList -> +0 byte the count, +8 the pickups
constexpr uint32_t kPickupList = 0x5844;
constexpr uint32_t kPickupModels = 0x5800;  // -> a model each pickup, 0 until looked up
constexpr uint32_t kPickupSize = 0x18;      // +8 +0xC its model's name, +0x10 byte its weapon kind
constexpr uint32_t kResources = 0x2D45D8;   // -> +kFileSlots: kFileSlotCount slots of kFileSlotSize
constexpr uint32_t kFileSlots = 0xD10;
constexpr uint32_t kFileSlotCount = 17;
constexpr uint32_t kFileSlotSize = 0x0C;
constexpr uint32_t kFindResource = 0x1F83E0;  // stdcall ([kResources], name, name): the resource, 0 none

void LogPickups(FILE* f, X86Context& ctx) {
    static uint32_t logged;
    uint32_t pickups = R32(kPickups);
    uint32_t list = pickups ? R32(pickups + kPickupList) : 0;
    uint32_t world = R32(kWorld);
    // once the stage's own pack is in, its models with it
    if (!list || list == logged || !world || !R32(world + 0x5B34)) return;
    logged = list;
    uint8_t count = G<uint8_t>(list);
    uint32_t entries = R32(list + 8);
    uint32_t models = R32(pickups + kPickupModels);
    uint32_t resources = R32(kResources);
    std::fprintf(f, "\n=== pickups, level %d: %u at %08X, models %08X\n", world ? int(G<uint8_t>(world + kWorldLevel)) : -1,
                 count, entries, models);
    LOGR("pickups: %u, level %d", count, world ? int(G<uint8_t>(world + kWorldLevel)) : -1);
    X86Context saved = ctx;
    for (uint8_t i = 0; i < count && entries; i++) {
        uint32_t entry = entries + i * kPickupSize;
        uint8_t kind = G<uint8_t>(entry + 0x10);
        uint32_t lo = R32(entry + 8);
        uint32_t hi = R32(entry + 0xC);
        uint32_t cached = models ? R32(models + i * 4) : 0;
        uint32_t found = cached;  // no lookup: one while the stage still loads crashed (sub_001F83E0)
        uint32_t data = R32(weapon_kinds::Entry(kind) + 8);
        std::fprintf(f, "  %2u kind %2u %-9s model %08X%08X cached %08X; words", i, kind, RecordName(data), hi, lo,
                     found);
        for (uint32_t at = 0; at < kPickupSize; at += 4) std::fprintf(f, " %08X", R32(entry + at));
        std::fprintf(f, "\n");
        LOGR("  pickup %u: kind %u %s, model %08X%08X%s", i, kind, RecordName(data), hi, lo,
             found ? ", looked up" : "");
    }
    ctx = saved;
    uint32_t used = 0;
    std::fprintf(f, "file slots of %08X:\n", resources);
    for (uint32_t s = 0; s < kFileSlotCount && resources; s++) {
        uint32_t slot = resources + kFileSlots + s * kFileSlotSize;
        std::fprintf(f, "  slot %2u: %08X %08X %08X\n", s, R32(slot), R32(slot + 4), R32(slot + 8));
        if (R32(slot + 8)) used++;
    }
    std::fprintf(f, "  %u of %u slots with a file\n", used, kFileSlotCount);
    LOGR("file slots: %u of %u with a file", used, kFileSlotCount);
    std::fflush(f);
}

// Research: a borrowed weapon's pickup model, from another level's Guns.bin (Guns_s.bin) loaded as the stage loads its
// own (sub_001EDC90: sub_001F37E0 queues it in loading slot 8, sub_001D3790 files it as a type 0xC slot keyed by level
// and New Game+, its models found by name from then on). Once the stage's own pack is in and the loading slot is
// idle, the pack with the model of the weapon in hand, when the stage lacks it, is queued the same way, its slot then
// given a key of its own and a use count of 1. Every step, the slots and the pack places are logged; anything full or
// unexpected leaves the weapon to vanish when dropped, as before. Only on F12, once in control: done on its own as the
// stage started, the first lookup crashed.
constexpr uint32_t kQueue = 0x1F37E0;       // eax the loading slot, ecx the path, edx [kResources], 6 arguments
constexpr uint32_t kFileIn = 0x1D3790;      // the loading slot's callback: files the pack in a type 0xC slot
constexpr uint32_t kGunsSlot = 8;           // the loading slot the stage's Guns.bin uses
constexpr uint32_t kLoadSize = 0x13C;       // a loading slot: +0x134 byte busy
constexpr uint32_t kLoadBusy = 0x134;
constexpr uint32_t kSlots = 0xD14;          // in [kResources]: kSlotCount of kSlotSize: +0 type, +4 the file, +8 key, +0xA uses
constexpr uint32_t kSlotCount = 17;
constexpr uint32_t kSlotSize = 0x0C;
constexpr uint32_t kGunsType = 0x0C;
constexpr uint32_t kPackPlaces = 0xC64;     // in [kResources]: kPackPlaceCount places each of the two lists
constexpr uint32_t kPackPlaces2 = 0xC90;
constexpr uint32_t kPackPlaceCount = 11;
constexpr uint32_t kWorldGuns = 0x5B34;     // the stage's own pack, once in
constexpr uint32_t kWorldNewGamePlusFlag = 0x5CB0;
constexpr uint16_t kOurKey = 0x7E00;        // ours: 0x7E00 + the kind, a key no level has

struct Borrow {
    uint8_t kind = 0xFF;
    uint32_t file = 0;      // the pack's file, once filed
    int state = 0;          // 0 none, 1 queued, 2 filed, -1 given up
};
Borrow g_borrow;
uint32_t g_borrow_world;
uint32_t g_path;  // guest room for the path

// the pickup of a kind: its model's name, false without one
bool PickupModelName(uint8_t kind, uint32_t& lo, uint32_t& hi) {
    uint32_t pickups = R32(kPickups);
    uint32_t list = pickups ? R32(pickups + kPickupList) : 0;
    uint8_t count = list ? G<uint8_t>(list) : 0;
    uint32_t entries = list ? R32(list + 8) : 0;
    for (uint8_t i = 0; i < count && entries; i++) {
        uint32_t entry = entries + i * kPickupSize;
        if (G<uint8_t>(entry + 0x10) != kind) continue;
        lo = R32(entry + 8);
        hi = R32(entry + 0xC);
        return true;
    }
    return false;
}

// the Xbox path of a level's Guns.bin or Guns_s.bin holding that model, empty for none
std::string PackWith(uint32_t lo, uint32_t hi) {
    for (int level = 0; level < 100; level++) {
        for (const char* suffix : {"", "_s"}) {
            char name[96];
            std::snprintf(name, sizeof name, "D:\\Levels\\Level_%02d\\Stg_0001\\Guns%s.bin", level, suffix);
            std::string host = ResolvePath(name, 0);
            if (host.empty()) continue;
            FILE* f = _wfopen(std::filesystem::path(host).c_str(), L"rb");
            if (!f) continue;
            uint32_t entry[4];
            bool found = false;
            std::fseek(f, 0x100, SEEK_SET);
            while (std::fread(entry, 4, 4, f) == 4 && (entry[1] >> 16) == 0x5446)
                if (entry[0] == lo && entry[1] == hi) found = true;
            std::fclose(f);
            if (found) return name + 2;  // without D:, as the game's own path
        }
    }
    return "";
}

void LogSlots(FILE* f, const char* when) {
    uint32_t resources = R32(kResources);
    if (!resources) return;
    uint32_t used = 0;
    std::fprintf(f, "slots %s:", when);
    for (uint32_t s = 0; s < kSlotCount; s++) {
        uint32_t slot = resources + kSlots + s * kSlotSize;
        if (!R32(slot + 4)) continue;
        used++;
        std::fprintf(f, " [%u type %X file %08X key %04X uses %u]", s, R32(slot), R32(slot + 4), G<uint16_t>(slot + 8),
                     G<uint16_t>(slot + 0xA));
    }
    uint32_t packs = 0, packs2 = 0;
    for (uint32_t p = 0; p < kPackPlaceCount; p++) {
        packs += R32(resources + kPackPlaces + p * 4) != 0;
        packs2 += R32(resources + kPackPlaces2 + p * 4) != 0;
    }
    std::fprintf(f, "\n  %u of %u slots, pack places %u and %u of %u\n", used, kSlotCount, packs, packs2, kPackPlaceCount);
    LOGR("borrow: slots %s: %u of %u, pack places %u and %u of %u", when, used, kSlotCount, packs, packs2,
         kPackPlaceCount);
    std::fflush(f);
}

constexpr int kBorrowKey = VK_F12;
bool g_borrow_asked;  // F12: borrow the weapon in hand's pickup model, once in control

void BorrowPickupModel(FILE* f, X86Context& ctx) {
    uint32_t world = R32(kWorld);
    uint32_t resources = R32(kResources);
    if (!world || !resources || !R32(world + kWorldGuns)) return;
    if (world != g_borrow_world || g_borrow.kind == 0xFF) {
        if (world != g_borrow_world) {
            LogSlots(f, "at a new level");
            g_borrow = {};
            g_borrow_world = world;
        }
    }
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon) return;
    uint8_t kind = G<uint8_t>(weapon + weapon_kinds::kWeaponKind);
    uint16_t key = uint16_t(G<uint8_t>(world + kWorldLevel) | G<uint8_t>(world + kWorldNewGamePlusFlag) << 8);
    if (g_borrow.state == 1) {
        // filed: the type 0xC slot of the stage's key whose file is not the stage's own
        for (uint32_t s = 0; s < kSlotCount; s++) {
            uint32_t slot = resources + kSlots + s * kSlotSize;
            uint32_t file = R32(slot + 4);
            if (!file || R32(slot) != kGunsType || G<uint16_t>(slot + 8) != key || file == R32(world + kWorldGuns))
                continue;
            G<uint16_t>(slot + 8) = uint16_t(kOurKey + g_borrow.kind);
            G<uint16_t>(slot + 0xA) = 1;
            g_borrow.file = file;
            g_borrow.state = 2;
            std::fprintf(f, "borrow: kind %u filed in slot %u, file %08X, key now %04X\n", g_borrow.kind, s, file,
                         kOurKey + g_borrow.kind);
            LOGR("borrow: kind %u pack filed in slot %u", g_borrow.kind, s);
            LogSlots(f, "after");
            return;
        }
        return;
    }
    if (!g_borrow_asked) return;
    g_borrow_asked = false;
    if (g_borrow.state != 0 || kind == g_borrow.kind) {
        std::fprintf(f, "borrow: F12, already %d for kind %u\n", g_borrow.state, g_borrow.kind);
        return;
    }
    uint32_t lo = 0, hi = 0;
    if (!PickupModelName(kind, lo, hi)) return;
    X86Context saved = ctx;
    uint32_t model = CallGuest(ctx, kFindResource, {resources, lo, hi});
    ctx = saved;
    if (model) return;  // the stage has it
    g_borrow.kind = kind;
    g_borrow.state = -1;
    std::fprintf(f, "\n=== borrow: kind %u (%s), pickup model %08X%08X not in this stage\n", kind,
                 RecordName(R32(weapon + kWeaponData)), hi, lo);
    LogSlots(f, "before");
    if (G<uint8_t>(resources + kGunsSlot * kLoadSize + kLoadBusy)) {
        std::fprintf(f, "borrow: the guns loading slot is busy, left\n");
        g_borrow = {};
        return;
    }
    uint32_t free_slots = 0, free_packs = 0, free_packs2 = 0;
    for (uint32_t s = 0; s < kSlotCount; s++) free_slots += !R32(resources + kSlots + s * kSlotSize + 4);
    for (uint32_t p = 0; p < kPackPlaceCount; p++) {
        free_packs += !R32(resources + kPackPlaces + p * 4);
        free_packs2 += !R32(resources + kPackPlaces2 + p * 4);
    }
    if (!free_slots || !free_packs || !free_packs2) {
        std::fprintf(f, "borrow: no room (%u slots, %u and %u pack places free), the weapon vanishes when dropped\n",
                     free_slots, free_packs, free_packs2);
        LOGR("borrow: no room for kind %u's pack", kind);
        return;
    }
    std::string path = PackWith(lo, hi);
    if (path.empty()) {
        std::fprintf(f, "borrow: no level's Guns.bin has the model\n");
        LOGR("borrow: no level has kind %u's pickup model", kind);
        return;
    }
    if (!g_path) g_path = mem::AllocSystem(0x100);
    std::snprintf(static_cast<char*>(HostPtr(g_path)), 0x100, "%s", path.c_str());
    std::fprintf(f, "borrow: queueing %s\n", path.c_str());
    LOGR("borrow: kind %u's pickup model from %s", kind, path.c_str());
    std::fflush(f);
    ctx.eax = kGunsSlot;
    ctx.ecx = g_path;
    ctx.edx = resources;
    CallGuest(ctx, kQueue, {1, kFileIn, world, 0x17, 1, 0x2000000});
    ctx = saved;
    g_borrow.state = 1;
}


// Every file the game opens (game_files.cpp): all go to research.log, those of the levels' guns and weapons to
// black.log as well, with the weapon in hand, to see which weapon files a level loads and when
bool WeaponFile(std::string name) {
    for (char& c : name) c = char(std::tolower(uint8_t(c)));
    for (const char* part : {"fpguns", "gun", "wpn", "weapon", "bg1_", "ammo", "hud"})
        if (name.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

void research::FileOpened(const std::string& name, uint32_t root, uint32_t status) {
    FILE* f = Log();
    bool weapon_file = WeaponFile(name);
    if (!f && !weapon_file) return;
    static auto start = std::chrono::steady_clock::now();
    double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::string host = ResolvePath(name, root);
    std::error_code ec;
    uint64_t size = host.empty() ? 0 : uint64_t(std::filesystem::file_size(host, ec));
    if (ec) size = 0;
    uint32_t world = R32(kWorld);
    uint32_t weapon = world ? R32(Player() + kPlayerWeapon) : 0;
    const char* in_hand = weapon ? RecordName(R32(weapon + kWeaponData)) : "-";
    int level = world ? int(G<uint8_t>(world + kWorldLevel)) : -1;
    if (f) {
        std::fprintf(f, "file %.2f s: %s, %llu bytes, status %08X, level %d, in hand %s\n", at, name.c_str(),
                     static_cast<unsigned long long>(size), status, level, in_hand);
        std::fflush(f);
    }
    if (weapon_file)
        LOGR("file %.2f s: %s, %llu bytes, status %08X, level %d, in hand %s", at, name.c_str(),
             static_cast<unsigned long long>(size), status, level, in_hand);
}

namespace {

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

void BetweenFrames(X86Context& ctx) {
    FILE* f = Log();
    int step = g_give_step;
    g_give_step = 0;
    if (!f || !R32(kWorld)) return;
    LogPickups(f, ctx);
    BorrowPickupModel(f, ctx);
    if (!step) return;
    static uint32_t swaps;
    GiveWeapon(f, ctx, step, ++swaps);
    static std::string note;
    note = "F5: " + WeaponLine(R32(Player() + kPlayerWeapon));
    Note(note.c_str());
}

void OverlayLines(std::vector<std::string>& lines) {
    if (Log()) lines.push_back("WEAPON LOGGING: ON");
    if (g_note && std::chrono::steady_clock::now() < g_note_until) lines.push_back(g_note);
    if (Log()) lines.push_back(g_exaggerate_line);
    if (Log()) lines.push_back(g_reload_speed_on ? "RELOAD+SWITCH x2: ON" : "RELOAD+SWITCH x2: OFF");
    if (Log() && R32(kWorld)) {
        std::string line = WeaponLine(R32(Player() + kPlayerWeapon));
        if (!line.empty()) lines.push_back(line);
    }
    if (uint32_t weapon = R32(kWorld) ? R32(Player() + kPlayerWeapon) : 0) {
        uint32_t data = R32(weapon + kWeaponData);
        char clip[64];
        std::snprintf(clip, sizeof clip, "ROUNDS %u / %d  SPARE %u%s", Rounds(weapon),
                      data ? int32_t(R32(data + kDataClip)) : 0, Spare(weapon, data), g_reload.on ? "  RELOADING" : "");
        lines.push_back(clip);
    }
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
    if (Pressed(kReloadSpeedKey)) {
        g_reload_speed_on = !g_reload_speed_on;
        Note(g_reload_speed_on ? "F11: RELOAD+SWITCH x2 ON" : "F11: RELOAD+SWITCH x2 OFF");
        std::fprintf(f, "\n=== frame %u: F11, reload x%g %s\n", frame, kReloadSpeed, g_reload_speed_on ? "on" : "off");
    }
    if (Pressed(kBorrowKey)) {
        g_borrow_asked = true;
        Note("F12: BORROW PICKUP MODEL");
    }
    if (Pressed(kWeaponKey)) {
        g_give_step = GetAsyncKeyState(VK_SHIFT) & 0x8000 ? -1 : 1;
        Note(g_give_step > 0 ? "F5: NEXT WEAPON" : "F5: WEAPON BEFORE");
    }
    TrackReloads(f, weapon, data, frame);
    rebalance::OverrideAnimationSpeeds(g_reload_speed_on ? kReloadSpeed : 0.0f);
    LogStates(f, weapon, frame);

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
