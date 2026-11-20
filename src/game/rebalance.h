#pragma once
#include <cstdint>

// rebalance.cpp: balance.toml beside the exe, each weapon's clip and the hits each enemy type takes, by difficulty
namespace rebalance {

void Tick();  // game thread, once a frame of play: reads the file and applies the clips once the first level loads
// game thread, once a frame of play: the player's reload animation at speed times the game's while the weapon in
// hand reloads, the game's speed given back once it ends; 1 leaves the game's
void ApplyReloadSpeed(uint32_t weapon, float speed);

}  // namespace rebalance
