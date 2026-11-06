#include "features.hpp"
#include "mem.hpp"
#include "scope.hpp"
#include "screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

extern "C" void ws_menu_mouse_x();

// The menus are drawn as one 4:3 picture in the middle of the screen. Their
// hit-testing compares the mouse with the untransformed layout, so the mouse
// is moved into that space as it arrives from the window procedure; the
// cursor is drawn inside the scope and lands back under the real pointer.
namespace
{
    // The map page frames the map at x = 60..580 of the 640-wide canvas. The
    // page is narrowed to 4:3 like the other menus, so the map keeps its
    // shape; the frame is widened by the inverse amount wherever the game
    // reads it (drawing, tile streaming, scroll limits, cursor and mouse), so
    // after narrowing it reaches the screen's real 60..580 positions and the
    // map fills the width instead of the middle 4:3.
    constexpr float kMapLeft = 60.0f, kMapRight = 580.0f, kCanvasCentre = 320.0f, kCanvasWidth = 640.0f;
    float g_mapLeft = kMapLeft, g_mapRight = kMapRight;
    float g_mapLeftScale = kMapLeft / kCanvasWidth, g_mapRightScale = kMapRight / kCanvasWidth;
    // How far the map cursor may go (x = 64..576), widened the same way.
    constexpr float kCursorLeft = 64.0f, kCursorRight = 576.0f;
    float g_cursorLeft = kCursorLeft, g_cursorRight = kCursorRight;

    void UpdateMapFrame(float kx)
    {
        g_mapLeft = kCanvasCentre - (kCanvasCentre - kMapLeft) / kx;
        g_mapRight = kCanvasCentre + (kMapRight - kCanvasCentre) / kx;
        g_mapLeftScale = g_mapLeft / kCanvasWidth;
        g_mapRightScale = g_mapRight / kCanvasWidth;
        g_cursorLeft = kCanvasCentre - (kCanvasCentre - kCursorLeft) / kx;
        g_cursorRight = kCanvasCentre + (kCursorRight - kCanvasCentre) / kx;
    }

    // CMenuManager::StretchX(60 / 580) in the map's mouse hit test.
    float __thiscall MapMouseLeft(void*, float) { return g_mapLeft * screen::Width() / kCanvasWidth; }
    float __thiscall MapMouseRight(void*, float) { return g_mapRight * screen::Width() / kCanvasWidth; }

    scope::Xform Menu()
    {
        scope::Xform xf;
        if (config::widescreen && config::menuLayout != config::MenuLayout::Stretch)
            xf.kx = std::min(1.0f, screen::HudSquash());
        UpdateMapFrame(config::wideMap ? xf.kx : 1.0f);
        xf.ax = 0.5f * screen::Width();
        xf.flags = scope::kKeepFullWidth;
        xf.fit = config::menuBackground;
        return xf;
    }

    // The map page: one picture made of many tiles (no edge anchors), framed
    // by black masks that must keep reaching the screen edges.
    scope::Xform Map()
    {
        scope::Xform xf = Menu();
        // Zoomed in, one tile can cover the whole screen: it is part of the
        // map, not background artwork, so it is narrowed like the others.
        xf.flags = scope::kPinScreenEdges;
        xf.fit = scope::Fit::Stretch;
        return xf;
    }

    // Loading screens are 640x448 layouts too; their artwork keeps its own
    // aspect ratio, with black bars when pillarboxed.
    scope::Xform Loading()
    {
        scope::Xform xf = Menu();
        xf.flags |= scope::kFillSides;
        xf.fit = config::loadingScreens;
        return xf;
    }

    // The menus themselves: the interactive middle as one 4:3 block (the mouse
    // is remapped to match), titles, header pictures and footers at the edges.
    scope::Xform Menus()
    {
        scope::Xform xf = Menu();
        if (config::menuLayout == config::MenuLayout::Edges)
        {
            const screen::Span region = screen::Region(config::menuMaxAspect);
            xf.flags |= scope::kEdgeAnchors;
            xf.edgeLeft = region.left;
            xf.edgeRight = region.right;
        }
        return xf;
    }

    scope::Hook g_drawFrontEnd = { "CMenuManager::DrawFrontEnd", 0x57C290, 6, { 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x32 }, &Menus };
    scope::Hook g_printMap = { "CMenuManager::PrintMap", 0x575130, 5, { 0x83, 0xEC, 0x54, 0x53, 0x55 }, &Map };
    scope::Hook g_loading[] = {
        // sub esp, 18h; call rel32 (relocated into the trampoline)
        { "CLoadingScreen::RenderSplash", 0x58FF60, 8, { 0x83, 0xEC, 0x18, 0xE8, 0xE8, 0x73, 0x19, 0x00 }, &Loading, &scope::FillSides, nullptr, nullptr, 3 },
        { "CLoadingScreen::RenderLoadingBar", 0x590370, 5, { 0x83, 0xEC, 0x10, 0x56, 0x57 }, &Menu },
    };
}

extern "C" int __cdecl RemapMenuMouseX(int x)
{
    const scope::Xform xf = Menu();
    if (xf.kx >= 1.0f)
        return x;
    // The real screen, 0..width, in the menu's own (wider) space: the cursor
    // can reach the wide map's sides and is drawn back under the pointer.
    const float v = xf.ax + (static_cast<float>(x) - xf.ax) / xf.kx;
    const float lo = xf.ax - xf.ax / xf.kx, hi = xf.ax + (screen::Width() - xf.ax) / xf.kx;
    return static_cast<int>(std::lround(std::clamp(v, lo, hi)));
}

