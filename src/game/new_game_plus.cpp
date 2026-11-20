// New Game+: the level load (sub_001EDC90) sets kWorldNewGamePlus for a story level when the game was finished on
// this difficulty or a harder one, or on Black Ops, and kWorldStartWeapon when Black Ops was finished or is played.
// What it decided is kept here as it decides it. The settings unlock once the profile has the game finished on a
// difficulty (kCompletions, what the level load reads); locked, the game is its own everywhere, Black Ops' New
// Game+ included. Unlocked, in a New Game+ level each read of the flag the game makes for them is answered by its
// setting; any other level is the game's own:
//   Silver weapons: the flag itself, written off at the load. It picks the level's data with the silver weapons
//     (sub_001EDC90, sub_001B7390, sub_001D3790, sub_001EBD60) and the silver kind of a weapon given (sub_001D4F30),
//     so it holds from the next level loaded
//   Infinite ammo: the four reads that refill the spare ammo after a shot (0x1EFC0F), the clip when it runs dry
//     (0x1EFCDF) and show 999 on the HUD (0x12CEEA, 0x13A947)
//   Starting weapon: the hash of the player's first weapon as it starts (sub_001DEC70, where Black Ops puts the
//     M16A2), the chosen kind's, then its clip filled and spare ammo given. A weapon the level lacks is borrowed
//     (borrowed_weapons.cpp)
#include <algorithm>
#include <cstdint>

#include "common/options.h"
#include "core/log.h"
#include "game/layout.h"
#include "game/new_game_plus.h"
#include "game/weapon_kinds.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

constexpr uint32_t kCharacterWeapons = 0x298;  // -> the weapon objects, the first the primary
// the loaded profile's progress by difficulty, kCompletionSize each: +0 int, 2 or more the game finished on it.
// All 0 before a profile loads
constexpr uint32_t kCompletions = 0x2D0E64;
constexpr uint32_t kCompletionSize = 0x100;
constexpr uint32_t kDifficulties = 4;
constexpr int32_t kFinished = 2;
constexpr int kSpareClips = 4;

bool g_active;
uint32_t g_given = UINT32_MAX;  // the kind the player was just started with, UINT32_MAX none

bool Unlocked() {
    for (uint32_t d = 0; d < kDifficulties; d++)
        if (int32_t(R32(kCompletions + d * kCompletionSize)) >= kFinished) return true;
    return false;
}

// the game's own while locked
bool InfiniteAmmo() { return g_active && (!Unlocked() || game::Opt().ngp_infinite_ammo); }

void SetLow(uint32_t& reg, bool on) { reg = (reg & ~0xFFu) | (on ? 1u : 0u); }

// the hash of the player's first weapon, hi:lo in two registers, the chosen kind's when this level has its model
void StartWeapon(X86Context& ctx, uint32_t& lo, uint32_t& hi) {
    int chosen = new_game_plus::StartWeaponRecord();
    if (ctx.ebx != Player() || chosen < 0) return;
    uint32_t record = uint32_t(chosen);
    uint32_t kind = weapon_kinds::OfRecord(WeaponRecord(record));
    uint32_t entry = weapon_kinds::Entry(kind);
    if (!entry) {
        LOGR("new game+: no weapon kind for %s, the game's start weapon", kWeaponNames[record]);
        return;
    }
    X86Context saved = ctx;
    uint32_t model = weapon_kinds::Model(ctx, entry);
    ctx = saved;
    LOGR("new game+: starting with %s (kind %u) for %08X%08X%s", kWeaponNames[record], kind, hi, lo,
         model ? "" : ", borrowed: this stage has no pickup of it");
    lo = R32(entry);
    hi = R32(entry + 4);
    g_given = kind;
}

}  // namespace

namespace new_game_plus {

bool Active() { return g_active; }

int StartWeaponRecord() {
    uint32_t choice = game::Opt().ngp_start_weapon;
    if (!g_active || !Unlocked() || !choice || choice > kWeaponRecords) return -1;
    return int(choice - 1);
}

}  // namespace new_game_plus

