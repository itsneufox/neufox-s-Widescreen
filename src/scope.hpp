#pragma once
#include "config.hpp"

#include <cstdint>

// A scope is the extent of one game draw function (CHud::DrawRadar,
// CMenuManager::DrawFrontEnd, ...). While it runs, everything it draws goes
// through one affine transform around an anchor:  x' = ax + (x - ax) * kx.
// Draws outside any scope (SA-MP, CLEO, other mods) are left untouched.
namespace scope
{
    enum Flags : uint32_t
    {
        kKeepFullWidth = 1 << 0,  // full-width quads (fades, backgrounds) stay full width
        kFillSides = 1 << 1,      // black out the sides left uncovered by a full-height overlay
        kEdgeAnchors = 1 << 2,    // decoration in the top/bottom bands sticks to the nearest side edge
        kAutoAnchors = 1 << 3,    // every element sticks to the side edge of the third it is in
        kPinScreenEdges = 1 << 4, // untextured fills keep the corners that touch the screen's side edges there
    };

    using config::Fit;  // for full-screen artwork (menu backgrounds, loading screens)

    struct Xform
    {
        float ax = 0.0f, kx = 1.0f, tx = 0.0f;
        float ay = 0.0f, ky = 1.0f, ty = 0.0f;
        float edgeLeft = 0.0f, edgeRight = 0.0f;  // kEdgeAnchors / kAutoAnchors targets
        uint32_t flags = 0;
        Fit fit = Fit::Stretch;

        bool Identity() const { return kx == 1.0f && ky == 1.0f && tx == 0.0f && ty == 0.0f; }
        float X(float x) const { return ax + (x - ax) * kx + tx; }
        float Y(float y) const { return ay + (y - ay) * ky + ty; }
    };

    // Anchor for one element with these (untransformed) bounds: the scope's
    // own, or a side edge for kEdgeAnchors decoration (menu titles, header
    // pictures, footer hints) that would otherwise float inside the 4:3 area.
    float AnchorX(const Xform& xf, float minX, float maxX, float minY, float maxY);

    struct Frame;

    struct Hook
    {
        const char* name;
        uintptr_t addr;
        uint8_t len;
        uint8_t expect[15];
        Xform (*make)();                 // transform for this call
        void (*leave)(const Frame&) = nullptr;  // optional, after the function returns
        void* trampoline = nullptr;
        uint8_t* thunk = nullptr;
        int8_t relAt = -1;  // offset of a call/jmp rel32 inside the prologue, if any
    };

    struct Frame
    {
        Hook* hook;
        uint32_t ret;
        Xform xf;
        // Union of everything drawn, after transform (for kFillSides).
        bool drew;
        float minX, minY, maxX, maxY;
    };

    // Detours `h.addr` so every call, from any caller and in any calling
    // convention, runs inside the scope.
    bool Install(Hook& h);

    // `leave` for kFillSides scopes: when what was drawn spans the full
    // height (a scope overlay, pillarboxed artwork), black out both sides.
    void FillSides(const Frame& f);

    Frame* Top();
    // For C++ wrappers: draw the inner call untransformed.
    void PushIdentity();
    // For drawing outside a hooked function under a given transform.
    void Push(const Xform& xf);
    void Pop();
}