namespace
{
    void InstallWideMap()
    {
        struct Site { uintptr_t at; uint8_t op0, op1; uint32_t operand; const float* to; };
        const Site sites[] = {
            // CMenuManager::PrintMap: the frame (640-wide screens, then the scale for others)
            { 0x57532E, 0xD9, 0x05, 0x858B34, &g_mapLeft },  { 0x575353, 0xD9, 0x05, 0x865260, &g_mapRight },
            { 0x57533C, 0xD8, 0x0D, 0x8652B8, &g_mapLeftScale }, { 0x575361, 0xD8, 0x0D, 0x86536C, &g_mapRightScale },
            // Which tiles are streamed in
            { 0x573D64, 0xD9, 0x05, 0x858B34, &g_mapLeft },  { 0x573DCD, 0xD8, 0x05, 0x865260, &g_mapRight },
            // Scroll limits
            { 0x5787A5, 0xD8, 0x15, 0x858B34, &g_mapLeft },  { 0x5787B2, 0xD8, 0x25, 0x858B34, &g_mapLeft },
            { 0x5787C6, 0xD8, 0x1D, 0x865260, &g_mapRight }, { 0x5787D3, 0xD9, 0x05, 0x865260, &g_mapRight },
            // Keyboard/pad cursor reaching the left and right of the frame
            { 0x5782D0, 0xD8, 0x1D, 0x858B34, &g_mapLeft },  { 0x578374, 0xD8, 0x1D, 0x858B34, &g_mapLeft },
            { 0x5783F8, 0xD8, 0x1D, 0x858B34, &g_mapLeft },  { 0x577E1D, 0xD8, 0x1D, 0x865260, &g_mapRight },
            { 0x577EC1, 0xD8, 0x1D, 0x865260, &g_mapRight }, { 0x577F45, 0xD8, 0x1D, 0x865260, &g_mapRight },
            // How far the map cursor follows the mouse / zoom keeps it (x only; y stays)
            { 0x577714, 0xD8, 0x1D, 0x8653E0, &g_cursorRight }, { 0x577780, 0xD8, 0x1D, 0x8653E0, &g_cursorRight },
            { 0x578074, 0xD8, 0x1D, 0x8653E0, &g_cursorRight }, { 0x578289, 0xD8, 0x1D, 0x8653E0, &g_cursorRight },
            { 0x577794, 0xD8, 0x1D, 0x859A44, &g_cursorLeft },  { 0x577800, 0xD8, 0x1D, 0x859A44, &g_cursorLeft },
            { 0x577FC4, 0xD8, 0x1D, 0x859A44, &g_cursorLeft },  { 0x57841C, 0xD8, 0x1D, 0x859A44, &g_cursorLeft },
        };
        for (const Site& s : sites)
        {
            const uint8_t op[] = { s.op0, s.op1 };
            mem::RepointOperand(s.at, op, sizeof(op), s.operand, s.to);
        }

        // Mouse inside the map: call CMenuManager::StretchX (0x5733E0) with 60 / 580.
        for (auto [at, fn] : { std::pair<uintptr_t, const void*>{ 0x577CB7, reinterpret_cast<const void*>(&MapMouseLeft) },
                               std::pair<uintptr_t, const void*>{ 0x577CD0, reinterpret_cast<const void*>(&MapMouseRight) } })
        {
            const int32_t rel = static_cast<int32_t>(0x5733E0 - (at + 5));
            uint8_t call[5] = { 0xE8 };
            std::memcpy(call + 1, &rel, 4);
            if (mem::Matches(at, call, sizeof(call)))
                mem::WriteCall(at, fn, sizeof(call));
            else
                Log("skip: map mouse bounds at %08X are already patched", static_cast<unsigned>(at));
        }
    }
}

void InstallFrontendFixes()
{
    for (scope::Hook& h : g_loading)
        scope::Install(h);
    InstallVideoFix();

    if (!scope::Install(g_drawFrontEnd))
        return;
    scope::Install(g_printMap);
    if (config::wideMap)
        InstallWideMap();
    InstallMenuSettings();

    static const uint8_t storeMouseX[] = { 0x89, 0x0D, 0x28, 0x68, 0xBA, 0x00 };  // mov [m_nMousePosWinX], ecx
    if (mem::Matches(0x748323, storeMouseX, sizeof(storeMouseX)))
        mem::WriteCall(0x748323, reinterpret_cast<void*>(&ws_menu_mouse_x), sizeof(storeMouseX));
    else
        Log("skip: menu mouse input at 00748323 is already patched; menu clicks may be offset");

    // CMenuManager::UserInput clamps the mouse to 0..width again; the range
    // above already is the screen, so both X clamps become jumps.
    static const uint8_t clampLow[] = { 0x7D, 0x0A }, clampHigh[] = { 0x7E, 0x06 };  // jge / jle
    static const uint8_t jmpLow[] = { 0xEB, 0x0A }, jmpHigh[] = { 0xEB, 0x06 };
    if (mem::Matches(0x580004, clampLow, 2) && mem::Matches(0x58001B, clampHigh, 2))
    {
        mem::Write(0x580004, jmpLow, 2);
        mem::Write(0x58001B, jmpHigh, 2);
    }
    else
        Log("skip: menu mouse clamp at 00580004 is already patched");
}
