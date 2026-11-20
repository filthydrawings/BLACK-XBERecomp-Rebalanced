#pragma once
#include <cstdint>

// rebalance.cpp: balance.toml beside the exe, each weapon's clip and the hits each enemy type takes, by difficulty
namespace rebalance {

void Tick();  // game thread, once a frame of play: reads the file and applies the clips once the first level loads
// every sped up kind of animation (reloads) at that speed whatever the settings and balance.toml, the research
// logger's F11; 0 for none
void OverrideAnimationSpeeds(float speed);

}  // namespace rebalance