bool game::NewGamePlusUnlocked() { return Unlocked(); }

// sub_001EDC90 at 0x1EE1FF, ebp the world: both flags set, the level's data not looked up yet
void NewGamePlusFlags(X86Context& ctx, uint8_t*) {
    uint32_t world = ctx.ebp;
    uint8_t level = G<uint8_t>(world + kWorldLevel);
    g_active = level < kStoryLevels && G<uint8_t>(world + kWorldNewGamePlus);
    bool silver = g_active && (!Unlocked() || game::Opt().ngp_silver_weapons);
    if (g_active && !silver) G<uint8_t>(world + kWorldNewGamePlus) = 0;
    LOGR("new game+: level %u, %s, settings %s (finished %d %d %d %d), silver weapons %s, infinite ammo %s, start "
         "weapon flag %u",
         level, g_active ? "on" : "off", Unlocked() ? "unlocked" : "locked", int32_t(R32(kCompletions)),
         int32_t(R32(kCompletions + kCompletionSize)), int32_t(R32(kCompletions + 2 * kCompletionSize)),
         int32_t(R32(kCompletions + 3 * kCompletionSize)), silver ? "on" : "off", InfiniteAmmo() ? "on" : "off",
         G<uint8_t>(world + kWorldStartWeapon));
}

// after each read of the flag for ammo, its register: the Infinite ammo setting in New Game+, off elsewhere
void InfiniteAmmoAl(X86Context& ctx, uint8_t*) { SetLow(ctx.eax, InfiniteAmmo()); }
void InfiniteAmmoCl(X86Context& ctx, uint8_t*) { SetLow(ctx.ecx, InfiniteAmmo()); }
void InfiniteAmmoDl(X86Context& ctx, uint8_t*) { SetLow(ctx.edx, InfiniteAmmo()); }

// sub_001DEC70 at 0x1DEE95, a character starting: ebx it, eax:ecx the hash of its first weapon
void StartWeaponSpawn(X86Context& ctx, uint8_t*) { StartWeapon(ctx, ctx.eax, ctx.ecx); }

// sub_001DEC70 at 0x1DED51, a character starting again: ebx it, eax:esi the hash of the weapon it is given
void StartWeaponRespawn(X86Context& ctx, uint8_t*) { StartWeapon(ctx, ctx.eax, ctx.esi); }

// sub_001DEC70 at 0x1DF077, its weapons made and the first one's clip set from the level: the chosen weapon's clip
// filled and its spare ammo given
void StartWeaponAmmo(X86Context& ctx, uint8_t*) {
    uint32_t kind = g_given;
    g_given = UINT32_MAX;
    if (kind == UINT32_MAX || ctx.ebx != Player()) return;
    uint32_t weapons = R32(ctx.ebx + kCharacterWeapons);
    uint32_t weapon = weapons ? R32(weapons) : 0;
    if (!weapon || G<uint8_t>(weapon + weapon_kinds::kWeaponKind) != kind) return;
    uint32_t data = R32(weapon + kWeaponData);
    int32_t clip = data ? int32_t(R32(data + kDataClip)) : 0;
    uint32_t magazine = R32(weapon + kWeaponMagazine);
    if (magazine && clip > 0) G<uint16_t>(magazine + kMagazineRounds) = uint16_t(clip);
    uint32_t spare = R32(weapon + kWeaponSpareAmmo);
    if (spare && data && clip > 0) {
        uint16_t& left = G<uint16_t>(spare + R32(data + kDataAmmoType) * 2);
        left = std::max<uint16_t>(left, uint16_t(std::min(clip * kSpareClips, 0xFFFF)));
    }
    LOGR("new game+: start weapon clip %d, spare %u", clip,
         spare && data ? G<uint16_t>(spare + R32(data + kDataAmmoType) * 2) : 0);
}
