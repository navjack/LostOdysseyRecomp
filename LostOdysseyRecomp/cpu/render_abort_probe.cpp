// Diagnostic: identify what calls the CRT abort() before the guest reaches KeBugCheck.
//
// On macOS/Metal a battle-turn transition ended in guest KeBugCheck(0): the UE3 rendering thread
// (RenderingThreadMain 0x824856A0) dispatched a render command through vtable slot +4 at
// 0x824857D8 and landed in the CRT abort() (0x82B7ADC0, followed by doexit -> KeBugCheck). A
// vtable slot that resolves to abort is the pure-virtual-call pattern: the command object was
// still being constructed or already destroyed by the game thread while the rendering thread
// executed it. This hook records the caller, the object, its vtable and payload, then runs the
// original abort unchanged, so the next reproduction names the command type.

#include <stdafx.h>
#include <os/logger.h>

extern "C" PPC_FUNC(__imp__sub_82B7ADC0); // CRT abort()

namespace
{
    constexpr uint32_t kRenderCommandDispatchReturn = 0x824857DC; // after bctrl in RenderingThreadMain

    // Any guest address above the null guard; the E alias (0xE0000000+) is unmapped on hosts
    // with 16 KiB pages. Render commands live in GRenderCommandBuffer at 0x700000.
    bool Readable(uint32_t address, uint32_t size)
    {
        return address >= 0x10000 && address < 0xE0000000 && address + size < 0xE0000000 && (address & 3) == 0;
    }
}

PPC_FUNC(sub_82B7ADC0)
{
    const uint32_t lr = uint32_t(ctx.lr);
    const uint32_t object = ctx.r3.u32;
    const bool renderDispatch = lr == kRenderCommandDispatchReturn;
    LOG_ERROR("guest abort() caller={:#x}{} r3={:#x} r4={:#x} r5={:#x} r31={:#x}", lr,
        renderDispatch ? " (render command dispatch: probable pure virtual call)" : "",
        object, ctx.r4.u32, ctx.r5.u32, ctx.r31.u32);
    if (Readable(object, 64))
    {
        const uint32_t vtable = PPC_LOAD_U32(object);
        std::string payload;
        for (uint32_t offset = 0; offset < 64; offset += 4)
            payload += fmt::format(" {:08x}", PPC_LOAD_U32(object + offset));
        LOG_ERROR("guest abort() object {:#x}:{}", object, payload);
        if (Readable(vtable, 16))
            LOG_ERROR("guest abort() vtable {:#x}: slot0={:#x} slot4={:#x} slot8={:#x} slot12={:#x}", vtable,
                PPC_LOAD_U32(vtable), PPC_LOAD_U32(vtable + 4), PPC_LOAD_U32(vtable + 8), PPC_LOAD_U32(vtable + 12));
    }
    __imp__sub_82B7ADC0(ctx, base);
}
