#include "features.hpp"
#include "game.hpp"
#include "scope.hpp"
#include "screen.hpp"
#include "mem.hpp"

#include <cstring>

// Each HUD element is scaled towards the screen edge or centre it belongs
// to, so it keeps its 4:3 shape and its margin from that edge.
namespace
{
    using scope::Xform;

    // SA-MP's /hudscalefix rewrites the canvas scales and radar size around
    // the radar and crosshair draws. Inside those scopes the original values
    // are put back, and SA-MP's restored after.
    struct CanvasValues
    {
        float invWidth, invHeight, radarHeight, radarWidth;
    };
    constexpr uintptr_t kInvWidth = 0x859520, kInvHeight = 0x859524;
    constexpr uintptr_t kRadarHeight = 0x866B74, kRadarWidth = 0x866B78;
    constexpr CanvasValues kVanilla = { 1.0f / 640.0f, 1.0f / 448.0f, 76.0f, 94.0f };

    CanvasValues g_foreign;
    bool g_overridden = false;

    float Get(uintptr_t a) { return *reinterpret_cast<const float*>(a); }
    void Set(uintptr_t a, float v)
    {
        if (Get(a) != v)
            mem::Write(a, &v, sizeof(v));
    }

    void UseVanillaCanvas()
    {
        const CanvasValues now = { Get(kInvWidth), Get(kInvHeight), Get(kRadarHeight), Get(kRadarWidth) };
        if (std::memcmp(&now, &kVanilla, sizeof(now)) == 0)
            return;
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("SA-MP /hudscalefix (or another mod) rescales the radar/crosshair; using the original values inside them");
        }
        g_foreign = now;
        g_overridden = true;
        Set(kInvWidth, kVanilla.invWidth);
        Set(kInvHeight, kVanilla.invHeight);
        Set(kRadarHeight, kVanilla.radarHeight);
        Set(kRadarWidth, kVanilla.radarWidth);
    }

    void RestoreForeignCanvas(const scope::Frame&)
    {
        if (!g_overridden)
            return;
        g_overridden = false;
        Set(kInvWidth, g_foreign.invWidth);
        Set(kInvHeight, g_foreign.invHeight);
        Set(kRadarHeight, g_foreign.radarHeight);
        Set(kRadarWidth, g_foreign.radarWidth);
    }

    void CrosshairLeave(const scope::Frame& f)
    {
        RestoreForeignCanvas(f);
        scope::FillSides(f);
    }
    using config::HudPart;

    enum class H { Left, Centre, Right };
    enum class V { Top, Centre, Bottom };

    template <H h, V v, HudPart part>
    Xform Anchored()
    {
        const config::HudPartSettings& p = config::Part(part);
        const float s = config::hudScale * p.scale;
        const screen::Span region = screen::Region(config::hudMaxAspect);
        Xform xf;
        xf.kx = screen::HudSquash() * s * config::hudWidth * p.width;
        xf.ky = s * config::hudHeight;
        xf.ax = h == H::Left ? region.left : h == H::Centre ? 0.5f * screen::Width() : region.right;
        xf.ay = v == V::Top ? 0.0f : v == V::Centre ? 0.5f * screen::Height() : screen::Height();
        xf.tx = screen::Px1080(p.offsetX);
        xf.ty = screen::Px1080(p.offsetY);
        xf.flags = scope::kKeepFullWidth;
        if (part == HudPart::PlayerInfo || part == HudPart::Wanted)
        {
            // In two-player mode one of the two HUDs is on the left.
            xf.flags |= scope::kAutoAnchors;
            xf.edgeLeft = region.left;
            xf.edgeRight = region.right;
        }
        return xf;
    }

    // CRadar lays the radar out as 94x76 units of the 640x448 canvas, which
    // is 15% wider than tall even on a 4:3 screen. Round narrows it about its
    // own left edge (x = 40), so its distance from the screen edge is kept.
    Xform Radar()
    {
        UseVanillaCanvas();
        Xform xf = Anchored<H::Left, V::Bottom, HudPart::Radar>();
        if (!config::roundRadar)
            return xf;
        constexpr float kRound = (76.0f / 448.0f * 3.0f) / (94.0f / 640.0f * 4.0f);  // height / width at 4:3
        const float left = 40.0f * screen::Width() / 640.0f;
        xf.tx += (left - xf.ax) * xf.kx * (1.0f - kRound);
        xf.kx *= kRound;
        return xf;
    }

    // Script text and sprites (minigames, mission screens) are full 640x448
    // layouts: keep them as one 4:3 picture in the middle.
    Xform Pillarbox()
    {
        Xform xf;
        xf.kx = screen::HudSquash();
        xf.ax = 0.5f * screen::Width();
        xf.flags = scope::kKeepFullWidth;
        return xf;
    }

    // The crosshair is scaled about the point the camera actually aims at:
    // the off-centre 3rd-person aim point, or the centre in first person.
    // Scaled about the given aim point; offsets would move it away from where
    // shots land, so size only.
    Xform CrosshairAbout(bool thirdPerson)
    {
        const config::HudPartSettings& p = config::Part(HudPart::Crosshair);
        Xform xf;
        xf.kx = screen::HudSquash() * p.scale * p.width;
        xf.ky = p.scale;
        xf.ax = screen::Width() * (thirdPerson ? game::CrosshairMultX() : 0.5f);
        xf.ay = screen::Height() * (thirdPerson ? game::CrosshairMultY() : 0.5f);
        return xf;
    }

    Xform Crosshair()
    {
        UseVanillaCanvas();
        const uint16_t mode = game::ActiveCamMode();
        const bool thirdPerson = mode == 53 || mode == 55 || mode == 65;  // MODE_AIMWEAPON*
        Xform xf = CrosshairAbout(thirdPerson);
        xf.flags = scope::kFillSides;
        return xf;
    }

    scope::Hook g_hud[] = {
        { "CHud::DrawCrosshairs", 0x58E020, 10, { 0x83, 0xEC, 0x30, 0x0F, 0xB6, 0x05, 0x74, 0xCD, 0xB7, 0x00 }, &Crosshair, &CrosshairLeave },
        { "CHud::DrawPlayerInfo", 0x58EAF0, 6, { 0x81, 0xEC, 0xA0, 0x01, 0x00, 0x00 }, &Anchored<H::Right, V::Top, HudPart::PlayerInfo> },
        { "CHud::DrawWanted", 0x58D9A0, 5, { 0x83, 0xEC, 0x4C, 0x53, 0x55 }, &Anchored<H::Right, V::Top, HudPart::Wanted> },
        { "CHud::DrawMissionTimers", 0x58B180, 5, { 0xA0, 0xC0, 0xAE, 0xBA, 0x00 }, &Anchored<H::Right, V::Top, HudPart::Timers> },
        { "CHud::DrawVehicleName", 0x58AEA0, 7, { 0x51, 0x8B, 0x15, 0x58, 0xA4, 0xBA, 0x00 }, &Anchored<H::Right, V::Bottom, HudPart::ZoneNames> },
        { "CHud::DrawAreaName", 0x58AA50, 7, { 0x51, 0x8B, 0x15, 0xD8, 0xB1, 0xBA, 0x00 }, &Anchored<H::Right, V::Bottom, HudPart::ZoneNames> },
        { "CHud::DrawMissionTitle", 0x58D240, 6, { 0x51, 0xA0, 0x40, 0xAD, 0xBA, 0x00 }, &Anchored<H::Right, V::Bottom, HudPart::MissionTitle> },
        { "CHud::DrawRadar", 0x58A330, 5, { 0xA1, 0xCC, 0xA7, 0x96, 0x00 }, &Radar, &RestoreForeignCanvas },
        { "CHud::DrawVitalStats", 0x589650, 5, { 0xA0, 0x88, 0x30, 0xA4, 0x00 }, &Anchored<H::Left, V::Bottom, HudPart::Radar> },
        { "CHud::DrawHelpText", 0x58B6E0, 5, { 0xA0, 0xA0, 0xA7, 0xBA, 0x00 }, &Anchored<H::Left, V::Top, HudPart::HelpText> },
        { "CHud::DrawTripSkip", 0x58A160, 9, { 0x83, 0xEC, 0x1C, 0xDB, 0x05, 0x44, 0x70, 0xC1, 0x00 }, &Anchored<H::Centre, V::Bottom, HudPart::Messages> },
        { "CHud::DrawSubtitles", 0x58C250, 5, { 0xA0, 0x40, 0xB0, 0xBA, 0x00 }, &Anchored<H::Centre, V::Bottom, HudPart::Subtitles> },
        { "CHud::DrawOddJobMessage", 0x58CC80, 5, { 0xA0, 0x89, 0x44, 0xA4, 0x00 }, &Anchored<H::Centre, V::Centre, HudPart::Messages> },
        { "CHud::DrawSuccessFailedMessage", 0x58C6A0, 8, { 0x51, 0xF6, 0x05, 0x1C, 0xB2, 0xBA, 0x00, 0x01 }, &Anchored<H::Centre, V::Centre, HudPart::Messages> },
        { "CHud::DrawBustedWastedMessage", 0x58CA50, 6, { 0x51, 0xA0, 0xC0, 0xAD, 0xBA, 0x00 }, &Anchored<H::Centre, V::Centre, HudPart::Messages> },
        { "CAudioEngine::DisplayRadioStationName", 0x507030, 5, { 0xB9, 0xF8, 0xB6, 0x8C, 0x00 }, &Anchored<H::Centre, V::Top, HudPart::RadioName> },
        { "CGarages::PrintMessages", 0x447790, 6, { 0x51, 0xA1, 0x84, 0xCB, 0xB7, 0x00 }, &Anchored<H::Centre, V::Centre, HudPart::Messages> },
        { "CDarkel::DrawMessages", 0x43CEC0, 5, { 0xA0, 0x88, 0x30, 0xA4, 0x00 }, &Anchored<H::Centre, V::Centre, HudPart::Messages> },
    };

    // CHud::DrawScriptText also draws CTheScripts' sprites and rectangles.
    scope::Hook g_scriptText = { "CHud::DrawScriptText", 0x58C080, 6, { 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00 }, &Pillarbox };
}

