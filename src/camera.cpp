#include "features.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "screen.hpp"

#include <algorithm>
#include <cmath>

// 3D: the game sets the RenderWare view window from a horizontal FOV, so a
// wider screen shows less vertically (Vert-). With Hor+ the view window is
// widened instead, and every place that relies on the old relation between
// FOV and view window is adjusted to match.

extern "C"
{
    // Read by the stubs in thunks.S.
    float g_horFactor = 1.0f;             // tan(rendered hfov / 2) / tan(game fov / 2)
    float g_aimAspect = 4.0f / 3.0f;      // aspect / horFactor
    float g_half = 0.5f;
    float g_degToRadHalf = 3.14159265f / 360.0f;
    float g_radToDegTwice = 360.0f / 3.14159265f;

    void ws_aim_mult_x();
    void ws_render_fov();
}

namespace
{
    int g_spriteRefWidth = 640;  // width used to size coronas and other camera-facing sprites

    // CClouds::RenderSkyPolys stretches the horizon band past the screen
    // edges by this much; a wider view needs a wider band.
    constexpr float kSkyExtent = 1.4f;
    float g_skyExtent = kSkyExtent;

    using CameraSizeFn = void(__cdecl*)(void* camera, void* rect, float viewWindow, float aspect);
    CameraSizeFn g_cameraSize;

    // Vanilla renders cutscenes (and other bordered camera shots) at a 5:4
    // aspect inside 4:3; Hor+ keeps that vertical framing on wider screens.
    constexpr float kCutsceneAspect = 1.25f;

    // FOVScale, as a factor on the view window: tan(fov * scale / 2) / tan(fov / 2).
    float g_fovFactor = 1.0f;

    void UpdateFovFactor(float viewWindow)
    {
        g_fovFactor = 1.0f;
        if (!config::widescreen || config::fovScale == 1.0f || game::BordersOn() || viewWindow <= 0.0f)
            return;
        const float half = std::min(std::atan(viewWindow) * config::fovScale, 1.48f);  // < 85 degrees
        g_fovFactor = std::tan(half) / viewWindow;
    }

    float UpdateFactors()
    {
        const float aspect = screen::Aspect();
        const float base = game::BordersOn() ? kCutsceneAspect : screen::kBaseAspect;
        g_horFactor = (config::widescreen && config::horPlus ? std::max(1.0f, aspect / base) : 1.0f) * g_fovFactor;
        g_aimAspect = aspect / g_horFactor;
        g_skyExtent = kSkyExtent * g_horFactor;
        g_spriteRefWidth = static_cast<int>(std::lround(game::ScreenWidth() / g_horFactor));
        return aspect;
    }

    // Replaces CDraw::CalculateAspectRatio, which only knew 4:3, 16:9 (menu
    // "widescreen" option) and 5:4 (cutscenes).
    float __cdecl CalculateAspectRatio()
    {
        config::Poll();  // runs every frame: a cheap place to pick up ini edits
        return game::AspectRatio() = UpdateFactors();
    }

    // CameraSize(camera, rect, tan(fov / 2), aspect)
    void __cdecl CameraSize(void* camera, void* rect, float viewWindow, float)
    {
        UpdateFovFactor(viewWindow);
        const float aspect = UpdateFactors();
        g_cameraSize(camera, rect, viewWindow * g_horFactor, aspect);
    }

    void PatchCall(uintptr_t at, const uint8_t (&expect)[6], void (*stub)())
    {
        if (mem::Matches(at, expect, sizeof(expect)))
            mem::WriteCall(at, reinterpret_cast<void*>(stub), sizeof(expect));
        else
            Log("skip: unexpected code at %08X", at);
    }
}

void InstallCameraFixes()
{
    static const uint8_t calcAspect[] = { 0xA0, 0x93, 0x67, 0xBA, 0x00 };
    if (mem::Matches(0x6FF420, calcAspect, sizeof(calcAspect)))
        mem::WriteJump(0x6FF420, reinterpret_cast<void*>(&CalculateAspectRatio));
    else
        Log("skip: CDraw::CalculateAspectRatio is already patched");

    static const uint8_t cameraSize[] = { 0x83, 0xEC, 0x34, 0x57, 0x8B, 0x7C, 0x24, 0x3C };
    g_cameraSize = reinterpret_cast<CameraSizeFn>(
        mem::Detour("CameraSize", 0x72FC70, cameraSize, sizeof(cameraSize), reinterpret_cast<void*>(&CameraSize)));

    // Installed even with Hor+ off: at a factor of 1 these are the original
    // behaviour, and the menu can switch Hor+ while the game runs.
    if (!g_cameraSize)
        return;

    static const uint8_t fdiv[] = { 0xD8, 0x35 };
    static const uint8_t fild[] = { 0xDB, 0x05 };
    static const uint8_t fldMultX[6] = { 0xD9, 0x05, 0x14, 0xEC, 0xB6, 0x00 };
    static const uint8_t fldFov[6] = { 0xD9, 0x05, 0x38, 0x50, 0x8D, 0x00 };

    // Aim direction through the crosshair (CCamera::Find3rdPersonCamTargetVector,
    // CCamera::Find3rdPersonQuickAimPitch, CPlayerPed aiming): these rebuild the
    // view window from the camera FOV, so give them the widened one.
    PatchCall(0x5149A6, fldMultX, &ws_aim_mult_x);
    PatchCall(0x5219AE, fldMultX, &ws_aim_mult_x);
    for (uintptr_t at : { 0x5149CEu, 0x50AD79u, 0x5219DAu })
        mem::RepointOperand(at, fdiv, sizeof(fdiv), 0xC3EFA4, &g_aimAspect);

    // Frustum planes used for culling (CCamera::CalculateDerivedValues).
    PatchCall(0x514D63, fldFov, &ws_render_fov);

    // Sky: the horizon band is sized for the unwidened view.
    static const uint8_t fmul[] = { 0xD8, 0x0D };
    for (uintptr_t at : { 0x714841u, 0x71485Eu })
        mem::RepointOperand(at, fmul, sizeof(fmul), 0x859994, &g_skyExtent);

    // CSprite::CalcScreenCoors sizes sprites as screenWidth * 70 / fov; use
    // the width the unwidened view would have so coronas keep their shape.
    if (config::proportionalSprites)
        for (uintptr_t at : { 0x70CED8u, 0x71DA6Du })
            mem::RepointOperand(at, fild, sizeof(fild), 0xC17044, &g_spriteRefWidth);
}
