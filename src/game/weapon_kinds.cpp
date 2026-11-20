// The game's weapon kinds: each weapon a character holds is one, its name's hash finding its model in the level.
#include <cstdint>
#include <initializer_list>

#include "game/layout.h"
#include "game/weapon_kinds.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kKinds = 0x2D12D0;
constexpr uint32_t kModelName = 0xC09F0;      // cdecl (hash, hash, 0x48000000, 0xE69A1DD7): edx:eax its model's name
constexpr uint32_t kResources = 0x2D45D8;
constexpr uint32_t kFindResource = 0x1F83E0;  // stdcall ([kResources], name, name): the resource, 0 none

uint32_t Table() {
    uint32_t holder = R32(kKinds);
    return holder ? R32(holder) : 0;
}

// a guest call's edx:eax, which CallGuest does not keep: the registers and the stack given back as it does
uint64_t CallGuest64(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    X86Context saved = ctx;
    for (auto p = args.end(); p != args.begin();) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    W32(ctx.esp, 0xFFFFFFF0u);
    x86_icall(ctx, g_base, fn);
    uint64_t result = uint64_t(ctx.edx) << 32 | ctx.eax;
    ctx = saved;
    return result;
}

}  // namespace

namespace weapon_kinds {

uint32_t Count() {
    uint32_t table = Table();
    return table ? G<uint8_t>(table + 1) : 0;
}

uint32_t Entry(uint32_t kind) {
    uint32_t table = Table();
    return table && kind < Count() ? R32(table + 4) + kind * kKindSize : 0;
}

uint32_t OfRecord(uint32_t record) {
    for (uint32_t kind = 0; kind < Count(); kind++)
        if (R32(Entry(kind) + 8) == record) return kind;
    return Count();
}

uint32_t Model(X86Context& ctx, uint32_t entry) {
    uint64_t name = CallGuest64(ctx, kModelName, {R32(entry), R32(entry + 4), 0x48000000, 0xE69A1DD7});
    return CallGuest(ctx, kFindResource, {R32(kResources), uint32_t(name), uint32_t(name >> 32)});
}

}  // namespace weapon_kinds
