#pragma once

// new_game_plus.cpp: the level the game loads counts as New Game+ (layout::kWorldNewGamePlus as the game set it),
// and there the New Game+ Settings: silver weapons, infinite ammo, the starting weapon
namespace new_game_plus {

bool Active();  // the level loaded last is a story level and the game made it New Game+
// the weapon record the player starts this level with, the Starting weapon setting's; -1 the game's own
int StartWeaponRecord();

}  // namespace new_game_plus
