// New Game+: the level load (sub_001EDC90) sets kWorldNewGamePlus for a story level when the game was finished on
// this difficulty or a harder one, or on Black Ops, and kWorldStartWeapon when Black Ops was finished or is played.
#include <cstdint>

#include "game/layout.h"
#include "game/new_game_plus.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace new_game_plus {

bool Active() {
    uint32_t world = R32(kWorld);
    return world && G<uint8_t>(world + kWorldLevel) < kStoryLevels && G<uint8_t>(world + kWorldNewGamePlus);
}

}  // namespace new_game_plus
