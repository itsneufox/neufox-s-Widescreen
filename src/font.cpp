#include "features.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "scope.hpp"
#include "textdraw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// Text is not drawn by PrintString: it is queued with a copy of the font
// state and rendered later by CFont::RenderFontBuffer, outside any scope. So
// text is transformed here, at queue time: position, scale and wrap limits
// pass through the scope transform for the duration of the call.
namespace
{
    using PrintStringFn = void(__cdecl*)(float x, float y, const char* text);
    using RenderFontBufferFn = void(__cdecl*)();

    PrintStringFn g_printString;
    RenderFontBufferFn g_renderFontBuffer;
    bool g_inTextdraw = false;

    // An SA-MP textdraw, in screen coordinates: no scope is involved. Its extent
    // decides which panel it belongs to: the box if it has one, otherwise the text.
    void PrintTextdraw(float x, float y, const char* text)
    {
        namespace font = game::font;
        const float scaleX = font::ScaleX(), wrap = font::WrapX(), centre = font::CentreSize(), rightWrap = font::RightJustifyWrap();
        const bool centred = font::Centred(), right = font::RightJustified();

        const bool box = font::HasBox();
        const float room = box ? (centred ? centre : right ? x - rightWrap : wrap - x) : 0.0f;

        // CFont::GetStringWidth adds up all of a text's lines: measure them one by one.
        float width = 0.0f;
        int lines = 0;
        for (const char* line = text; line;)
        {
            const char* const next = std::strstr(line, "~n~");
            char part[400];
            const size_t length = std::min(next ? static_cast<size_t>(next - line) : std::strlen(line), sizeof(part) - 1);
            std::memcpy(part, line, length);
            part[length] = '\0';
            const float w = reinterpret_cast<float(__cdecl*)(const char*, bool, bool)>(0x71A0E0)(part, true, false);
            width = std::max(width, w);
            lines += 1 + (box && room > 1.0f ? static_cast<int>(w / room) : 0);
            line = next ? next + 3 : nullptr;
        }

        float left, end;
        if (box)
        {
            left = centred ? x - 0.5f * centre : right ? rightWrap : x;
            end = centred ? x + 0.5f * centre : right ? x : wrap;
        }
        else
        {
            left = centred ? x - 0.5f * width : right ? x - width : x;
            end = left + width;
        }
        const float bottom = y + lines * font::ScaleY() * 20.0f;

        const textdraw::Placement p = textdraw::Place(left, end, y, bottom, !box);
        if (p.k >= 1.0f)
            return g_printString(x, y, text);

        font::ScaleX() = scaleX * p.k;
        font::WrapX() = p.X(wrap);
        font::RightJustifyWrap() = p.X(rightWrap);
        font::CentreSize() = centre * p.k;

        g_inTextdraw = true;
        g_printString(p.X(x), y, text);
        g_inTextdraw = false;

        font::ScaleX() = scaleX;
        font::WrapX() = wrap;
        font::CentreSize() = centre;
        font::RightJustifyWrap() = rightWrap;
    }

    void __cdecl PrintString(float x, float y, const char* text)
    {
        if (textdraw::FromSamp(__builtin_return_address(0)))
            return PrintTextdraw(x, y, text);

        scope::Frame* f = scope::Top();
        if (!f || f->xf.Identity())
            return g_printString(x, y, text);

        namespace font = game::font;
        scope::Xform xf = f->xf;
        xf.ax = scope::AnchorX(xf, x, x, y, y);
        const float scaleX = font::ScaleX(), scaleY = font::ScaleY();
        const float slantX = font::SlantRefX(), slantY = font::SlantRefY();
        const float wrap = font::WrapX(), centre = font::CentreSize(), rightWrap = font::RightJustifyWrap();

        font::ScaleX() = scaleX * xf.kx;
        font::ScaleY() = scaleY * xf.ky;
        font::SlantRefX() = xf.X(slantX);
        font::SlantRefY() = xf.Y(slantY);
        font::WrapX() = xf.X(wrap);
        font::CentreSize() = centre * xf.kx;
        font::RightJustifyWrap() = xf.X(rightWrap);

        // The text background box is drawn right away from the transformed
        // state; it must not be transformed a second time.
        scope::PushIdentity();
        g_printString(xf.X(x), xf.Y(y), text);
        scope::Pop();

        font::ScaleX() = scaleX;
        font::ScaleY() = scaleY;
        font::SlantRefX() = slantX;
        font::SlantRefY() = slantY;
        font::WrapX() = wrap;
        font::CentreSize() = centre;
        font::RightJustifyWrap() = rightWrap;
    }

