// Weapons a level does not have: their first person files are the same in every level folder that has them
// (Levels\Level_NN\fpguns\), and the game opens them as a weapon comes into the hand. One the level's folder lacks is
// opened from another level's folder, its name rewritten in place: the digits of Level_NN, same length. The level's
// pickups are its own stage's (Stg_NNNN\Guns.bin, Guns_s.bin in New Game+), so a weapon dropped that this stage has
// no pickup model of leaves none, and the weapon is gone: sub_001E0EB0 would make it the list's first pickup, or one
// whose model the stage lacks, which crashes.
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "game/borrowed_weapons.h"
#include "game/layout.h"
#include "game/new_game_plus.h"
#include "game/weapon_kinds.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

constexpr int kLevels = 100;
constexpr uint32_t kPickups = 0x2D0D64;     // -> +kPickupList -> +0 byte the count, +8 the pickups
constexpr uint32_t kPickupList = 0x5844;
constexpr uint32_t kPickupModels = 0x5800;  // -> a model each pickup, 0 until looked up
constexpr uint32_t kPickupSize = 0x18;      // +8 +0xC its model's name, +0x10 byte its weapon kind
constexpr uint32_t kPickupName = 0x08;
constexpr uint32_t kPickupKind = 0x10;
constexpr uint32_t kFindResource = 0x1F83E0;  // stdcall ([kResources], name, name): the resource, 0 none

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool Exists(const std::string& name, uint32_t root) {
    std::string host = ResolvePath(name, root);
    std::error_code ec;
    return !host.empty() && std::filesystem::is_regular_file(host, ec);
}


// The Starting weapon's pickup model, when this stage has none: the level load (sub_001EDC90) queues the stage's own
// Guns.bin on loading request 8, files it (sub_001D3790) in a type 0xC slot keyed by level and New Game+, and finds it
// (world +0x5B34) before setting up its pickups. Right there (0x1EE310) one more step is put in: the Guns.bin or
// Guns_s.bin of a level that has the model is queued the same way and the loader held, as its own steps hold, until
// the pack is filed; its slot then gets a key of its own. The pickups set up next find the model, so the weapon drops
// as a pickup. At the level's end (sub_001EBD60) our slot's use goes down as the stage's own does. Anything unexpected
// leaves the weapon to vanish when dropped.
constexpr uint32_t kResources = 0x2D45D8;
constexpr uint32_t kQueue = 0x1F37E0;       // eax the request, ecx the path, edx [kResources], 6 arguments
constexpr uint32_t kFileIn = 0x1D3790;      // the callback: the pack filed in a type 0xC slot
constexpr uint32_t kRequest = 8;            // the request the level load queues on
constexpr uint32_t kRequestSize = 0x13C;    // +0x134 byte busy
constexpr uint32_t kRequestBusy = 0x134;
constexpr uint32_t kSlots = 0xD14;          // kSlotCount of kSlotSize: +0 type, +4 the file, +8 key, +0xA uses
constexpr uint32_t kSlotCount = 17;
constexpr uint32_t kSlotSize = 0x0C;
constexpr uint32_t kGunsType = 0x0C;
constexpr uint32_t kPackPlaces = 0xC64;     // two lists of kPackPlaceCount
constexpr uint32_t kPackPlaces2 = 0xC90;
constexpr uint32_t kPackPlaceCount = 11;
constexpr uint32_t kWorldGuns = 0x5B34;
constexpr uint32_t kOurKey = 0x7E00;        // + the kind: a key no level has
constexpr int kMostWaits = 1200;            // calls of the load step before giving up

enum Step { kIdle, kQueued, kDone };
Step g_step = kIdle;
uint32_t g_level_world;    // the world whose load the step is for
int g_waits;
uint8_t g_kind;
uint32_t g_path;           // guest room for the path
std::chrono::steady_clock::time_point g_queued_at;

uint32_t Slot(uint32_t resources, uint32_t s) { return resources + kSlots + s * kSlotSize; }

void Counts(uint32_t resources, uint32_t& slots, uint32_t& packs, uint32_t& packs2) {
    slots = packs = packs2 = 0;
    for (uint32_t s = 0; s < kSlotCount; s++) slots += R32(Slot(resources, s) + 4) != 0;
    for (uint32_t p = 0; p < kPackPlaceCount; p++) {
        packs += R32(resources + kPackPlaces + p * 4) != 0;
        packs2 += R32(resources + kPackPlaces2 + p * 4) != 0;
    }
}

void LogCounts(const char* when) {
    uint32_t resources = R32(kResources);
    if (!resources) return;
    uint32_t slots, packs, packs2;
    Counts(resources, slots, packs, packs2);
    LOGR("borrowed pickup: %s: %u of %u slots, pack places %u and %u of %u", when, slots, kSlotCount, packs, packs2,
         kPackPlaceCount);
}

