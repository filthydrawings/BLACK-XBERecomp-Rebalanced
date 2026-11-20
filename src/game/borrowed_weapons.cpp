// Weapons a level does not have: their first person files are the same in every level folder that has them
// (Levels\Level_NN\fpguns\), and the game opens them as a weapon comes into the hand. One the level's folder lacks is
// opened from another level's folder, its name rewritten in place: the digits of Level_NN, same length. The level's
// pickups are its own stage's (Stg_NNNN\Guns.bin, Guns_s.bin in New Game+), so a weapon dropped that this stage has
// no pickup model of leaves none, and the weapon is gone: sub_001E0EB0 would make it the list's first pickup, or one
// whose model the stage lacks, which crashes.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "game/borrowed_weapons.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr int kLevels = 100;
constexpr uint32_t kPickups = 0x2D0D64;     // -> +kPickupList -> +0 byte the count, +8 the pickups
constexpr uint32_t kPickupList = 0x5844;
constexpr uint32_t kPickupModels = 0x5800;  // -> a model each pickup, 0 until looked up
constexpr uint32_t kPickupSize = 0x18;      // +8 +0xC its model's name, +0x10 byte its weapon kind
constexpr uint32_t kPickupName = 0x08;
constexpr uint32_t kPickupKind = 0x10;
constexpr uint32_t kResources = 0x2D45D8;
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
    __imp__sub_001E0EB0(ctx, base);
}
