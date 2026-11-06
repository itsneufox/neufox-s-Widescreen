#include "features.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "scope.hpp"
#include "screen.hpp"

#include <algorithm>

// Replaces CCamera::DrawBordersForWideScreen. Vanilla switches the bars on
// instantly (m_ScreenReductionPercentage jumps to 30) and sizes them from the
// screen height, so they fit any aspect ratio; this adds size, sliding, and
// side bars that limit the picture to exactly what vanilla framed.
namespace
{
    constexpr float kFullReduction = 30.0f;
    // Vanilla bar heights at full reduction, as fractions of the screen height
    // (CCamera::GetScreenRect: height/2 * 30% -/+ 22 and 14 units of 448).
    constexpr float kTopBar = 0.15f - 22.0f / 448.0f;
    constexpr float kBottomBar = 0.15f + 14.0f / 448.0f;
    // Vanilla shows cutscenes at 5:4 of the screen height (see camera.cpp).
    constexpr float kVanillaWidth = 1.25f;

    float g_shown = 0.0f;  // 0..1, how far the bars are in

    void __thiscall DrawBorders(uint8_t* camera)
    {
        // Kept from the original: resets the motion blur alpha.
        int blurType = *reinterpret_cast<int*>(camera + 0x90);
        if (blurType == 0 || blurType == 2)
            *reinterpret_cast<int*>(camera + 0xA4) = 80;

        // The fix switched off: the original bars.
        const config::Borders mode = config::widescreen ? config::borders : config::Borders::Vanilla;
        const float size = config::widescreen ? config::borderSize : 1.0f;
        const float slide = config::widescreen ? config::borderSlide : 0.0f;

        const bool on = camera[0x3D];  // m_bWideScreenOn
        const float reduction = *reinterpret_cast<float*>(camera + 0x144);
        const float target = on ? std::clamp(reduction / kFullReduction, 0.0f, 1.0f) : 0.0f;

        if (slide > 0.0f)
        {
            const float step = game::TimeStep() / 50.0f / slide;
            g_shown = target > g_shown ? std::min(target, g_shown + step) : std::max(target, g_shown - step);
        }
        else
            g_shown = target;

        if (mode == config::Borders::Off || g_shown <= 0.0f)
            return;

        const float w = screen::Width(), h = screen::Height();
        const float amount = g_shown * size;
        const float top = h * kTopBar * amount, bottom = h * kBottomBar * amount;
        const game::RGBA black{ 0, 0, 0, 255 };

        scope::PushIdentity();
        game::DrawRect({ -5.0f, top, w + 5.0f, -5.0f }, black);
        game::DrawRect({ -5.0f, h + 5.0f, w + 5.0f, h - bottom }, black);
        if (mode == config::Borders::Pillarbox)
        {
            const float side = std::max(0.0f, 0.5f * (w - h * kVanillaWidth)) * g_shown;
            if (side > 0.0f)
            {
                game::DrawRect({ -5.0f, h - bottom, side, top }, black);
                game::DrawRect({ w - side, h - bottom, w + 5.0f, top }, black);
            }
        }
        scope::Pop();
    }
}

void InstallCutsceneBorders()
{
    static const uint8_t prologue[] = { 0x83, 0xEC, 0x24, 0x8D, 0x44, 0x24, 0x14 };
    if (mem::Matches(0x514860, prologue, sizeof(prologue)))
        mem::WriteJump(0x514860, reinterpret_cast<void*>(&DrawBorders));
    else
        Log("skip: CCamera::DrawBordersForWideScreen is already patched");
}
