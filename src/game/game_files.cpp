// The files the game opens, at each of its 11 calls of NtCreateFile: a weapon's first person file found in another
// level when this one lacks it (borrowed_weapons.cpp), before the call.
#include <cstdint>

#include "game/borrowed_weapons.h"
#include "xdk/hle.h"

using namespace kernel;

// at each call of NtCreateFile: [esp+8] its OBJECT_ATTRIBUTES, +0 the root, +4 the name (length, room, text)
void FileOpening(X86Context& ctx, uint8_t*) {
    uint32_t attrs = R32(ctx.esp + 8);
    uint32_t name = attrs ? R32(attrs + 4) : 0;
    uint32_t text = name ? R32(name + 4) : 0;
    uint16_t length = name ? G<uint16_t>(name) : 0;
    borrowed_weapons::Redirect(text, length, attrs ? R32(attrs) : 0);
}
