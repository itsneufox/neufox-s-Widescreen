#pragma once

// The optional scaling of SA-MP textdraws on screens wider than 16:9.
namespace textdraw
{
    // Where a textdraw goes: narrowed by k around the anchor ax.
    struct Placement
    {
        float ax, k;
        float X(float x) const { return ax + (x - ax) * k; }
    };

    // How much narrower than now a textdraw must be to look as it does at 16:9;
    // 1 when the setting is off or the screen is no wider than that.
    float Narrowing();
    // Whether a game function was called from samp.dll, with scaling active.
    bool FromSamp(const void* returnAddress);
    // Records a textdraw (in the coordinates SA-MP gave it) and returns where it goes.
    Placement Place(float left, float right, float top, float bottom, bool textOnly);
}
