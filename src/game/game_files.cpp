// The files the game opens, at each of its 11 calls of NtCreateFile: the name before the call, the result after it,
// handed to the research logger.
#include <cstdint>
#include <string>

#include "game/research.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

std::string g_opening;
uint32_t g_opening_root;

}  // namespace

// at each call of NtCreateFile: [esp+8] its OBJECT_ATTRIBUTES, +0 the root, +4 the name (length, room, text)
void FileOpening(X86Context& ctx, uint8_t*) {
    uint32_t attrs = R32(ctx.esp + 8);
    uint32_t name = attrs ? R32(attrs + 4) : 0;
    uint32_t text = name ? R32(name + 4) : 0;
    uint16_t length = name ? G<uint16_t>(name) : 0;
    g_opening.assign(text ? static_cast<const char*>(HostPtr(text)) : "", text ? length : 0);
    g_opening_root = attrs ? R32(attrs) : 0;
}

// after it: eax its status
void FileOpened(X86Context& ctx, uint8_t*) { research::FileOpened(g_opening, g_opening_root, ctx.eax); }
