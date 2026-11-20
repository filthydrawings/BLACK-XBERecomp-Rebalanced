// Research logger, temporary: finds the guest fields that hold a weapon's clip size, range and damage and a
// character's health. Writes logs\research.log beside the exe.
//   on each weapon change: the weapon object and its data block, as words
//   F6: the player, the weapon and its data, the words that changed since the last F6
//   F7: the weapon object and its data block again, in full
//   each character as its health is first set: its health, the +0x3D0 it came from and the difficulty
// On screen, under the FPS counter: the player's health, the difficulty and the last character whose health changed.
// Nothing in the game is changed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// character
constexpr uint32_t kCharacterHealth = 0x2F0;     // float
constexpr uint32_t kCharacterHealthFrom = 0x3D0;  // float, copied to the health as a character starts
constexpr size_t kMostCharacters = 512;

// characters whose health was set, and what it read last frame
struct Character {
    uint32_t va;
    float health;
};
std::vector<Character> g_characters;
uint32_t g_last_hit;  // the last one other than the player whose health changed

FILE* Log() {
    static FILE* file = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exe).parent_path() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return _wfopen((dir / "research.log").c_str(), L"w");
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

// the characters' health against last frame: the last one changed is the one the overlay shows
void WatchCharacters(uint32_t player) {
    for (Character& c : g_characters) {
        float now = G<float>(c.va + kCharacterHealth);
        if (now != c.health && c.va != player) g_last_hit = c.va;
        c.health = now;
    }
}

}  // namespace

// sub_001DEC70 at 0x1DF229, mov [ebx+0x2F0], ecx: a character's health, from its +0x3D0
void CharacterHealthStart(X86Context& ctx, uint8_t*) {
    uint32_t character = ctx.ebx;
    float health = AsFloat(ctx.ecx);
    bool player = character == Player();
    if (FILE* f = Log()) {
        std::fprintf(f, "character %08X starts: health %g, +3D0 %g, difficulty %u%s\n", character, health,
                     G<float>(character + kCharacterHealthFrom), Difficulty(), player ? ", the player" : "");
        std::fflush(f);
    }
    auto known = std::find_if(g_characters.begin(), g_characters.end(),
                              [&](const Character& c) { return c.va == character; });
    if (known != g_characters.end()) known->health = health;
    else if (g_characters.size() < kMostCharacters) g_characters.push_back({character, health});
}

namespace research {

void OverlayLines(std::vector<std::string>& lines) {
    if (!R32(kWorld)) return;
    char text[64];
    uint32_t player = Player();
    std::snprintf(text, sizeof text, "DIFFICULTY %u", Difficulty());
    lines.push_back(text);
    std::snprintf(text, sizeof text, "PLAYER %.0f / %.0f", double(G<float>(player + kCharacterHealth)),
                  double(G<float>(player + kCharacterHealthFrom)));
    lines.push_back(text);
    if (g_last_hit) {
        std::snprintf(text, sizeof text, "HIT %08X %.0f / %.0f", g_last_hit,
                      double(G<float>(g_last_hit + kCharacterHealth)),
                      double(G<float>(g_last_hit + kCharacterHealthFrom)));
        lines.push_back(text);
    }
}

void Tick() {
    FILE* f = Log();
    if (!f || !R32(kWorld)) return;
    uint32_t player = Player();
    WatchCharacters(player);
    uint32_t weapon = R32(player + kPlayerWeapon);
    uint32_t data = weapon ? R32(weapon + kWeaponData) : 0;
    static uint32_t frame;
    frame++;

    static uint32_t last_data;
    bool dump = Pressed(kDumpKey);
    if (data != last_data || dump) {
        std::fprintf(f, "\n=== frame %u: %s, weapon %08X\n", frame, dump ? "F7" : "weapon changed", weapon);
        Dump(f, "weapon", weapon, kWeaponBytes);
        Dump(f, "weapon data", data, kDataBytes);
        last_data = data;
    }

    static Snapshot snapshots[] = {{"player"}, {"weapon"}, {"weapon data"}};
    if (Pressed(kDiffKey)) {
        static int count;
        std::fprintf(f, "\n=== F6 #%d, frame %u\n", ++count, frame);
        Diff(f, snapshots[0], player, kPlayerBytes);
        Diff(f, snapshots[1], weapon, kWeaponBytes);
        Diff(f, snapshots[2], data, kDataBytes);
    }
    std::fflush(f);
}

}  // namespace research