// The circle crosshair as CHud::DrawCrossHairs draws it while aiming (one
// quarter of the siteM16 sprite, mirrored four times around the aim point),
// for the settings page's live preview when the player is not aiming.
void DrawCrosshairPreview()
{
    void* sprite = reinterpret_cast<void*>(0xBAB1FC + 1 * 4);  // CHud::Sprites[SPRITE_SITE_M16]
    if (!*reinterpret_cast<void**>(sprite))
        return;

    // The current weapon's spread, or a typical rifle's when unarmed.
    float radius = 0.6f;
    if (void* ped = reinterpret_cast<void*(__cdecl*)(int)>(0x56E210)(-1))  // FindPlayerPed
    {
        const float r = reinterpret_cast<float(__thiscall*)(void*)>(0x609CD0)(ped);  // CPlayerPed::GetWeaponRadiusOnScreen
        if (r > 0.2f)
            radius = r;
    }

    const float w = screen::Width(), h = screen::Height();
    const float cx = w * game::CrosshairMultX(), cy = h * game::CrosshairMultY();
    const float sx = w / 640.0f * 32.0f * radius, sy = h / 448.0f * 32.0f * radius;
    const game::RGBA white{ 255, 255, 255, 255 };
    // Each quarter maps the sprite from its outer corner to the centre; the
    // flipped edges mirror it into the other three.
    const game::Rect quarters[] = {
        { cx - sx, cy, cx, cy - sy }, { cx + sx, cy, cx, cy - sy },
        { cx - sx, cy, cx, cy + sy }, { cx + sx, cy, cx, cy + sy },
    };

    scope::Push(config::widescreen ? CrosshairAbout(true) : scope::Xform{});
    for (const game::Rect& r : quarters)
        reinterpret_cast<void(__thiscall*)(void*, const game::Rect&, const game::RGBA&)>(0x728350)(sprite, r, white);  // CSprite2d::Draw
    scope::Pop();
}

void InstallHudScopes()
{
    for (scope::Hook& h : g_hud)
        scope::Install(h);
    if (config::scriptDrawing)
        scope::Install(g_scriptText);
}
