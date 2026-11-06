#include "config.hpp"
#include "features.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// "WIDESCREEN SETTINGS" in Display Setup > Advanced. The game's table of menu
// pages is full, so the settings page borrows the Advanced page: opening it
// swaps the page's rows for ours, Back/Esc puts them back.
//
// Labels and values are drawn here rather than through CText::Get: SA-MP and
// modloader both hook that function, and the game handles our rows fine with
// unknown text keys (CText::Get returns an empty string for those).
namespace
{
    // CMenuManager (FrontEndMenuManager)
    uint8_t* Menu() { return reinterpret_cast<uint8_t*>(0xBA6748); }
    int8_t& CurrentScreen() { return *reinterpret_cast<int8_t*>(Menu() + 0x15D); }
    int32_t& CurrentItem() { return *reinterpret_cast<int32_t*>(Menu() + 0x54); }

    constexpr int8_t kAdvancedPage = 27;  // SCREEN_DISPLAY_ADVANCED
    constexpr int8_t kGoBack = -2;        // SCREEN_GO_BACK

    enum : int8_t
    {
        kTypeEnter = 11,
        kTypeOption = 12,
        kAlignLeft = 1,
        kAlignRight = 2,
        kAlignCentre = 3,
        kActionBack = 2,
        kActionWidescreen = 26,  // the game's own WIDESCREEN row, which this mod makes pointless
    };

#pragma pack(push, 1)
    struct Item
    {
        int8_t action;
        char name[8];
        int8_t type;
        int8_t target;
        uint8_t pad;
        uint16_t x, y;
        int8_t align;
        uint8_t pad2;
    };
    struct Page
    {
        char title[8];
        int8_t parent;
        int8_t startItem;
        Item items[12];
    };
#pragma pack(pop)
    static_assert(sizeof(Item) == 0x12 && sizeof(Page) == 0xE2, "menu table layout");

    // ---- game functions --------------------------------------------------

    using DrawStandardMenusFn = void(__thiscall*)(void* menu, bool drawTitle);
    using ProcessMenuOptionsFn = void(__thiscall*)(void* menu, int8_t leftRight, bool* back, bool accept);
    using SwitchToNewScreenFn = void(__thiscall*)(void* menu, int8_t screen);
    DrawStandardMenusFn g_drawStandardMenus;
    ProcessMenuOptionsFn g_processMenuOptions;
    SwitchToNewScreenFn g_switchToNewScreen;

    const char* GameText(const char* key)
    {
        using GetFn = const char*(__thiscall*)(void* text, const char* key);
        return reinterpret_cast<GetFn>(0x6A0050)(reinterpret_cast<void*>(0xC1B340), key);  // TheText.Get
    }

    int DisplaySlider(float x, float y, float h1, float h2, float length, float value, int spacing)
    {
        using Fn = int(__thiscall*)(void*, float, float, float, float, float, float, int);
        return reinterpret_cast<Fn>(0x576860)(Menu(), x, y, h1, h2, length, value, spacing);
    }

    namespace font
    {
        uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return r | g << 8 | b << 16 | uint32_t(a) << 24; }
        void Scale(float w, float h) { reinterpret_cast<void(__cdecl*)(float, float)>(0x719380)(w, h); }
        void Color(uint32_t c) { reinterpret_cast<void(__cdecl*)(uint32_t)>(0x719430)(c); }
        void Style(uint8_t s) { reinterpret_cast<void(__cdecl*)(uint8_t)>(0x719490)(s); }
        void DropColor(uint32_t c) { reinterpret_cast<void(__cdecl*)(uint32_t)>(0x719510)(c); }
        void Edge(int8_t e) { reinterpret_cast<void(__cdecl*)(int8_t)>(0x719590)(e); }
        void Proportional(bool on) { reinterpret_cast<void(__cdecl*)(bool)>(0x7195B0)(on); }
        void Background(bool on, bool wrap) { reinterpret_cast<void(__cdecl*)(bool, bool)>(0x7195C0)(on, wrap); }
        void Orientation(int8_t o) { reinterpret_cast<void(__cdecl*)(int8_t)>(0x719610)(o); }
        void Print(float x, float y, const char* s) { reinterpret_cast<void(__cdecl*)(float, float, const char*)>(0x71A700)(x, y, s); }

