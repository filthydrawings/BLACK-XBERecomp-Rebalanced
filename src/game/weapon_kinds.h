#pragma once
#include <cstdint>

struct X86Context;

// weapon_kinds.cpp: the game's table of weapon kinds, [[0x2D12D0]]: +1 byte the count, +4 the kinds, kKindSize each,
// +0 and +4 its name's hash, +8 its weapon data. A weapon object's byte +0 is its kind's place in the table
namespace weapon_kinds {

constexpr uint32_t kKindSize = 0x20;
constexpr uint32_t kWeaponKind = 0;  // byte, in the weapon object

uint32_t Count();
uint32_t Entry(uint32_t kind);  // 0 for none
uint32_t OfRecord(uint32_t record);  // the first kind whose data is that weapon record (layout::WeaponRecord), Count() none
// the level's model of the kind at entry, as sub_001C5CD0 finds it, 0 when the level has not loaded it; game thread
uint32_t Model(X86Context& ctx, uint32_t entry);

}  // namespace weapon_kinds