// the pickup model's name of a weapon kind, from the pickup list
bool PickupName(uint8_t kind, uint32_t& lo, uint32_t& hi) {
    uint32_t pickups = R32(kPickups);
    uint32_t list = pickups ? R32(pickups + kPickupList) : 0;
    uint8_t count = list ? G<uint8_t>(list) : 0;
    uint32_t entries = list ? R32(list + 8) : 0;
    for (uint8_t i = 0; i < count && entries; i++) {
        uint32_t entry = entries + i * kPickupSize;
        if (G<uint8_t>(entry + kPickupKind) != kind) continue;
        lo = R32(entry + kPickupName);
        hi = R32(entry + kPickupName + 4);
        return true;
    }
    return false;
}

// a level's Guns.bin or Guns_s.bin on disk holds the model
bool PackHas(int level, bool silver, uint32_t lo, uint32_t hi) {
    char name[96];
    std::snprintf(name, sizeof name, "D:\\Levels\\Level_%02d\\Stg_0001\\Guns%s.bin", level, silver ? "_s" : "");
    std::string host = ResolvePath(name, 0);
    if (host.empty()) return false;
    FILE* f = _wfopen(std::filesystem::path(host).c_str(), L"rb");
    if (!f) return false;
    uint32_t entry[4];
    bool found = false;
    std::fseek(f, 0x100, SEEK_SET);
    while (!found && std::fread(entry, 4, 4, f) == 4 && (entry[1] >> 16) == 0x5446)
        found = entry[0] == lo && entry[1] == hi;
    std::fclose(f);
    return found;
}

// true holds the level load at this step, false lets it go on
bool StagePickupStep(X86Context& ctx) {
    uint32_t world = ctx.ebp;
    uint32_t resources = R32(kResources);
    if (!world || !resources) return false;
    if (world != g_level_world || g_step == kDone) {
        if (world != g_level_world) {
            g_level_world = world;
            g_step = kIdle;
        } else {
            return false;
        }
    }
    uint16_t key = uint16_t(G<uint8_t>(world + kWorldLevel) | G<uint8_t>(world + kWorldNewGamePlus) << 8);
    if (g_step == kQueued) {
        if (++g_waits > kMostWaits) {
            LOGW("borrowed pickup: the pack was not filed after %d steps, the weapon vanishes when dropped", g_waits);
            g_step = kDone;
            return false;
        }
        if (G<uint8_t>(resources + kRequest * kRequestSize + kRequestBusy)) return true;
        for (uint32_t s = 0; s < kSlotCount; s++) {
            uint32_t slot = Slot(resources, s);
            uint32_t file = R32(slot + 4);
            if (!file || R32(slot) != kGunsType || G<uint16_t>(slot + 8) != key || file == R32(world + kWorldGuns))
                continue;
            G<uint16_t>(slot + 8) = uint16_t(kOurKey + g_kind);
            G<uint16_t>(slot + 0xA) = 1;
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_queued_at).count();
            LOGR("borrowed pickup: kind %u's pack filed in slot %u after %.0f ms, key %04X", g_kind, s, ms,
                 kOurKey + g_kind);
            LogCounts("after loading it");
            g_step = kDone;
            return false;
        }
        if (g_waits > 2) {  // the request is idle and no new slot: the callback did not file it
            LOGW("borrowed pickup: the load finished but no slot holds the pack, the weapon vanishes when dropped");
            g_step = kDone;
        }
        return g_step != kDone;
    }
    // kIdle: is there anything to borrow?
    g_step = kDone;
    int record = new_game_plus::StartWeaponRecord();
    if (record < 0) return false;
    uint32_t kind = weapon_kinds::OfRecord(WeaponRecord(uint32_t(record)));
    if (kind >= weapon_kinds::Count()) return false;
    uint32_t lo = 0, hi = 0;
    if (!PickupName(uint8_t(kind), lo, hi)) {
        LOGW("borrowed pickup: no pickup entry for kind %u", kind);
        return false;
    }
    int level = G<uint8_t>(world + kWorldLevel);
    bool silver = G<uint8_t>(world + kWorldNewGamePlus);
    if (PackHas(level, silver, lo, hi)) return false;  // the stage has it
    int from = -1;
    bool from_silver = false;
    for (int l = 0; l < 100 && from < 0; l++) {
        if (l == level) continue;
        for (bool s : {silver, !silver})
            if (from < 0 && PackHas(l, s, lo, hi)) {
                from = l;
                from_silver = s;
            }
    }
    LogCounts("at the stage's load");
    if (from < 0) {
        LOGW("borrowed pickup: no level has kind %u's pickup model %08X%08X", kind, hi, lo);
        return false;
    }
    uint32_t slots, packs, packs2;
    Counts(resources, slots, packs, packs2);
    if (slots >= kSlotCount || packs >= kPackPlaceCount || packs2 >= kPackPlaceCount) {
        LOGW("borrowed pickup: no room for another pack, the weapon vanishes when dropped");
        return false;
    }
    if (G<uint8_t>(resources + kRequest * kRequestSize + kRequestBusy)) {
        LOGW("borrowed pickup: the loading request is busy, the weapon vanishes when dropped");
        return false;
    }
    if (!g_path) g_path = mem::AllocSystem(0x100);
    char* path = static_cast<char*>(HostPtr(g_path));
    std::snprintf(path, 0x100, "Levels\\Level_%02d\\Stg_0001\\Guns%s.bin", from, from_silver ? "_s" : "");
    LOGR("borrowed pickup: kind %u (%s), model %08X%08X, loading %s", kind, kWeaponNames[record], hi, lo, path);
    X86Context saved = ctx;
    ctx.eax = kRequest;
    ctx.ecx = g_path;
    ctx.edx = resources;
    CallGuest(ctx, kQueue, {1, kFileIn, world, 0x17, 1, 0x2000000});
    ctx = saved;
    g_kind = uint8_t(kind);
    g_waits = 0;
    g_queued_at = std::chrono::steady_clock::now();
    g_step = kQueued;
    return true;
}

}  // namespace

