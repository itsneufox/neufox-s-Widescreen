#include "scope.hpp"
#include "game.hpp"
#include "im2d.hpp"
#include "mem.hpp"
#include "screen.hpp"

#include <windows.h>
#include <cstring>

// Assembly side (thunks.S): the per-hook thunk pushes its Hook* and jumps to
// ws_scope_enter, which calls ScopeEnter and continues into the trampoline.
// ScopeEnter swaps the caller's return address for ws_scope_exit, so the
// scope closes when the function returns, whatever its calling convention.
extern "C" void ws_scope_enter();
extern "C" void ws_scope_exit();

namespace scope
{
    static constexpr int kMaxDepth = 32;
    static Frame g_stack[kMaxDepth];
    static int g_depth = 0;
    static DWORD g_renderThread = 0;

    Frame* Top() { return g_depth ? &g_stack[g_depth - 1] : nullptr; }

    static Frame* Push(Hook* hook, uint32_t ret, const Xform& xf)
    {
        if (g_depth >= kMaxDepth)
            return nullptr;
        Frame& f = g_stack[g_depth++];
        f = Frame{};
        f.hook = hook;
        f.ret = ret;
        f.xf = xf;
        return &f;
    }

    static int g_overflow = 0;

    void PushIdentity()
    {
        // On overflow the outer transform simply stays active.
        if (!Push(nullptr, 0, Xform{}))
            ++g_overflow;
    }

    void Push(const Xform& xf)
    {
        if (!Push(nullptr, 0, xf))
            ++g_overflow;
    }

    void Pop()
    {
        if (g_overflow)
            --g_overflow;
        else if (g_depth)
            --g_depth;
    }

    float AnchorX(const Xform& xf, float minX, float maxX, float minY, float maxY)
    {
        if (!(xf.flags & (kEdgeAnchors | kAutoAnchors)))
            return xf.ax;
        const float w = screen::Width(), h = screen::Height();
        if ((xf.flags & kEdgeAnchors) && maxY > 0.25f * h && minY < 0.9f * h)
            return xf.ax;  // the interactive middle stays put, so hit-testing holds
        const float centre = 0.5f * (minX + maxX);
        if (centre < w / 3.0f)
            return xf.edgeLeft;
        if (centre > 2.0f * w / 3.0f)
            return xf.edgeRight;
        return xf.ax;
    }

    void FillSides(const Frame& f)
    {
        const float w = screen::Width(), h = screen::Height();
        if (!f.drew || f.minY > 1.0f || f.maxY < h - 1.0f)
            return;
        const game::RGBA black{ 0, 0, 0, 255 };
        PushIdentity();
        if (f.minX > 0.5f)
            game::DrawRect({ 0.0f, h, f.minX, 0.0f }, black);
        if (f.maxX < w - 0.5f)
            game::DrawRect({ f.maxX, h, w, 0.0f }, black);
        Pop();
    }

    bool Install(Hook& h)
    {
        h.thunk = mem::AllocCode(10);
        if (!h.thunk)
            return false;

        // push imm32 <hook>; jmp ws_scope_enter
        h.thunk[0] = 0x68;
        Hook* self = &h;
        std::memcpy(h.thunk + 1, &self, 4);
        h.thunk[5] = 0xE9;
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ws_scope_enter) -
                                           reinterpret_cast<uintptr_t>(h.thunk + 10));
        std::memcpy(h.thunk + 6, &rel, 4);

        h.trampoline = mem::Detour(h.name, h.addr, h.expect, h.len, h.thunk, h.relAt);
        return h.trampoline != nullptr;
    }
}

extern "C" void* __cdecl ScopeEnter(scope::Hook* hook, uint32_t* retSlot)
{
    using namespace scope;

    // All 2D is drawn on the render thread; anything else passes through.
    if (!g_renderThread)
        g_renderThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_renderThread)
        return hook->trampoline;

    // With the fix switched off every scope is the identity: the original 2D.
    Frame* f = Push(hook, *retSlot, hook->make && config::widescreen ? hook->make() : Xform{});
    if (!f)
        return hook->trampoline;

    *retSlot = reinterpret_cast<uint32_t>(&ws_scope_exit);
    im2d::Install();
    return hook->trampoline;
}

extern "C" uint32_t __cdecl ScopeLeave()
{
    using namespace scope;
    Frame f = g_stack[--g_depth];
    if (f.hook->leave)
        f.hook->leave(f);
    return f.ret;
}