        enum : uint8_t { kGothic = 0, kSubtitles = 1, kMenu = 2 };
        enum : int8_t { kCentre = 0, kLeft = 1, kRight = 2 };
        const uint32_t kNormal = Rgba(74, 90, 107), kSelected = Rgba(172, 203, 241), kBlack = Rgba(0, 0, 0);
        const uint32_t kDisabled = Rgba(14, 30, 47);  // the game's colour for MIP MAPPING when it cannot be changed
    }

    float Width() { return static_cast<float>(*reinterpret_cast<int*>(0xC17044)); }
    float Height() { return static_cast<float>(*reinterpret_cast<int*>(0xC17048)); }
    float StretchX(float v) { return v * Width() / 640.0f; }
    float StretchY(float v) { return v * Height() / 448.0f; }

    // ---- the settings ------------------------------------------------------
    //
    // Every setting the menu shows is one entry below: how to read and change
    // it, how to save it, its default, label and description. The pages, the
    // values, the descriptions and RESTORE DEFAULTS all come from these.

    enum class Kind : uint8_t { Toggle, Slider, Choice };

    struct Choice
    {
        const char* label;  // shown in the menu
        const char* ini;    // written to the ini
        float value;        // the setting's value
    };

    struct Range { float lo = 0.0f, hi = 1.0f, step = 0.05f; };  // Slider
    struct List { const Choice* choices = nullptr; int count = 0; };  // Choice
    struct Opt
    {
        bool (*offer)(int) = nullptr;  // Choice: which entries make sense here
        bool (*usable)() = nullptr;    // greyed out when false
        bool restart = false;          // only applies on the next start
        bool master = false;           // the WIDESCREEN switch itself
        const char* why = nullptr;     // shown when greyed out
    };

    struct Setting
    {
        const char* label;          // nullptr: the game's own WIDESCREEN text
        const char* help;           // bottom right while selected
        Kind kind;
        float (*get)();             // Toggle: 0/1, Slider: value, Choice: value
        void (*set)(float);
        void (*save)();
        float def;
        Range range = {};
        List list = {};
        Opt opt = {};
    };

    void Save(const char* section, const char* key, const char* value) { config::Write(section, key, value); }
    void SaveBool(const char* section, const char* key, bool v) { Save(section, key, v ? "1" : "0"); }
    void SaveFloat(const char* section, const char* key, float v, int decimals = 2)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        Save(section, key, buf);
    }

    // Aspect ratios, for the screen shape and the HUD / menu areas.
    constexpr Choice kAspects[] = {
        { "AUTO", "auto", 0.0f }, { "4 X 3", "4:3", 4.0f / 3.0f }, { "16 X 10", "16:10", 16.0f / 10.0f },
        { "16 X 9", "16:9", 16.0f / 9.0f }, { "21 X 9", "21:9", 21.0f / 9.0f }, { "32 X 9", "32:9", 32.0f / 9.0f },
    };
    constexpr Choice kAreas[] = {
        { "FULL", "0", 0.0f }, { "4 X 3", "4:3", 4.0f / 3.0f }, { "16 X 10", "16:10", 16.0f / 10.0f },
        { "16 X 9", "16:9", 16.0f / 9.0f }, { "21 X 9", "21:9", 21.0f / 9.0f },
    };
    // An area only does something when it is narrower than the screen.
    bool NarrowerThanScreen(int i) { return kAreas[i].value == 0.0f || kAreas[i].value < Width() / Height() - 0.01f; }
    bool AnyArea() { return NarrowerThanScreen(1); }

    constexpr Choice kLayouts[] = {
        { "EDGES", "edges", float(config::MenuLayout::Edges) }, { "CENTERED", "centered", float(config::MenuLayout::Centered) },
        { "STRETCHED", "stretch", float(config::MenuLayout::Stretch) },
    };
    constexpr Choice kFits[] = {
        { "CROP", "crop", float(config::Fit::Crop) }, { "PILLARBOX", "pillarbox", float(config::Fit::Pillarbox) },
        { "STRETCHED", "stretch", float(config::Fit::Stretch) },
    };
    constexpr Choice kVideoFits[] = {
        { "PILLARBOX", "pillarbox", float(config::Fit::Pillarbox) }, { "STRETCHED", "stretch", float(config::Fit::Stretch) },
    };
    constexpr Choice kBars[] = {
        { "ORIGINAL", "vanilla", float(config::Borders::Vanilla) }, { "OFF", "off", float(config::Borders::Off) },
        { "PILLARBOX", "pillarbox", float(config::Borders::Pillarbox) },
    };
    bool BarsOn() { return config::borders != config::Borders::Off; }

    template <size_t N>
    const char* IniOf(const Choice (&list)[N], float value)
    {
        for (const Choice& c : list)
            if (std::fabs(c.value - value) < 0.01f)
                return c.ini;
        return list[0].ini;
    }

    // HUD parts, edited one at a time on the HUD Parts page.
    using config::HudPart;
    struct PartInfo { const char* label; const char* section; };
    constexpr PartInfo kParts[] = {  // in HudPart order
        { "RADAR", "HUD.Radar" }, { "PLAYER INFO", "HUD.PlayerInfo" }, { "WANTED LEVEL", "HUD.Wanted" },
        { "TIMERS", "HUD.Timers" }, { "AREA NAMES", "HUD.ZoneNames" }, { "MISSION TITLE", "HUD.MissionTitle" },
        { "HELP TEXT", "HUD.HelpText" }, { "SUBTITLES", "HUD.Subtitles" }, { "MESSAGES", "HUD.Messages" },
        { "RADIO NAME", "HUD.RadioName" }, { "CROSSHAIR", "HUD.Crosshair" },
    };
    static_assert(sizeof(kParts) / sizeof(*kParts) == static_cast<int>(HudPart::Count));
    constexpr Choice kPartChoices[] = {
        { "RADAR", "", 0 }, { "PLAYER INFO", "", 1 }, { "WANTED LEVEL", "", 2 }, { "TIMERS", "", 3 }, { "AREA NAMES", "", 4 },
        { "MISSION TITLE", "", 5 }, { "HELP TEXT", "", 6 }, { "SUBTITLES", "", 7 }, { "MESSAGES", "", 8 },
        { "RADIO NAME", "", 9 }, { "CROSSHAIR", "", 10 },
    };
    int g_part = 0;
    config::HudPartSettings& Part() { return config::hudParts[g_part]; }
    const char* PartSection() { return kParts[g_part].section; }
    bool PartMoves() { return g_part != static_cast<int>(HudPart::Crosshair); }  // moving it would miss the aim

    // clang-format off
    const Setting sWidescreen { nullptr, "TURN THE WIDESCREEN FIX ON OR OFF", Kind::Toggle,
        [] { return float(config::widescreen); }, [](float v) { config::widescreen = v > 0.5f; },
        [] { SaveBool("General", "Widescreen", config::widescreen); }, 1.0f, {}, {}, Opt{ .master = true } };
    const Setting sAspect { "ASPECT RATIO", "SCREEN SHAPE", Kind::Choice,
        [] { return config::forcedAspect; }, [](float v) { config::forcedAspect = v; },
        [] { Save("General", "AspectRatio", IniOf(kAspects, config::forcedAspect)); }, 0.0f, {}, { kAspects, 6 } };
    const Setting sFov { "FIELD OF VIEW", "HOW WIDE THE CAMERA SEES", Kind::Slider,
        [] { return config::fovScale; }, [](float v) { config::fovScale = v; },
        [] { SaveFloat("Camera", "FOV", config::fovScale); }, 1.0f, { 0.5f, 2.0f }, {} };
    const Setting sHudSize { "HUD SIZE", "SIZE OF THE WHOLE HUD", Kind::Slider,
        [] { return config::hudScale; }, [](float v) { config::hudScale = v; },
        [] { SaveFloat("HUD", "Scale", config::hudScale); }, 1.0f, { 0.6f, 1.4f }, {} };
    const Setting sRadarSize { "RADAR SIZE", "SIZE OF THE RADAR", Kind::Slider,
        [] { return config::hudParts[int(HudPart::Radar)].scale; }, [](float v) { config::hudParts[int(HudPart::Radar)].scale = v; },
        [] { SaveFloat("HUD.Radar", "Scale", config::hudParts[int(HudPart::Radar)].scale); }, 1.0f, { 0.5f, 2.0f }, {} };
    const Setting sCrosshairSize { "CROSSHAIR SIZE", "SIZE OF THE AIMING CROSSHAIR", Kind::Slider,
        [] { return config::hudParts[int(HudPart::Crosshair)].scale; }, [](float v) { config::hudParts[int(HudPart::Crosshair)].scale = v; },
        [] { SaveFloat("HUD.Crosshair", "Scale", config::hudParts[int(HudPart::Crosshair)].scale); }, 1.0f, { 0.5f, 2.0f }, {} };
    const Setting sRoundRadar { "ROUND RADAR", "MAKE THE RADAR A CIRCLE", Kind::Toggle,
        [] { return float(config::roundRadar); }, [](float v) { config::roundRadar = v > 0.5f; },
        [] { SaveBool("HUD.Radar", "Round", config::roundRadar); }, 1.0f, {}, {} };
    const Setting sSquareIcon { "SQUARE WEAPON ICON", "MAKE THE WEAPON ICON SQUARE", Kind::Toggle,
        [] { return float(config::squareWeaponIcon); }, [](float v) { config::squareWeaponIcon = v > 0.5f; },
        [] { SaveBool("HUD", "SquareWeaponIcon", config::squareWeaponIcon); }, 1.0f, {}, {} };
    const Setting sHudArea { "HUD AREA", "KEEP THE HUD IN A NARROWER AREA", Kind::Choice,
        [] { return config::hudMaxAspect; }, [](float v) { config::hudMaxAspect = v; },
        [] { Save("HUD", "Area", IniOf(kAreas, config::hudMaxAspect)); }, 0.0f, {}, { kAreas, 5 }, Opt{ .offer = NarrowerThanScreen, .usable = AnyArea, .why = "YOUR SCREEN IS ALREADY 4 X 3" } };

    const Setting sMenuLayout { "MENU LAYOUT", "WHERE MENU TITLES AND PICTURES GO", Kind::Choice,
        [] { return float(config::menuLayout); }, [](float v) { config::menuLayout = config::MenuLayout(int(v)); },
        [] { Save("Menus", "Layout", IniOf(kLayouts, float(config::menuLayout))); }, float(config::MenuLayout::Edges), {}, { kLayouts, 3 } };
    const Setting sMenuArea { "MENU AREA", "KEEP MENU TITLES AND PICTURES IN A NARROWER AREA", Kind::Choice,
        [] { return config::menuMaxAspect; }, [](float v) { config::menuMaxAspect = v; },
        [] { Save("Menus", "Area", IniOf(kAreas, config::menuMaxAspect)); }, 0.0f, {}, { kAreas, 5 }, Opt{ .offer = NarrowerThanScreen, .usable = AnyArea, .why = "YOUR SCREEN IS ALREADY 4 X 3" } };
    const Setting sWideMap { "WIDE MAP", "MAP USES THE WHOLE SCREEN", Kind::Toggle,
        [] { return float(config::wideMap); }, [](float v) { config::wideMap = v > 0.5f; },
        [] { SaveBool("Menus", "WideMap", config::wideMap); }, 1.0f, {}, {}, Opt{ .restart = true } };
    const Setting sBackgrounds { "BACKGROUNDS", "HOW MENU ARTWORK FILLS THE SCREEN", Kind::Choice,
        [] { return float(config::menuBackground); }, [](float v) { config::menuBackground = config::Fit(int(v)); },
        [] { Save("Menus", "Backgrounds", IniOf(kFits, float(config::menuBackground))); }, float(config::Fit::Crop), {}, { kFits, 3 } };
    const Setting sLoading { "LOADING SCREENS", "HOW LOADING SCREENS FILL THE SCREEN", Kind::Choice,
        [] { return float(config::loadingScreens); }, [](float v) { config::loadingScreens = config::Fit(int(v)); },
        [] { Save("Menus", "LoadingScreens", IniOf(kFits, float(config::loadingScreens))); }, float(config::Fit::Pillarbox), {}, { kFits, 3 } };
    const Setting sVideos { "INTRO VIDEOS", "HOW THE INTRO VIDEOS ARE SHOWN", Kind::Choice,
        [] { return float(config::videos); }, [](float v) { config::videos = config::Fit(int(v)); },
        [] { Save("Menus", "Videos", IniOf(kVideoFits, float(config::videos))); }, float(config::Fit::Pillarbox), {}, { kVideoFits, 2 } };
    const Setting sSkipIntro { "SKIP INTRO", "SKIP THE LOGO AND INTRO VIDEOS", Kind::Toggle,
        [] { return float(config::skipIntro); }, [](float v) { config::skipIntro = v > 0.5f; },
        [] { SaveBool("Startup", "SkipIntro", config::skipIntro); }, 0.0f, {}, {}, Opt{ .restart = true } };
    const Setting sBars { "CUTSCENE BARS", "BLACK BARS IN CUTSCENES", Kind::Choice,
        [] { return float(config::borders); }, [](float v) { config::borders = config::Borders(int(v)); },
        [] { Save("Cutscenes", "Bars", IniOf(kBars, float(config::borders))); }, float(config::Borders::Vanilla), {}, { kBars, 3 } };
    const Setting sBarSize { "BAR SIZE", "HEIGHT OF THE CUTSCENE BARS", Kind::Slider,
        [] { return config::borderSize; }, [](float v) { config::borderSize = v; },
        [] { SaveFloat("Cutscenes", "BarSize", config::borderSize); }, 1.0f, { 0.0f, 2.0f, 0.1f }, {}, Opt{ .usable = BarsOn, .why = "NOT USED WHILE CUTSCENE BARS ARE OFF" } };
    const Setting sBarSlide { "BAR SLIDE", "CUTSCENE BARS SLIDE IN AND OUT", Kind::Toggle,
        [] { return float(config::borderSlide > 0.0f); }, [](float v) { config::borderSlide = v > 0.5f ? 0.5f : 0.0f; },
        [] { SaveFloat("Cutscenes", "BarSlide", config::borderSlide, 1); }, 0.0f, {}, {}, Opt{ .usable = BarsOn, .why = "NOT USED WHILE CUTSCENE BARS ARE OFF" } };

    const Setting sHorPlus { "WIDER VIEW", "SHOW MORE AT THE SIDES", Kind::Toggle,
        [] { return float(config::horPlus); }, [](float v) { config::horPlus = v > 0.5f; },
        [] { SaveBool("Camera", "HorPlus", config::horPlus); }, 1.0f, {}, {} };
    const Setting sHudWidth { "HUD WIDTH", "WIDTH OF THE WHOLE HUD", Kind::Slider,
        [] { return config::hudWidth; }, [](float v) { config::hudWidth = v; },
        [] { SaveFloat("HUD", "Width", config::hudWidth); }, 1.0f, { 0.5f, 1.5f }, {} };
    const Setting sHudHeight { "HUD HEIGHT", "HEIGHT OF THE WHOLE HUD", Kind::Slider,
        [] { return config::hudHeight; }, [](float v) { config::hudHeight = v; },
        [] { SaveFloat("HUD", "Height", config::hudHeight); }, 1.0f, { 0.5f, 1.5f }, {} };
    const Setting sCoronas { "ROUND CORONAS", "KEEP LIGHTS AND THE SUN ROUND", Kind::Toggle,
        [] { return float(config::proportionalSprites); }, [](float v) { config::proportionalSprites = v > 0.5f; },
        [] { SaveBool("Camera", "RoundCoronas", config::proportionalSprites); }, 1.0f, {}, {}, Opt{ .restart = true } };
    const Setting sWaterFix { "WATER FIX", "FIX WATER MISSING AT THE SCREEN SIDES", Kind::Toggle,
        [] { return float(config::waterFix); }, [](float v) { config::waterFix = v > 0.5f; },
        [] { SaveBool("Camera", "WaterFix", config::waterFix); }, 1.0f, {}, {}, Opt{ .restart = true } };
    const Setting sScripts { "SCRIPT DRAWING", "ALSO FIX TEXT AND PICTURES FROM SCRIPTS", Kind::Toggle,
        [] { return float(config::scriptDrawing); }, [](float v) { config::scriptDrawing = v > 0.5f; },
        [] { SaveBool("HUD", "ScriptDrawing", config::scriptDrawing); }, 0.0f, {}, {}, Opt{ .restart = true } };
    const Setting sLivePreview { "LIVE PREVIEW", "SHOW THE GAME BEHIND THE CAMERA AND HUD PAGES", Kind::Toggle,
        [] { return float(config::liveMenuPreview); }, [](float v) { config::liveMenuPreview = v > 0.5f; },
        [] { SaveBool("Menus", "LivePreview", config::liveMenuPreview); }, 1.0f, {}, {}, Opt{ .restart = true } };

    const Setting sPart { "PART", "PART OF THE HUD TO ADJUST", Kind::Choice,
        [] { return float(g_part); }, [](float v) { g_part = int(v); }, [] {}, 0.0f, {}, { kPartChoices, 11 } };
    const Setting sPartSize { "SIZE", "SIZE OF THIS PART", Kind::Slider,
        [] { return Part().scale; }, [](float v) { Part().scale = v; },
        [] { SaveFloat(PartSection(), "Scale", Part().scale); }, 1.0f, { 0.5f, 2.0f }, {} };
    const Setting sPartWidth { "WIDTH", "WIDTH OF THIS PART", Kind::Slider,
        [] { return Part().width; }, [](float v) { Part().width = v; },
        [] { SaveFloat(PartSection(), "Width", Part().width); }, 1.0f, { 0.5f, 2.0f }, {} };
    const Setting sPartX { "MOVE SIDEWAYS", "MOVE THIS PART LEFT OR RIGHT", Kind::Slider,
        [] { return Part().offsetX; }, [](float v) { Part().offsetX = v; },
        [] { SaveFloat(PartSection(), "OffsetX", Part().offsetX, 0); }, 0.0f, { -300.0f, 300.0f, 10.0f }, {}, Opt{ .usable = PartMoves, .why = "THE CROSSHAIR CAN ONLY BE RESIZED" } };
    const Setting sPartY { "MOVE UP / DOWN", "MOVE THIS PART UP OR DOWN", Kind::Slider,
        [] { return Part().offsetY; }, [](float v) { Part().offsetY = v; },
        [] { SaveFloat(PartSection(), "OffsetY", Part().offsetY, 0); }, 0.0f, { -300.0f, 300.0f, 10.0f }, {}, Opt{ .usable = PartMoves, .why = "THE CROSSHAIR CAN ONLY BE RESIZED" } };
    // clang-format on

    // ---- the pages -----------------------------------------------------------
    //
    // Like the game's Options page: a list of categories, each opening its own
    // page; BACK on a category page returns to the list.

    struct PageDef
    {
        const char* name;       // in the list, and the page's title
        const char* title;
        const char* contents;   // shown while the category is selected
        std::initializer_list<const Setting*> rows;
        bool live = false;      // the game shows behind this page (it changes the view or the HUD)
    };

    const PageDef kPages[] = {
        { "GENERAL", "General", "WIDESCREEN ON OR OFF / ASPECT RATIO / LIVE PREVIEW / SKIP INTRO",
          { &sWidescreen, &sAspect, &sLivePreview, &sSkipIntro } },
        { "CAMERA", "Camera", "WIDER VIEW / FIELD OF VIEW / CORONAS / WATER",
          { &sHorPlus, &sFov, &sCoronas, &sWaterFix }, true },
        { "HUD", "HUD", "SIZES / ROUND RADAR / WEAPON ICON / HUD AREA",
          { &sHudSize, &sHudWidth, &sHudHeight, &sHudArea, &sRadarSize, &sRoundRadar, &sCrosshairSize, &sSquareIcon, &sScripts }, true },
        { "HUD PARTS", "HUD Parts", "SIZE AND POSITION OF EACH PART OF THE HUD",
          { &sPart, &sPartSize, &sPartWidth, &sPartX, &sPartY }, true },
        { "MENUS", "Menus", "MENU LAYOUT / MAP / BACKGROUNDS / LOADING SCREENS / VIDEOS",
          { &sMenuLayout, &sMenuArea, &sWideMap, &sBackgrounds, &sLoading, &sVideos } },
        { "CUTSCENES", "Cutscenes", "CUTSCENE BARS / BAR SIZE / BAR SLIDE",
          { &sBars, &sBarSize, &sBarSlide } },
    };
    constexpr int kPageCount = sizeof(kPages) / sizeof(*kPages);
    constexpr int kList = -1;  // g_page while the category list is shown

    // Actions of the generated rows (the game's own go up to 65).
    enum Action : int8_t
    {
        kOpen = 100,       // WIDESCREEN SETTINGS on the Advanced page
        kReset,            // RESTORE DEFAULTS
        kClose,            // BACK
        kRowBase = 110,    // + row index: a setting
        kCategoryBase = 80 // + page index: a category in the list
    };

    constexpr const char* kResetHelp = "RESTORE DEFAULTS FOR THIS PAGE";
    constexpr const char* kResetPartHelp = "RESTORE DEFAULTS FOR THIS PART";
    constexpr const char* kEntryHelp = "FIELD OF VIEW / HUD / RADAR / MENUS / CUTSCENE BARS";

    // Category pages: rows on the left, every row with an explicit Y (the
    // game's automatic 30-unit spacing would not fit long pages). The first
    // row stays below the top quarter, which the menu layout treats as the
    // title band (pushed to the screen edges).
    constexpr uint16_t kFirstRowX = 57, kFirstRowY = 116, kRowHeight = 24;
    uint16_t RowY(int i) { return static_cast<uint16_t>(kFirstRowY + i * kRowHeight); }
    constexpr uint16_t kPageResetY = 360, kPageBackY = 390;
    // The list: centred entries, BACK to the side where the Options page has it.
    constexpr uint16_t kListX = 320, kListY = 130, kListHeight = 30, kListBackX = 490, kListBackY = 380;

    int RowsOf(const PageDef& p) { return static_cast<int>(p.rows.size()); }
    bool PagesFit()
    {
        if (kPageCount + 1 > 12)
            return false;
        for (const PageDef& p : kPages)
            if (RowsOf(p) + 2 > 12 || RowY(RowsOf(p) - 1) + kRowHeight > kPageResetY)
                return false;
        return true;
    }

    Page* g_table = nullptr;
    int g_entryRow = -1;      // our row on the Advanced page
    bool g_open = false;      // the Advanced page currently shows our rows
    int g_page = kList;       // the list, or one of the pages
    Page g_saved;             // its own rows while it does

    const Setting* RowSetting(int item)
    {
        if (g_page == kList)
            return nullptr;
        const PageDef& p = kPages[g_page];
        return item >= 0 && item < RowsOf(p) ? p.rows.begin()[item] : nullptr;
    }

    // ---- the menu table ----------------------------------------------------

    // The table the game actually uses: other mods (open.mp, ...) may move it
    // to add pages, so it is read from the game's own code rather than
    // assumed at 0x8CE008. CMenuManager::ProcessMenuOptions reads an item's
    // action with `movzx edi, byte [ecx + table + 0xA]`.
    Page* FindTable()
    {
        static const uint8_t movzx[] = { 0x0F, 0xB6, 0xB9 };
        const uintptr_t at = mem::Address(0x577014);
        if (!mem::Matches(0x577014, movzx, sizeof(movzx)))
        {
            Log("menu: CMenuManager::ProcessMenuOptions is not as expected; no Widescreen Settings");
            return nullptr;
        }
        const uint32_t actionField = *reinterpret_cast<const uint32_t*>(at + 3);
        Page* table = reinterpret_cast<Page*>(actionField - 0xA);
        if (actionField != 0x8CE012)
            Log("menu: the menu table was moved by another mod (now at %08X)", static_cast<unsigned>(actionField - 0xA));
        return table;
    }

    Item MakeItem(int8_t action, const char* key, int8_t type, uint16_t x, uint16_t y, int8_t align)
    {
        Item it{};
        it.action = action;
        std::memcpy(it.name, key, std::min(std::strlen(key), sizeof(it.name) - 1));  // rest stays zero
        it.type = type;
        it.target = kAdvancedPage;
        it.x = x;
        it.y = y;
        it.align = align;
        return it;
    }

    // Puts the entry row on the Advanced page, in place of the game's
    // WIDESCREEN row; failing that in a free row before BACK.
    void AddEntryRow(Page& page)
    {
        const Item entry = MakeItem(kOpen, "NWS_ENT", kTypeEnter, 0, 0, kAlignLeft);
        for (int i = 0; i < 12; ++i)
        {
            Item& it = page.items[i];
            if (it.action == kActionWidescreen || !std::strncmp(it.name, "FED_WIS", 8))
            {
                const uint16_t x = it.x, y = it.y;
                it = entry;
                it.x = x;  // keep the position the game already laid out
                it.y = y;
                g_entryRow = i;
                return;
            }
        }
        for (int i = 1; i < 11; ++i)
        {
            if (page.items[i].action == kActionBack && !page.items[i + 1].action && !page.items[i + 1].name[0])
            {
                page.items[i + 1] = page.items[i];
                page.items[i] = entry;
                g_entryRow = i;
                return;
            }
        }
        Log("menu: no room on Display > Advanced for Widescreen Settings");
    }

    bool Ready()
    {
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (!PagesFit())
            {
                Log("menu: a settings page has more than 12 rows; no Widescreen Settings");
                return false;
            }
            g_table = FindTable();
            if (g_table)
                AddEntryRow(g_table[kAdvancedPage]);
        }
        return g_table && g_entryRow >= 0;
    }

    void BeginPage()
    {
        Page& page = g_table[kAdvancedPage];
        if (!g_open)
            g_saved = page;
        std::memset(page.items, 0, sizeof(page.items));
        std::strncpy(page.title, "NWS_TTL", sizeof(page.title));
        page.startItem = 0;
        g_open = true;
    }

    void RowFont(bool selected, int8_t orientation, int8_t edge, bool available = true);

    // Width of a row label in canvas units, measured with the game's own font
    // code (CFont::GetStringWidth) in the row font.
    float LabelWidth(const char* text)
    {
        RowFont(false, font::kLeft, 2);
        const float px = reinterpret_cast<float(__cdecl*)(const char*, bool, bool)>(0x71A0E0)(text, true, false);
        return px * 640.0f / Width();
    }

    // The category list. Entries look centred, but are laid out from their
    // measured left edge: the game places the selection arrow of a centred
    // row from the width of the row's own text, and ours have none.
    void OpenList(int cursor = 0)
    {
        BeginPage();
        Page& page = g_table[kAdvancedPage];
        int n = 0;
        for (; n < kPageCount; ++n)
        {
            const float left = kListX - 0.5f * LabelWidth(kPages[n].name);
            page.items[n] = MakeItem(int8_t(kCategoryBase + n), "NWS_ROW", kTypeEnter, static_cast<uint16_t>(std::max(0.0f, left)),
                                     uint16_t(kListY + n * kListHeight), kAlignLeft);
        }
        // The game's own BACK text (FEDS_TB), to the side like on the Options page.
        page.items[n] = MakeItem(kClose, "FEDS_TB", kTypeEnter, kListBackX, kListBackY, kAlignLeft);
        g_page = kList;
        CurrentItem() = cursor;
    }

    // One category's page: its settings, then BACK.
    void OpenPage(int index)
    {
        BeginPage();
        Page& page = g_table[kAdvancedPage];
        const PageDef& def = kPages[index];
        int n = 0;
        for (; n < RowsOf(def); ++n)
            page.items[n] = MakeItem(int8_t(kRowBase + n), "NWS_ROW", kTypeOption, kFirstRowX, RowY(n), kAlignLeft);
        // The game's own RESTORE DEFAULTS and BACK texts (FET_DEF, FEDS_TB), centred
        // at the bottom like on Display Setup, whatever the page's length.
        page.items[n] = MakeItem(kReset, "FET_DEF", kTypeEnter, 320, kPageResetY, kAlignCentre);
        page.items[n + 1] = MakeItem(kClose, "FEDS_TB", kTypeEnter, 320, kPageBackY, kAlignCentre);
        g_page = index;
        CurrentItem() = 0;
    }

    void Close()
    {
        if (!g_open)
            return;
        g_table[kAdvancedPage] = g_saved;
        g_open = false;
    }

    // BACK / Esc: from a page to the list (on that category), from the list out.
    void GoBack()
    {
        if (g_page != kList)
            OpenList(g_page);
        else
        {
            Close();
            CurrentItem() = g_entryRow;
        }
    }

    // ---- changing settings -------------------------------------------------------

    bool Available(const Setting& s)
    {
        if (!s.opt.master && !config::widescreen)
            return false;
        return !s.opt.usable || s.opt.usable();
    }

    int ChoiceIndex(const Setting& s)  // -1 for a value set in the ini that is not listed
    {
        const float v = s.get();
        for (int i = 0; i < s.list.count; ++i)
            if (std::fabs(s.list.choices[i].value - v) < 0.01f)
                return i;
        return -1;
    }

    void Change(const Setting& s, int dir)
    {
        if (!Available(s) || !dir)
            return;
        switch (s.kind)
        {
        case Kind::Toggle:
            s.set(s.get() > 0.5f ? 0.0f : 1.0f);
            break;
        case Kind::Slider:
        {
            const float n = std::round((s.get() - s.range.lo) / s.range.step) + dir;
            s.set(std::clamp(s.range.lo + n * s.range.step, s.range.lo, s.range.hi));
            break;
        }
        case Kind::Choice:
        {
            int i = ChoiceIndex(s);
            if (i < 0)
                i = 0;
            for (int n = 0; n < s.list.count; ++n)
            {
                i = (i + dir + s.list.count) % s.list.count;
                if (!s.opt.offer || s.opt.offer(i))
                    break;
            }
            s.set(s.list.choices[i].value);
            break;
        }
        }
        s.save();
    }

    // RESTORE DEFAULTS on a page: that page's settings; on HUD Parts, the
    // part being shown.
    void RestoreDefaults(int page)
    {
        for (const Setting* st : kPages[page].rows)
        {
            if (st == &sPart)
                continue;  // which part is shown, not a setting
            st->set(st->def);
            st->save();
        }
        // The game's own "DEFAULT SETTINGS RESTORED" helper text (FET_HRD).
        reinterpret_cast<void(__thiscall*)(void*, int)>(0x57CD10)(Menu(), 2);
    }

    // Text for the right column, or nullptr for a slider (position in `slider`).
    const char* Value(const Setting& s, float& slider)
    {
        switch (s.kind)
        {
        case Kind::Toggle:
            return GameText(s.get() > 0.5f ? "FEM_ON" : "FEM_OFF");
        case Kind::Slider:
            slider = (s.get() - s.range.lo) / (s.range.hi - s.range.lo);
            return nullptr;
        case Kind::Choice:
        {
            const int i = ChoiceIndex(s);
            return i >= 0 ? s.list.choices[i].label : "CUSTOM";
        }
        }
        return "";
    }

    // ---- drawing ---------------------------------------------------------------

    void RowFont(bool selected, int8_t orientation, int8_t edge, bool available)
    {
        font::Style(font::kMenu);
        font::Scale(StretchX(0.7f), StretchY(1.0f));
        font::Edge(edge);
        font::Proportional(true);
        font::Background(false, false);
        font::DropColor(font::kBlack);
        font::Color(!available ? font::kDisabled : selected ? font::kSelected : font::kNormal);
        font::Orientation(orientation);
    }

    void DrawLabel(const Item& it, bool selected, const char* text, bool available = true)
    {
        RowFont(selected, font::kLeft, 2, available);
        font::Print(StretchX(it.x), StretchY(it.y), text);
    }

    // The game handles the mouse only for its own sliders: click or drag on
    // ours sets the value from the pointer position.
    constexpr float kSliderX = 500.0f, kSliderLength = 100.0f;

    bool MouseHeld() { return *reinterpret_cast<const uint8_t*>(0xB73418) != 0; }  // CPad::NewMouseControllerState.lmb
    int MouseX() { return *reinterpret_cast<const int*>(Menu() + 0xBC); }
    int MouseY() { return *reinterpret_cast<const int*>(Menu() + 0xC0); }

    void SliderMouse(int row, const Item& it, const Setting& s)
    {
        if (!MouseHeld() || !Available(s))
            return;
        const float x0 = StretchX(kSliderX), length = StretchX(kSliderLength);
        const float mx = static_cast<float>(MouseX()), my = static_cast<float>(MouseY());
        if (my < StretchY(it.y - 4.0f) || my > StretchY(it.y + 24.0f) || mx < x0 - StretchX(6.0f) || mx > x0 + length + StretchX(6.0f))
            return;

        CurrentItem() = row;
        const float t = std::clamp((mx - x0) / length, 0.0f, 1.0f);
        const float value = std::clamp(s.range.lo + std::round(t * (s.range.hi - s.range.lo) / s.range.step) * s.range.step, s.range.lo, s.range.hi);
        if (std::fabs(s.get() - value) < 0.001f)
            return;
        s.set(value);
        s.save();
    }

    bool Live();

    void DrawTitle(const char* title)
    {
        font::Style(font::kGothic);
        font::Scale(StretchX(1.3f), StretchY(2.1f));
        font::Edge(1);
        font::Orientation(font::kLeft);
        font::Color(font::kSelected);
        font::DropColor(font::kBlack);
        font::Print(StretchX(40.0f), StretchY(28.0f), title);
    }

    void DrawList()
    {
        Page& page = g_table[kAdvancedPage];
        DrawTitle("Widescreen");
        for (int i = 0; i < kPageCount; ++i)
        {
            const Item& it = page.items[i];
            RowFont(i == CurrentItem(), font::kLeft, 2);
            font::Print(StretchX(it.x), StretchY(it.y), kPages[i].name);
        }
    }

    void DrawPage()
    {
        if (g_page == kList)
            return DrawList();

        Page& page = g_table[kAdvancedPage];
        const PageDef& def = kPages[g_page];

        // Nothing to see of the crosshair unless aiming: show it where it is set.
        const bool crosshairPage = def.rows.begin()[0] == &sHudSize ||
                                   (def.rows.begin()[0] == &sPart && g_part == static_cast<int>(HudPart::Crosshair));
        if (Live() && crosshairPage)
            DrawCrosshairPreview();

        DrawTitle(def.title);

        for (int i = 0; i < RowsOf(def); ++i)
        {
            const Setting& s = *def.rows.begin()[i];
            const Item& it = page.items[i];
            const bool selected = i == CurrentItem();
            const bool available = Available(s);
            DrawLabel(it, selected, s.label ? s.label : GameText("FED_WIS"), available);

            float slider = 0.0f;
            if (const char* text = Value(s, slider))
            {
                RowFont(selected, font::kRight, 1, available);
                font::Print(Width() - StretchX(40.0f), StretchY(it.y), text);
            }
            else
            {
                SliderMouse(i, it, s);
                Value(s, slider);  // after a possible change
                DisplaySlider(StretchX(kSliderX), StretchY(it.y - 2.0f), StretchY(4.0f), StretchY(20.0f), StretchX(kSliderLength),
                              std::clamp(slider, 0.0f, 1.0f), static_cast<int>(StretchX(3.0f)));
            }
        }
    }

    // ---- live preview ---------------------------------------------------------------
    //
    // On the settings page the game keeps drawing the world and the HUD behind a
    // darkened menu, so FOV, HUD and radar changes show as they are made. The
    // main loop (Idle) normally skips the world while the menu is open.

    bool MenuActive() { return *reinterpret_cast<const uint8_t*>(0xBA6748 + 0x5C) != 0; }  // m_bMenuActive

    // Idle (the in-game loop) runs only once a game is loaded; before that the
    // main menu has its own loop and there is no world to show behind the page.
    DWORD g_lastIdle = 0;
    bool InGame() { return g_lastIdle && GetTickCount() - g_lastIdle < 250; }

    bool Live()
    {
        return config::liveMenuPreview && g_open && g_page != kList && kPages[g_page].live && MenuActive() &&
               CurrentScreen() == kAdvancedPage && InGame();
    }

    // Replaces `mov al, [FrontEndMenuManager.m_bMenuActive]` where Idle chooses
    // between drawing the world and drawing only the menu.
    extern "C" uint8_t __cdecl IdleMenuActive()
    {
        g_lastIdle = GetTickCount();
        return MenuActive() && !Live();
    }

    // The world path recentres the mouse every frame (RsMouseSetPos); not
    // while the menu's cursor is in use.
    using MouseSetPosFn = void(__cdecl*)(void* pos);
    MouseSetPosFn g_mouseSetPos;
    void __cdecl IdleMouseSetPos(void* pos)
    {
        if (!Live())
            g_mouseSetPos(pos);
    }

    // CMenuManager::DrawBackground: the full-screen fill becomes a dark veil and
    // the page artwork is left out.
    using DrawRectFn = void(__cdecl*)(const void* rect, const uint8_t* rgba);
    using SpriteDrawFn = void(__thiscall*)(void* sprite, const void* rect, const uint8_t* rgba);
    DrawRectFn g_backgroundRect;
    SpriteDrawFn g_backgroundSprite;

    void __cdecl BackgroundRect(const void* rect, const uint8_t* rgba)
    {
        static const uint8_t veil[4] = { 0, 0, 0, 150 };
        g_backgroundRect(rect, Live() ? veil : rgba);
    }

    void __thiscall BackgroundSprite(void* sprite, const void* rect, const uint8_t* rgba)
    {
        if (!Live())
            g_backgroundSprite(sprite, rect, rgba);
    }

    // Repoints a `call rel32` whose current target is `target`; returns that
    // target (to call on), or nullptr.
    // Other mods (SilentPatch, open.mp, ...) may already have redirected the
    // call: then whatever it calls now is wrapped instead.
    void* RedirectCall(uintptr_t at, uintptr_t target, const void* to, const char* what)
    {
        const uintptr_t site = mem::Address(at);
        if (*reinterpret_cast<const uint8_t*>(site) != 0xE8)
        {
            Log("menu: %s at %08X is not a call any more; no live preview", what, static_cast<unsigned>(at));
            return nullptr;
        }
        const uintptr_t current = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
        if (current != mem::Address(target))
            Log("menu: %s at %08X was redirected by another mod; chaining", what, static_cast<unsigned>(at));
        mem::WriteCall(at, to, 5);
        return reinterpret_cast<void*>(current);
    }

    void InstallLivePreview()
    {
        static const uint8_t readMenuActive[] = { 0xA0, 0xA4, 0x67, 0xBA, 0x00 };  // mov al, [0xBA67A4]
        if (!mem::Matches(0x53E9AC, readMenuActive, sizeof(readMenuActive)))
        {
            Log("menu: Idle at 0053E9AC is already patched; no live preview");
            return;
        }
        g_mouseSetPos = reinterpret_cast<MouseSetPosFn>(RedirectCall(0x53E9F1, 0x6194A0, reinterpret_cast<const void*>(&IdleMouseSetPos), "mouse recentring"));
        g_backgroundRect = reinterpret_cast<DrawRectFn>(RedirectCall(0x57B9BB, 0x727B60, reinterpret_cast<const void*>(&BackgroundRect), "menu background"));
        g_backgroundSprite = reinterpret_cast<SpriteDrawFn>(RedirectCall(0x57B9FD, 0x728350, reinterpret_cast<const void*>(&BackgroundSprite), "menu artwork"));
        if (!g_mouseSetPos || !g_backgroundRect || !g_backgroundSprite)
            return;  // without all three the world would show under an opaque menu, or the cursor would stick
        mem::WriteCall(0x53E9AC, reinterpret_cast<const void*>(&IdleMenuActive), sizeof(readMenuActive));
    }

    // ---- helper text ----------------------------------------------------------------
    //
    // The line at the bottom right ("LEFT / RIGHT / MOUSEWHEEL - ADJUST")
    // describes the selected row on our page, with the game's own hint below.

    using DisplayHelperTextFn = void(__thiscall*)(void* menu, const char* key);
    DisplayHelperTextFn g_displayHelperText;

    void PrintHelper(const char* line1, const char* hintKey, const char* line2 = nullptr)
    {
        static char text[256];
        if (hintKey)
            line2 = GameText(hintKey);
        if (line2)
            std::snprintf(text, sizeof(text), "%s~n~%s", line1, line2);
        else
            std::snprintf(text, sizeof(text), "%s", line1);

        // CMenuManager::DisplayHelperText's own style and position.
        font::Scale(StretchX(0.4f), StretchY(0.6f));
        font::Style(font::kMenu);
        font::Orientation(font::kRight);
        font::Edge(0);
        font::Color(font::Rgba(255, 255, 255));
        reinterpret_cast<void(__cdecl*)(float, float, const char*)>(0x71A820)(StretchX(610.0f), Height() - StretchY(10.0f), text);
    }

    void __thiscall DisplayHelperText(void* menu, const char* key)
    {
        const bool ours = !key && g_table && CurrentScreen() == kAdvancedPage && (g_open || CurrentItem() == g_entryRow);
        const int32_t transient = *reinterpret_cast<const int32_t*>(Menu() + 0x1B18);  // m_HelperText ("DEFAULT SETTINGS RESTORED", ...)
        if (!ours || transient)
            return g_displayHelperText(menu, key);

        if (!g_open)
            return PrintHelper(kEntryHelp, "FEH_JMP");  // CLICK LMB / RETURN - ENTER MENU

        const int8_t action = g_table[kAdvancedPage].items[CurrentItem()].action;
        if (const Setting* s = RowSetting(CurrentItem()))
        {
            if (!Available(*s))
                PrintHelper(!config::widescreen ? "NOT USED WHILE WIDESCREEN IS OFF" : s->opt.why ? s->opt.why : "NOT AVAILABLE", nullptr);
            else if (s->opt.restart)
                PrintHelper(s->help, nullptr, "APPLIES AFTER A RESTART");
            else
                PrintHelper(s->help, "FET_MIG");  // LEFT / RIGHT / MOUSEWHEEL - ADJUST
        }
        else if (action >= kCategoryBase && action < kCategoryBase + kPageCount)
            PrintHelper(kPages[action - kCategoryBase].contents, "FEH_JMP");  // CLICK LMB / RETURN - ENTER MENU
        else if (action == kReset)
            PrintHelper(kPages[g_page].rows.begin()[0] == &sPart ? kResetPartHelp : kResetHelp, nullptr);
        else
            PrintHelper(GameText("FEH_BPO"), nullptr);  // CLICK LMB / RETURN - BACK
    }

    // ---- hooks -----------------------------------------------------------------

    void __thiscall DrawStandardMenus(void* menu, bool drawTitle)
    {
        const bool ready = Ready();
        if (ready && g_open && CurrentScreen() != kAdvancedPage)
            Close();  // left some other way (menu closed, ...)

        g_drawStandardMenus(menu, drawTitle);

        if (!ready || CurrentScreen() != kAdvancedPage)
            return;
        if (g_open)
            DrawPage();
        else
            DrawLabel(g_table[kAdvancedPage].items[g_entryRow], CurrentItem() == g_entryRow, "WIDESCREEN SETTINGS");
    }

    void __thiscall ProcessMenuOptions(void* menu, int8_t leftRight, bool* back, bool accept)
    {
        if (g_table && CurrentScreen() == kAdvancedPage)
        {
            const int8_t action = g_table[kAdvancedPage].items[CurrentItem()].action;
            if (!g_open)
            {
                if (action == kOpen)
                {
                    if (accept)
                        OpenList();
                    return;
                }
            }
            else
            {
                if (action == kClose || action == kReset || (action >= kCategoryBase && action < kCategoryBase + kPageCount))
                {
                    if (accept)
                    {
                        if (action == kClose)
                            GoBack();
                        else if (action == kReset)
                            RestoreDefaults(g_page);
                        else
                            OpenPage(action - kCategoryBase);
                    }
                    return;
                }
                if (const Setting* s = RowSetting(CurrentItem()))
                {
                    // Left/right/wheel step; Enter or a click cycles toggles and lists.
                    const int dir = leftRight ? (leftRight > 0 ? 1 : -1) : (accept && s->kind != Kind::Slider ? 1 : 0);
                    Change(*s, dir);
                    return;
                }
            }
        }
        g_processMenuOptions(menu, leftRight, back, accept);
    }

    void __thiscall SwitchToNewScreen(void* menu, int8_t screen)
    {
        if (g_open)
        {
            if (screen == kGoBack && CurrentScreen() == kAdvancedPage)
            {
                GoBack();  // Esc: from a page to the list, from the list out to Advanced
                return;
            }
            Close();
        }
        g_switchToNewScreen(menu, screen);
    }
}