    // TEXT OUTLINE. The game draws a shadow as one offset copy and an edge as 8,
    // with an X offset in canvas units: blocky, and wider than tall once the text
    // is narrowed. In a scope (the game's own menus and HUD, never SA-MP or CLEO)
    // both become a ring of copies that is round on screen. Letter spacing is left
    // alone, so shadowed text keeps its layout.
    using PrintLineFn = void(__cdecl*)(float x, float y, const char* start, const char* end, float wrap);
    PrintLineFn g_printLine;

    void __cdecl PrintLine(float x, float y, const char* start, const char* end, float wrap)
    {
        namespace font = game::font;
        const int8_t shadow = font::Shadow(), edge = font::Edge();
        const int size = edge ? edge : shadow;
        // Narrowed textdraws keep their shadow, but their edge would be far too
        // wide on screen: it gets the ring whatever TEXT OUTLINE is set to.
        const bool ourText = config::textOutline && config::widescreen && scope::Top();
        if (size <= 0 || !(ourText || (g_inTextdraw && edge > 0)))
            return g_printLine(x, y, start, end, wrap);

        // Offset copies only ever get queued by the original when these are set.
        font::Shadow() = 0;
        font::Edge() = 0;

        static constexpr int kCopies = 16;
        static constexpr float kStep = 2.0f * 3.14159265f / kCopies;
        const uint32_t color = font::Color();
        if (const uint8_t dropAlpha = font::DropAlpha())
        {
            // Copies overlap: each is fainter, so together they reach the drop alpha.
            const float perCopy = 1.0f - std::pow(1.0f - dropAlpha / 255.0f, 1.0f / kCopies);
            const uint32_t alpha = static_cast<uint32_t>(std::lround(perCopy * 255.0f));
            font::Color() = font::DropRed() | font::DropGreen() << 8 | font::DropBlue() << 16 | alpha << 24;
            font::IsCopy() = 1;

            const float radius = size * game::ScreenHeight() / 448.0f;
            const bool slanted = font::Slant() != 0.0f;
            const float slantX = font::SlantRefX(), slantY = font::SlantRefY();
            for (int i = 0; i < kCopies; ++i)
            {
                const float ox = radius * std::cos(i * kStep), oy = radius * std::sin(i * kStep);
                if (slanted)
                {
                    font::SlantRefX() = slantX + ox;
                    font::SlantRefY() = slantY + oy;
                }
                g_printLine(x + ox, y + oy, start, end, wrap);
            }
            font::SlantRefX() = slantX;
            font::SlantRefY() = slantY;

            font::Color() = color;
            font::IsCopy() = 0;
        }

        g_printLine(x, y, start, end, wrap);

        font::Shadow() = shadow;
        font::Edge() = edge;
    }

    // The buffer is also flushed mid-scope when it fills up.
    void __cdecl RenderFontBuffer()
    {
        scope::PushIdentity();
        g_renderFontBuffer();
        scope::Pop();
    }
}

void InstallFontHooks()
{
    static const uint8_t printString[] = { 0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x20 };
    static const uint8_t renderBuffer[] = { 0xA1, 0xA8, 0x16, 0xC7, 0x00 };

    g_renderFontBuffer = reinterpret_cast<RenderFontBufferFn>(
        mem::Detour("CFont::RenderFontBuffer", 0x719840, renderBuffer, sizeof(renderBuffer),
                    reinterpret_cast<void*>(&RenderFontBuffer)));
    if (!g_renderFontBuffer)
        return;  // without it, flushed glyphs would be transformed twice

    g_printString = reinterpret_cast<PrintStringFn>(
        mem::Detour("CFont::PrintString", 0x71A700, printString, sizeof(printString),
                    reinterpret_cast<void*>(&PrintString)));

    // sub esp, 8; movzx eax, byte ptr [0xC71A94]
    static const uint8_t printLine[] = { 0x83, 0xEC, 0x08, 0x0F, 0xB6, 0x05, 0x94, 0x1A, 0xC7, 0x00 };
    g_printLine = reinterpret_cast<PrintLineFn>(
        mem::Detour("CFont::PrintString (one line)", 0x719B40, printLine, sizeof(printLine),
                    reinterpret_cast<void*>(&PrintLine)));
}