namespace borrowed_weapons {

void Redirect(uint32_t text, uint16_t length, uint32_t root) {
    if (!text || !length) return;
    std::string name(static_cast<const char*>(HostPtr(text)), length);
    std::string lower = Lower(name);
    size_t guns = lower.find("\\fpguns\\");
    size_t level = lower.rfind("level_", guns);
    if (guns == std::string::npos || level == std::string::npos || guns != level + 8) return;
    size_t digits = level + 6;
    if (!std::isdigit(uint8_t(name[digits])) || !std::isdigit(uint8_t(name[digits + 1]))) return;
    if (Exists(name, root)) return;
    for (int n = 0; n < kLevels; n++) {
        std::string other = name;
        other[digits] = char('0' + n / 10);
        other[digits + 1] = char('0' + n % 10);
        if (other == name || !Exists(other, root)) continue;
        G<char>(text + uint32_t(digits)) = other[digits];
        G<char>(text + uint32_t(digits) + 1) = other[digits + 1];
        LOGR("borrowed weapon: %s from %s", name.c_str(), other.c_str());
        return;
    }
    LOGW("borrowed weapon: %s is in no level's folder", name.c_str());
}

}  // namespace borrowed_weapons

// sub_001E0EB0: a pickup made of a weapon dropped, [esp+4] its kind, eax the pickups. Without a pickup of that kind
// whose model this stage has, none
X86_EXTERN_FUNC(__imp__sub_001E0EB0);
X86_FUNC(sub_001E0EB0) {
    uint8_t kind = G<uint8_t>(ctx.esp + 4);
    uint32_t pickups = ctx.eax;
    uint32_t list = pickups ? R32(pickups + kPickupList) : 0;
    uint8_t count = list ? G<uint8_t>(list) : 0;
    uint32_t entries = list ? R32(list + 8) : 0;
    uint32_t models = pickups ? R32(pickups + kPickupModels) : 0;
    uint32_t model = 0;
    for (uint8_t i = 0; i < count && entries && models; i++) {
        uint32_t entry = entries + i * kPickupSize;
        if (G<uint8_t>(entry + kPickupKind) != kind) continue;
        model = R32(models + i * 4);
        if (!model)
            model = CallGuest(ctx, kFindResource, {R32(kResources), R32(entry + kPickupName), R32(entry + kPickupName + 4)});
        break;
    }
    if (!model) {
        LOGR("borrowed weapon: kind %u dropped, this stage has no pickup model of it, none made", kind);
        Return(ctx, 0, 5);
        return;
    }
    LOGR("weapon dropped: kind %u, pickup model %08X", kind, model);
    __imp__sub_001E0EB0(ctx, base);
}

// sub_001EDC90 at 0x1EE310, ebp the world: the stage's own pickup pack found, its pickups not set up yet. True holds
// the load (on to 0x1EDF95, the step's return, called again next frame)
bool BorrowStagePickups(X86Context& ctx, uint8_t*) { return StagePickupStep(ctx); }

// sub_001EBD60 at 0x1EBEBB, the level ending: the stage's own pickup slot's use just went down. Ours goes down with it
void ReleaseBorrowedPickups(X86Context&, uint8_t*) {
    uint32_t resources = R32(kResources);
    if (!resources) return;
    for (uint32_t s = 0; s < kSlotCount; s++) {
        uint32_t slot = Slot(resources, s);
        uint16_t key = G<uint16_t>(slot + 8);
        if (!R32(slot + 4) || R32(slot) != kGunsType || key < kOurKey || key >= kOurKey + 0x100) continue;
        uint16_t& uses = G<uint16_t>(slot + 0xA);
        if (uses) uses--;
        LOGR("borrowed pickup: level end, slot %u key %04X uses now %u", s, key, uses);
    }
    LogCounts("at the level's end");
}
