#pragma once
#include <cstdint>

// Settings from neufoxWidescreen.ini. Values marked "live" are re-read while
// the game runs (see Poll); the others only matter when hooks are installed.
namespace config
{
    // How full-screen artwork and videos are shown, based on their own
    // aspect ratio.
    enum class Fit : uint8_t { Stretch, Crop, Pillarbox };

    enum class MenuLayout : uint8_t { Edges, Centered, Stretch };

    enum class Borders : uint8_t { Vanilla, Off, Pillarbox };

    // Independently adjustable HUD parts.
    enum class HudPart : uint8_t
    {
        Radar, PlayerInfo, Wanted, Timers, ZoneNames, MissionTitle,
        HelpText, Subtitles, Messages, RadioName, Crosshair, Count
    };

    struct HudPartSettings
    {
        float scale = 1.0f;
        float width = 1.0f;     // extra horizontal factor
        float offsetX = 0.0f;   // pixels at 1080p, + is right
        float offsetY = 0.0f;   // pixels at 1080p, + is down
    };

    // [Display]
    extern bool widescreen;         // live; master switch: off is the original game
    extern float forcedAspect;      // live; 0 = the window's real aspect ratio
    extern bool horPlus;            // keep the 4:3 vertical FOV, widen horizontally
    extern float fovScale;          // live; gameplay FOV multiplier, cutscenes excluded
    extern bool proportionalSprites;
    extern bool waterFix;           // draw the water blocks at the edges of a wide view

    // [HUD]
    extern bool hud;
    extern float hudScale, hudWidth, hudHeight;  // live
    extern float hudMaxAspect;                    // live; 0 = the whole screen
    extern bool scriptDrawing;      // SCM/CLEO text and sprites (off: CLEO mods position for 640x448)
    extern HudPartSettings hudParts[static_cast<int>(HudPart::Count)];  // live
    extern bool squareWeaponIcon;   // live; the original icon is drawn taller than its texture
    extern bool roundRadar;         // live; the original radar is 15% wider than tall even at 4:3

    inline const HudPartSettings& Part(HudPart p) { return hudParts[static_cast<int>(p)]; }

    // [Frontend]
    extern bool frontend;
    extern MenuLayout menuLayout;   // live
    extern bool wideMap;            // the map page fills the screen width
    extern bool liveMenuPreview;    // the game stays visible behind the Widescreen Settings page
    extern float menuMaxAspect;     // live
    extern Fit menuBackground;      // live
    extern Fit loadingScreens;      // live
    extern Fit videos;              // live; Stretch or Pillarbox

    // [Cutscenes]
    extern Borders borders;         // live
    extern float borderSize;        // live; factor on the bar height
    extern float borderSlide;       // live; seconds for the bars to slide in/out, 0 = instant

    // [Startup]
    extern bool skipIntro;          // skip the logo and title videos

    // [Debug]
    extern char logPath[260];

    void Load(const char* iniPath);
    // Saves one setting to the ini (from the in-game menu).
    void Write(const char* section, const char* key, const char* value);
    // Re-reads the ini when it has changed on disk (at most once a second).
    void Poll();
}
