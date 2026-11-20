// Rebalance, from balance.toml beside the exe (config/balance.toml, copied there by the build): each weapon's clip and
// the body hits each enemy type takes to kill from each weapon, by difficulty. Read once, as the first level loads.
// The game keeps a hits table per enemy type, an entry per weapon record: the share of health a hit takes from the
// front and from the back, 1.02 over the hits. Every hit's entry is written just before the game reads it, with the
// setting for the difficulty played, or the game's own without one.
// The Balancing Settings toggles (game::Options) pick what applies, and take effect as they change.
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

#include "common/options.h"
#include "core/log.h"
#include "game/layout.h"
#include "game/rebalance.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

namespace fs = std::filesystem;

// the records a hits row lists, in its order: all but the RPG and the launcher, whose damage is the explosion's
constexpr uint32_t kRowWeapons[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr size_t kRowSize = std::size(kRowWeapons);
// the tables by difficulty, 0 to 3: easy plays normal's
constexpr const char* kDifficultyTables[] = {"normal", "normal", "hard", "black_ops"};
constexpr uint32_t kDifficulties = 4;
constexpr uint32_t kNormal = 1;

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
// a stat of the weapon records balance.toml sets, by its key in [weapons]
struct WeaponField {
    const char* key;
    uint32_t offset;
    bool is_float;
};
constexpr WeaponField kWeaponFields[] = {
    {"clip", kDataClip, false},
    {"range", kDataRange, true},
    {"recoil", kDataRecoil, true},
};
constexpr size_t kFields = std::size(kWeaponFields);
enum { kClipField, kRangeField, kRecoilField };
using FieldValues = std::array<std::optional<double>, kWeaponRecords>;
std::array<FieldValues, kFields> g_weapons;  // by field, from the file
FieldValues g_reload;  // reload speed multipliers, read but not applied: the reload timer is not found yet
bool g_loaded;

fs::path FilePath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path() / "balance.toml";
}

float Share(double hits) { return hits > 0 ? kShareOfHits / float(hits) : 0.0f; }


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
    g_weapons = {};
    g_reload = {};
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
            for (size_t f = 0; f < kFields; f++)
                if (auto v = (*weapon)[kWeaponFields[f].key].value<double>()) g_weapons[f][w] = *v;
            if (auto v = (*weapon)["reload"].value<double>()) g_reload[w] = *v;
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

// the field applied, by its toggle
bool Applies(size_t field) {
    const game::Options& o = game::Opt();
    switch (field) {
        case kClipField: return o.realistic_clips;
        case kRangeField: return o.range_adjust;
        case kRecoilField: return o.recoil_adjust;
        default: return false;
    }
}

// a record's field as a number, and back
double ReadField(uint32_t record, const WeaponField& f) {
    return f.is_float ? double(G<float>(record + f.offset)) : double(int32_t(R32(record + f.offset)));
}
void WriteField(uint32_t record, const WeaponField& f, double v) {
    if (f.is_float) G<float>(record + f.offset) = float(v);
    else W32(record + f.offset, uint32_t(int32_t(std::lround(v))));
}

// each field applied: the file's value, else the game's, which a field not applied gets back; the game's are read
// once, before any write. False before the records are mapped
bool ApplyWeapons() {
    static std::array<std::array<double, kWeaponRecords>, kFields> game;
    static bool read;
    if (!WeaponRecordsMapped()) return false;
    if (!read) {
        for (size_t f = 0; f < kFields; f++)
            for (uint32_t w = 0; w < kWeaponRecords; w++) game[f][w] = ReadField(WeaponRecord(w), kWeaponFields[f]);
        read = true;
    }
    for (size_t f = 0; f < kFields; f++) {
        bool applies = Applies(f);
        for (uint32_t w = 0; w < kWeaponRecords; w++) {
            double v = applies ? g_weapons[f][w].value_or(game[f][w]) : game[f][w];
            WriteField(WeaponRecord(w), kWeaponFields[f], v);
        }
    }
    return true;
}

// the weapon toggles as last applied, one bit a field
uint32_t WeaponToggles() {
    uint32_t bits = 0;
    for (size_t f = 0; f < kFields; f++) bits |= uint32_t(Applies(f)) << f;
    return bits;
}
std::optional<uint32_t> g_applied;  // none until the records take a write

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

// the table a difficulty plays: its own, or normal's with per difficulty rebalancing off
uint32_t TableFor(uint32_t difficulty) { return game::Opt().per_difficulty_rebalance ? difficulty : kNormal; }

}  // namespace

namespace rebalance {

void Tick() {
    if (!g_loaded) {
        if (!R32(kWorld)) return;
        Load();
        g_loaded = true;
    } else if (WeaponToggles() == g_applied) {
        return;
    }
    if (ApplyWeapons()) g_applied = WeaponToggles();
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
    auto set = g_settings.find(id);
    if (game::Opt().enemy_rebalance && set != g_settings.end())
        if (const auto& s = set->second[TableFor(Difficulty())][weapon]) shares = *s;
    uint32_t entry = t->table + weapon * kHitsEntrySize;
    G<float>(entry) = shares.front;
    G<float>(entry + 4) = shares.back;
}
