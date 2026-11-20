// Rebalance, from balance.toml beside the exe (config/balance.toml, copied there by the build): each weapon's clip and
// the body hits each enemy type takes to kill from each weapon, by difficulty. Read once, as the first level loads.
// The game keeps a hits table per enemy type, an entry per weapon record: the share of health a hit takes from the
// front and from the back, 1.02 over the hits. Every hit's entry is written just before the game reads it, with the
// setting for the difficulty played, or the game's own without one.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <toml++/toml.hpp>

#include "core/log.h"
#include "game/layout.h"
#include "game/rebalance.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

namespace fs = std::filesystem;

// the weapon records in order, as balance.toml names them
constexpr const char* kWeaponNames[kWeaponRecords] = {
    "dc3elite", "spas12", "sniper", "uzi", "g36c", "ak47", "rpg", "launcher",
    "mac10", "p90", "m249", "magnum", "m16", "rem870", "glock", "mp5"};
// the records a hits row lists, in its order: all but the RPG and the launcher, whose damage is the explosion's
constexpr uint32_t kRowWeapons[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr size_t kRowSize = std::size(kRowWeapons);
// the tables by difficulty, 0 to 3: easy plays normal's
constexpr const char* kDifficultyTables[] = {"normal", "normal", "hard", "black_ops"};
constexpr uint32_t kDifficulties = 4;

constexpr float kShareOfHits = 1.02f;  // the share of a hit of N hits to kill: this over N
constexpr uint32_t kMostTypes = 128;

struct Shares {
    float front = 0;
    float back = 0;
};
using Row = std::array<std::optional<Shares>, kWeaponRecords>;

struct Type {
    uint32_t id;
    uint32_t table;
    std::array<Shares, kWeaponRecords> game;  // as the game had them, before any write
};
std::vector<Type> g_types;  // read at the first hit
std::map<uint32_t, std::array<Row, kDifficulties>> g_settings;  // by type id
std::array<std::optional<int32_t>, kWeaponRecords> g_clips;
bool g_loaded;

fs::path FilePath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path() / "balance.toml";
}

float Share(double hits) { return hits > 0 ? kShareOfHits / float(hits) : 0.0f; }

uint32_t Record(uint32_t weapon) { return kWeaponRecordsAt + weapon * kWeaponRecordSize; }
bool RecordsMapped() { return R32(Record(0) + 4) == 1 && G<float>(Record(0) + 0x1C) == 1.0f; }

int WeaponIndex(std::string_view name) {
    for (uint32_t i = 0; i < kWeaponRecords; i++)
        if (name == kWeaponNames[i]) return int(i);
    return -1;
}

// a row of hits to kill, kRowSize numbers in kRowWeapons' order, as the shares of one side
std::optional<std::array<float, kRowSize>> ReadRow(const toml::node* node) {
    const toml::array* a = node ? node->as_array() : nullptr;
    if (!a || a->size() != kRowSize) return std::nullopt;
    std::array<float, kRowSize> shares;
    for (size_t i = 0; i < kRowSize; i++) {
        auto hits = (*a)[i].value<double>();
        if (!hits) return std::nullopt;
        shares[i] = Share(*hits);
    }
    return shares;
}

// [weapons] NAME = { clip = N, ... } and per difficulty table type_ID = [...], type_ID_back = [...]
void Load() {
    g_settings.clear();
    g_clips = {};
    fs::path path = FilePath();
    if (!fs::exists(path)) {
        LOGI("balance.toml: none beside the exe, the game's own values");
        return;
    }
    toml::table t;
    try {
        t = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        LOGE("balance.toml: %s", e.what());
        return;
    }
    if (const toml::table* weapons = t["weapons"].as_table()) {
        for (auto&& [name, node] : *weapons) {
            int w = WeaponIndex(name.str());
            const toml::table* weapon = node.as_table();
            if (w < 0 || !weapon) continue;
            if (auto clip = (*weapon)["clip"].value<int64_t>()) g_clips[w] = int32_t(*clip);
        }
    }
    for (uint32_t d = 0; d < kDifficulties; d++) {
        const toml::table* table = t[kDifficultyTables[d]].as_table();
        if (!table) continue;
        for (auto&& [key, node] : *table) {
            std::string name(key.str());
            uint32_t id = 0;
            int end = 0;
            if (std::sscanf(name.c_str(), "type_%u%n", &id, &end) != 1 || name.size() != size_t(end)) continue;
            auto front = ReadRow(&node);
            if (!front) {
                LOGE("balance.toml: %s.%s needs %zu numbers", kDifficultyTables[d], name.c_str(), kRowSize);
                continue;
            }
            auto back = ReadRow(table->get(name + "_back"));
            for (size_t i = 0; i < kRowSize; i++)
                g_settings[id][d][kRowWeapons[i]] = Shares{(*front)[i], back ? (*back)[i] : (*front)[i]};
        }
    }
    LOGI("balance.toml: %zu enemy types set", g_settings.size());
}

void ApplyClips() {
    static std::array<int32_t, kWeaponRecords> game;
    static bool read;
    if (!RecordsMapped()) return;
    if (!read) {
        for (uint32_t w = 0; w < kWeaponRecords; w++) game[w] = int32_t(R32(Record(w) + kDataClip));
        read = true;
    }
    for (uint32_t w = 0; w < kWeaponRecords; w++) W32(Record(w) + kDataClip, uint32_t(g_clips[w].value_or(game[w])));
}

// every type in the array a hit's type is in, with its hits table as the game has it
void ReadTypes(uint32_t any) {
    uint32_t base = any - R32(any + kTypeId) * kTypeSize;
    for (uint32_t id = 0; id < kMostTypes; id++) {
        uint32_t type = base + id * kTypeSize;
        uint32_t hits = R32(type + kTypeId) == id ? R32(type + kTypeHits) : 0;
        uint32_t table = hits ? R32(hits) : 0;
        if (!table) continue;
        Type t{id, table, {}};
        for (uint32_t w = 0; w < kWeaponRecords; w++) {
            uint32_t entry = table + w * kHitsEntrySize;
            t.game[w] = {G<float>(entry), G<float>(entry + 4)};
        }
        g_types.push_back(t);
    }
}

uint32_t Difficulty() { return std::min(R32(R32(kGame) + kGameDifficulty), kDifficulties - 1); }

}  // namespace

namespace rebalance {

void Tick() {
    if (g_loaded || !R32(kWorld)) return;
    Load();
    ApplyClips();
    g_loaded = true;
}

}  // namespace rebalance

// sub_001E74B0 at 0x1E77F4, the call that takes a hit's damage from its type's hits table: ebx the character hit,
// [ebp+0x18] the weapon's entry. The entry gets this difficulty's setting, else the game's own
void RebalanceHit(X86Context& ctx, uint8_t*) {
    uint32_t type = R32(ctx.ebx + kCharacterType);
    uint32_t weapon = G<uint8_t>(ctx.ebp + 0x18);
    if (!type || weapon >= kWeaponRecords) return;
    if (g_types.empty()) ReadTypes(type);
    uint32_t id = R32(type + kTypeId);
    auto t = std::find_if(g_types.begin(), g_types.end(), [&](const Type& t) { return t.id == id; });
    if (t == g_types.end()) return;
    Shares shares = t->game[weapon];
    if (auto set = g_settings.find(id); set != g_settings.end())
        if (const auto& s = set->second[Difficulty()][weapon]) shares = *s;
    uint32_t entry = t->table + weapon * kHitsEntrySize;
    G<float>(entry) = shares.front;
    G<float>(entry + 4) = shares.back;
}
