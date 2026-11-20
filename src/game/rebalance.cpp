// Rebalance, from balance.toml beside the exe (config/balance.toml, copied there by the build): each weapon's clip and
// the body hits each enemy type takes to kill from each weapon, by difficulty. Read once, as the first level loads.
// The game keeps a hits table per enemy type, an entry per weapon record: the share of health a hit takes from the
// front and from the back, 1.02 over the hits. Every hit's entry is written just before the game reads it, with the
// setting for the difficulty played, or the game's own without one.
// The Balancing Settings toggles (game::Options) pick what applies, and take effect as they change.
// Each weapon's reload and draw speeds play its reload and weapon switch animations faster or slower.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
// Animation speeds: an animation of the player's of a kind, by its name, plays at its weapon's speed for that kind.
// A reload plays the reload animation, and the clip refills as the animation reaches the point it does, so the
// animation's playback speed sets the reload's; a weapon switch plays the new weapon's draw animation
constexpr size_t kMostSuffixes = 4;
constexpr size_t kMostStates = 4;
struct AnimationKind {
    const char* key;                         // its speed in each weapon's [weapons] entry
    const char* suffixes[kMostSuffixes];     // the ends of its animations' names
    uint32_t states[kMostStates];            // it starts only with the weapon in one of these states; none, any
    bool game::Options::*toggle;
};
constexpr AnimationKind kAnimationKinds[] = {
    // a magazine reload in kWeaponReloading; a shotgun's shell by shell, 180 going in, 502 its shells (state 6), 182
    // coming out (state 8)
    {"reload", {"_101", "_180", "_502", "_182"}, {kWeaponReloading, 5, 6, 8}, &game::Options::reload_adjust},
    {"draw", {"_102"}, {}, &game::Options::draw_adjust},  // the weapon coming up, after a switch
};
constexpr size_t kKinds = std::size(kAnimationKinds);
std::array<FieldValues, kKinds> g_speeds;  // by kind, clamped to kLeastSpeed to kMostSpeed
constexpr double kLeastSpeed = 0.25;
constexpr double kMostSpeed = 4.0;
float g_speed_override;  // a speed for every kind whatever the settings, 0 none
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
    g_speeds = {};
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
            for (size_t k = 0; k < kKinds; k++) {
                auto v = (*weapon)[kAnimationKinds[k].key].value<double>();
                if (!v) continue;
                double speed = std::clamp(*v, kLeastSpeed, kMostSpeed);
                if (speed != *v)
                    LOGW("balance.toml: weapons.%s.%s %g is out of %g to %g, %g it is", kWeaponNames[w],
                         kAnimationKinds[k].key, *v, kLeastSpeed, kMostSpeed, speed);
                g_speeds[k][w] = speed;
            }
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

// From an animation of a kind starting (a reload's with the weapon in its reload state) the clip's speed is written
// each frame, the game may write its own, and given back once that animation ends or its speed is 1 again
constexpr uint32_t kAnimationNameBytes = 26;  // the controller's name, to its second copy at +0x76
struct SpedClip {
    uint32_t clip = 0;  // 0 none
    size_t kind = 0;
    float before = 0;   // its speed before
    float set = 0;      // the speed written
};
SpedClip g_sped;
using Speeds = std::array<float, kKinds>;

// the record's place in the array, kWeaponRecords for none
uint32_t RecordIndex(uint32_t data) {
    if (data < kWeaponRecordsAt) return kWeaponRecords;
    uint32_t at = data - kWeaponRecordsAt;
    return at % kWeaponRecordSize ? kWeaponRecords : std::min(at / kWeaponRecordSize, kWeaponRecords);
}

// the weapon in hand's speed of each kind: the override, else its own with the kind's toggle on, else 1
Speeds AnimationSpeeds(uint32_t weapon) {
    Speeds speeds;
    uint32_t w = weapon ? RecordIndex(R32(weapon + kWeaponData)) : kWeaponRecords;
    for (size_t k = 0; k < kKinds; k++) {
        if (g_speed_override > 0) speeds[k] = g_speed_override;
        else if (!(game::Opt().*kAnimationKinds[k].toggle) || w >= kWeaponRecords) speeds[k] = 1.0f;
        else speeds[k] = float(g_speeds[k][w].value_or(1.0));
    }
    return speeds;
}

// the kind of an animation by its name, kKinds for none
size_t KindOf(const std::string& name) {
    for (size_t k = 0; k < kKinds; k++) {
        for (const char* suffix : kAnimationKinds[k].suffixes) {
            size_t n = suffix ? std::strlen(suffix) : 0;
            if (n && name.size() >= n && name.compare(name.size() - n, n, suffix) == 0) return k;
        }
    }
    return kKinds;
}

// the weapon is in a state the kind starts in
bool StartsIn(const AnimationKind& kind, uint32_t weapon) {
    if (!kind.states[0]) return true;
    uint32_t state = weapon ? R32(weapon + kWeaponState) : 0;
    return std::find(std::begin(kind.states), std::end(kind.states), state) != std::end(kind.states);
}

// the player's animation at its kind's speed times the game's while it plays, the game's speed given back once it
// ends; a speed of 1 leaves the game's
void ApplyAnimationSpeeds(uint32_t weapon, const Speeds& speeds) {
    uint32_t ac = R32(Player() + kCharacterAnimation);
    uint32_t clip = ac ? R32(ac + kAnimClip) : 0;
    std::string name = ac ? ReadCString(ac + kAnimName, kAnimationNameBytes) : "";
    size_t kind = clip ? KindOf(name) : kKinds;
    if (g_sped.clip && (kind != g_sped.kind || clip != g_sped.clip || speeds[g_sped.kind] == 1.0f)) {
        LOGI("%s speed: clip %08X back to speed %g, \"%s\" playing", kAnimationKinds[g_sped.kind].key, g_sped.clip,
             g_sped.before, name.c_str());
        G<float>(g_sped.clip + kAnimClipSpeed) = g_sped.before;
        g_sped.clip = 0;
    }
    if (!g_sped.clip && kind < kKinds && speeds[kind] != 1.0f && StartsIn(kAnimationKinds[kind], weapon)) {
        float before = G<float>(clip + kAnimClipSpeed);
        g_sped = {clip, kind, before, (before != 0 ? before : 1.0f) * speeds[kind]};
        G<float>(clip + kAnimClipSpeed) = g_sped.set;
        LOGI("%s speed: clip %08X \"%s\" at frame %g, speed %g -> %g", kAnimationKinds[kind].key, clip, name.c_str(),
             G<float>(clip + kAnimClipFrame), before, g_sped.set);
    } else if (g_sped.clip && G<float>(clip + kAnimClipSpeed) != g_sped.set) {
        G<float>(clip + kAnimClipSpeed) = g_sped.set;
    }
}

}  // namespace

namespace rebalance {

void OverrideAnimationSpeeds(float speed) { g_speed_override = speed; }

void Tick() {
    if (!R32(kWorld)) return;
    if (!g_loaded) {
        Load();
        g_loaded = true;
    }
    if (WeaponToggles() != g_applied && ApplyWeapons()) g_applied = WeaponToggles();
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    ApplyAnimationSpeeds(weapon, AnimationSpeeds(weapon));
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