void InstallMenuSettings()
{
    static const uint8_t drawStd[] = { 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
    static const uint8_t processOptions[] = { 0x8B, 0x44, 0x24, 0x0C, 0x53 };
    static const uint8_t switchScreen[] = { 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0x5D, 0x01, 0x00, 0x00 };

    g_drawStandardMenus = reinterpret_cast<DrawStandardMenusFn>(
        mem::Detour("CMenuManager::DrawStandardMenus", 0x5794A0, drawStd, sizeof(drawStd), reinterpret_cast<void*>(&DrawStandardMenus)));
    if (!g_drawStandardMenus)
        return;
    g_processMenuOptions = reinterpret_cast<ProcessMenuOptionsFn>(
        mem::Detour("CMenuManager::ProcessMenuOptions", 0x576FE0, processOptions, sizeof(processOptions), reinterpret_cast<void*>(&ProcessMenuOptions)));
    g_switchToNewScreen = reinterpret_cast<SwitchToNewScreenFn>(
        mem::Detour("CMenuManager::SwitchToNewScreen", 0x573680, switchScreen, sizeof(switchScreen), reinterpret_cast<void*>(&SwitchToNewScreen)));
    if (!g_processMenuOptions || !g_switchToNewScreen)
        Log("menu: input hooks missing; Widescreen Settings may not respond");

    // push esi; mov esi, ecx; nop; nop (then a jump, in the retail exe)
    static const uint8_t helperText[] = { 0x56, 0x8B, 0xF1, 0x90, 0x90 };
    g_displayHelperText = reinterpret_cast<DisplayHelperTextFn>(
        mem::Detour("CMenuManager::DisplayHelperText", 0x57E240, helperText, sizeof(helperText), reinterpret_cast<void*>(&DisplayHelperText)));

    if (config::liveMenuPreview && g_processMenuOptions && g_switchToNewScreen)
        InstallLivePreview();
}
