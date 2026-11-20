#pragma once
#include <cstdint>

// rebalance.cpp: balance.toml beside the exe, each weapon's clip and the hits each enemy type takes, by difficulty
namespace rebalance {

void Tick();  // game thread, once a frame of play: reads the file and applies the clips once the first level loads

}  // namespace rebalance
