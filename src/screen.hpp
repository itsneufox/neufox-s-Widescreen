#pragma once
#include "config.hpp"
#include "game.hpp"

#include <algorithm>

// The game lays out all 2D on a 640x448 canvas stretched to the full window.
// On a wider window that stretch makes everything too wide; `HudSquash` is
// the horizontal factor that restores the 4:3 proportions.
namespace screen
{
    constexpr float kBaseAspect = 4.0f / 3.0f;

    inline float Width() { return static_cast<float>(game::ScreenWidth()); }
    inline float Height() { return static_cast<float>(game::ScreenHeight()); }

    inline float Aspect()
    {
        if (config::forcedAspect > 0.0f)
            return config::forcedAspect;
        int w = game::ScreenWidth(), h = game::ScreenHeight();
        return (w > 0 && h > 0) ? static_cast<float>(w) / static_cast<float>(h) : kBaseAspect;
    }

    inline float HudSquash() { return kBaseAspect / Aspect(); }

    // Left and right edge of the centred area no wider than `maxAspect`
    // (0 = the whole screen), where edge-anchored elements are placed.
    struct Span { float left, right; };
    inline Span Region(float maxAspect)
    {
        const float w = Width(), h = Height();
        const float regionW = maxAspect > 0.0f ? std::min(w, h * maxAspect) : w;
        return { 0.5f * (w - regionW), 0.5f * (w + regionW) };
    }

    // Converts a setting in pixels at 1080p to the current resolution.
    inline float Px1080(float value) { return value * Height() / 1080.0f; }
}
