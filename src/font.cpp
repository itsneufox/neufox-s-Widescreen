#include "features.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "scope.hpp"

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

    void __cdecl PrintString(float x, float y, const char* text)
    {
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
}
