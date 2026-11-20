#pragma once
#include <cstdint>

// borrowed_weapons.cpp: a weapon's first person files from another level's folder when this level's lacks them, and
// no pickup when one this stage has none of is dropped
namespace borrowed_weapons {

// before NtCreateFile opens the name at text, length long: Levels\Level_NN\fpguns\ rewritten to a level that has it
void Redirect(uint32_t text, uint16_t length, uint32_t root);

}  // namespace borrowed_weapons
