#include "config.hpp"
#include "features.hpp"
#include "mem.hpp"

#include <windows.h>
#include <control.h>

// The intro videos play through DirectShow in a window covering the game's
// client area. The renderer is told to draw the picture at its own aspect
// ratio inside that window; it paints the rest black.
namespace
{
    // IID_IBasicVideo, defined here rather than linking all of strmiids.
    constexpr GUID kIBasicVideo = { 0x56A868B5, 0x0AD4, 0x11CE, { 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };

    HWND GameWindow() { return **reinterpret_cast<HWND**>(0xC17054); }  // RsGlobal.ps->window
    IVideoWindow* VideoWindow() { return *reinterpret_cast<IVideoWindow**>(0xC920E0); }

    void Letterbox(IVideoWindow* window, long width, long height)
    {
        IBasicVideo* video = nullptr;
        if (FAILED(window->QueryInterface(kIBasicVideo, reinterpret_cast<void**>(&video))))
            return;

        long videoW = 0, videoH = 0;
        if (SUCCEEDED(video->GetVideoSize(&videoW, &videoH)) && videoW > 0 && videoH > 0)
        {
            // Fit inside the window: full height, or full width if the video is wider.
            long w = width, h = height;
            if (static_cast<long long>(videoW) * height > static_cast<long long>(videoH) * width)
                h = MulDiv(width, videoH, videoW);
            else
                w = MulDiv(height, videoW, videoH);
            video->SetDestinationPosition((width - w) / 2, (height - h) / 2, w, h);
        }
        video->Release();
    }

    // Replaces the game's function that sizes the video window to the client
    // area and routes its input back to the game.
    HRESULT __cdecl ResizeVideoWindow()
    {
        HWND game = GameWindow();
        IVideoWindow* window = VideoWindow();

        RECT client;
        GetClientRect(game, &client);
        HRESULT hr = window->SetWindowPosition(client.left, client.top, client.right, client.bottom);
        if (FAILED(hr))
            return hr;

        if (config::widescreen && config::videos == config::Fit::Pillarbox)
            Letterbox(window, client.right - client.left, client.bottom - client.top);

        hr = window->put_MessageDrain(reinterpret_cast<OAHWND>(game));
        SetFocus(game);
        return hr;
    }
}

// Start-up game state 1 (GS_INIT_LOGO_MPEG) starts Logo.mpg; jumping from
// there to state 5 (GS_INIT_ONCE) skips it and GTAtitles.mpg. The splash
// screens of state 0, which also load textures, still run.
void InstallSkipIntro()
{
    static const uint8_t initLogo[] = { 0x39, 0x1D, 0xCC, 0x20, 0xC9, 0x00 };  // cmp [noMovies], ebx
    if (!mem::Matches(0x748AF2, initLogo, sizeof(initLogo)))
    {
        Log("skip: intro state at 00748AF2 is already patched");
        return;
    }
    const uint8_t code[] = { 0xC7, 0x05, 0xC0, 0xD4, 0xC8, 0x00, 0x05, 0x00, 0x00, 0x00 };  // mov [gGameState], 5
    mem::Write(0x748AF2, code, sizeof(code));
    mem::WriteJump(0x748AFC, reinterpret_cast<void*>(mem::Address(0x748DA3)));  // jmp to the end of the state switch
}

void InstallVideoFix()
{
    static const uint8_t prologue[] = { 0x8B, 0x0D, 0x54, 0x70, 0xC1, 0x00 };  // mov ecx, [RsGlobal.ps]
    if (mem::Matches(0x7466D0, prologue, sizeof(prologue)))
        mem::WriteJump(0x7466D0, reinterpret_cast<void*>(&ResizeVideoWindow));
    else
        Log("skip: video window sizing at 007466D0 is already patched");
}
