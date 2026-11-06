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

    // Our actions: unused numbers (the game's go up to 65).
    enum Action : int8_t
    {
        kOpen = 100,
        kWidescreen, kFov, kHudSize, kRadarSize, kCrosshairSize, kRoundRadar, kHudArea, kMenuLayout, kBorders, kBorderSlide,
        kReset,
        kClose,
    };

    struct Row
    {
        Action action;
        const char* label;
        const char* help;  // shown at the bottom while the row is selected
    };
    constexpr Row kRows[] = {
        { kWidescreen, nullptr, "TURN THE WIDESCREEN FIX ON OR OFF - OFF IS THE ORIGINAL GAME" },  // label: the game's own WIDESCREEN
        { kFov, "FIELD OF VIEW", "HOW MUCH THE CAMERA SEES - CUTSCENES KEEP THEIR OWN" },
        { kHudSize, "HUD SIZE", "SIZE OF THE WHOLE HUD" },
        { kRadarSize, "RADAR SIZE", "SIZE OF THE RADAR ON TOP OF THE HUD SIZE" },
        { kCrosshairSize, "CROSSHAIR SIZE", "SIZE OF THE AIMING CROSSHAIR" },
        { kRoundRadar, "ROUND RADAR", "DRAW THE RADAR AS A CIRCLE - THE ORIGINAL IS A WIDE OVAL" },
        { kHudArea, "HUD AREA", "KEEP THE HUD INSIDE A NARROWER CENTRED AREA - FOR WIDE SCREENS" },
        { kMenuLayout, "MENU LAYOUT", "MENU TITLES AND PICTURES AT THE EDGES / CENTERED / STRETCHED" },
        { kBorders, "CUTSCENE BARS", "BLACK BARS IN CUTSCENES - ORIGINAL / NONE / ALSO AT THE SIDES" },
        { kBorderSlide, "BAR SLIDE", "CUTSCENE BARS SLIDE IN AND OUT INSTEAD OF APPEARING AT ONCE" },
    };
    constexpr const char* kResetHelp = "PUT EVERY WIDESCREEN SETTING BACK TO ITS DEFAULT";
    constexpr const char* kEntryHelp = "FIELD OF VIEW / HUD / RADAR / MENUS / CUTSCENE BARS";

    // Row positions on the 640x448 canvas. Every row gets an explicit Y: the
    // game's automatic 30-unit spacing would not fit 12 rows. The first row
    // stays below the top quarter, which the menu layout treats as the title
    // band (pushed to the screen edges).
    constexpr uint16_t kFirstRowX = 57, kFirstRowY = 116, kRowHeight = 24;
    uint16_t RowY(int i) { return static_cast<uint16_t>(kFirstRowY + i * kRowHeight); }
    constexpr int kRowCount = sizeof(kRows) / sizeof(*kRows);
    static_assert(kRowCount + 2 <= 12, "rows + RESTORE DEFAULTS + BACK must fit a page");

    // Slider ranges, in steps of 0.05 so 1.00 (the original) is always on a
    // step. FOV covers everything the ini accepts.
    constexpr float kFovMin = 0.50f, kFovMax = 2.00f;
    constexpr float kHudMin = 0.60f, kHudMax = 1.40f;
    constexpr float kPartMin = 0.50f, kPartMax = 2.00f;  // radar and crosshair
    constexpr float kSliderStep = 0.05f;

    Page* g_table = nullptr;
    int g_entryRow = -1;      // our row on the Advanced page
    bool g_open = false;      // the Advanced page currently shows our rows
    Page g_saved;             // its own rows while it does

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
            g_table = FindTable();
            if (g_table)
                AddEntryRow(g_table[kAdvancedPage]);
        }
        return g_table && g_entryRow >= 0;
    }

    void Open()
    {
        Page& page = g_table[kAdvancedPage];
        g_saved = page;
        std::memset(page.items, 0, sizeof(page.items));
        std::strncpy(page.title, "NWS_TTL", sizeof(page.title));
        page.startItem = 0;
        for (int i = 0; i < kRowCount; ++i)
            page.items[i] = MakeItem(kRows[i].action, "NWS_ROW", kTypeOption, kFirstRowX, RowY(i), kAlignLeft);
        // The game's own RESTORE DEFAULTS text (FET_DEF), centred like on its pages.
        page.items[kRowCount] = MakeItem(kReset, "FET_DEF", kTypeEnter, 320, static_cast<uint16_t>(RowY(kRowCount) + 6), kAlignCentre);
        // BACK keeps the game's text and sits below the rows.
        page.items[kRowCount + 1] = MakeItem(kClose, "FEDS_TB", kTypeEnter, 320, static_cast<uint16_t>(RowY(kRowCount + 1) + 6), kAlignCentre);
        g_open = true;
        CurrentItem() = 0;
    }

    void Close()
    {
        if (!g_open)
            return;
        g_table[kAdvancedPage] = g_saved;
        g_open = false;
    }

    // ---- settings ------------------------------------------------------------

    void Save(const char* section, const char* key, const char* value) { config::Write(section, key, value); }
    void SaveFloat(const char* section, const char* key, float v)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        Save(section, key, buf);
    }

    struct Slider
    {
        float* value;
        float lo, hi;
        const char* section;
        const char* key;
    };

    // The setting behind a slider row, or nullptr.
    const Slider* SliderFor(Action action)
    {
        static Slider fov, hud, radar, crosshair;
        fov = { &config::fovScale, kFovMin, kFovMax, "Camera", "FOV" };
        hud = { &config::hudScale, kHudMin, kHudMax, "HUD", "Scale" };
        radar = { &config::hudParts[static_cast<int>(config::HudPart::Radar)].scale, kPartMin, kPartMax, "HUD.Radar", "Scale" };
        crosshair = { &config::hudParts[static_cast<int>(config::HudPart::Crosshair)].scale, kPartMin, kPartMax, "HUD.Crosshair", "Scale" };
        switch (action)
        {
        case kFov: return &fov;
        case kHudSize: return &hud;
        case kRadarSize: return &radar;
        case kCrosshairSize: return &crosshair;
        default: return nullptr;
        }
    }

    float Step(float value, float lo, float hi, int dir)
    {
        const float n = std::round((value - lo) / kSliderStep) + dir;
        return std::clamp(lo + n * kSliderStep, lo, hi);
    }

    template <class E>
    E Cycle(E value, int count, int dir)
    {
        return static_cast<E>((static_cast<int>(value) + dir + count) % count);
    }

    // HUD AREA: the HUD kept inside a centred area of this aspect ratio. Only
    // ratios narrower than the screen do anything, so only those are offered.
    struct AreaRatio { float value; const char* label; const char* ini; };
    constexpr AreaRatio kAreas[] = {
        { 0.0f, "FULL", "0" },
        { 4.0f / 3.0f, "4 X 3", "4:3" },
        { 16.0f / 10.0f, "16 X 10", "16:10" },
        { 16.0f / 9.0f, "16 X 9", "16:9" },
        { 21.0f / 9.0f, "21 X 9", "21:9" },
    };
    constexpr int kAreaCount = sizeof(kAreas) / sizeof(*kAreas);

    bool AreaUsable(int i)
    {
        return kAreas[i].value == 0.0f || kAreas[i].value < Width() / Height() - 0.01f;
    }

    int AreaIndex(float value)  // -1 for a ratio set in the ini that is not listed
    {
        for (int i = 0; i < kAreaCount; ++i)
            if (std::fabs(kAreas[i].value - value) < 0.01f)
                return i;
        return -1;
    }

    void StepArea(int dir)
    {
        int i = AreaIndex(config::hudMaxAspect);
        if (i < 0)  // custom: continue from the nearest listed ratio below it
            for (i = 0; i + 1 < kAreaCount && kAreas[i + 1].value < config::hudMaxAspect; ++i) {}
        for (int n = 0; n < kAreaCount; ++n)
        {
            i = (i + dir + kAreaCount) % kAreaCount;
            if (AreaUsable(i))
                break;
        }
        config::hudMaxAspect = kAreas[i].value;
        config::Write("HUD", "Area", kAreas[i].ini);
    }

    // Rows that do nothing with the current settings are greyed out and locked.
    bool Available(Action action)
    {
        if (action != kWidescreen && action != kReset && !config::widescreen)
            return false;
        if (action == kBorderSlide)
            return config::borders != config::Borders::Off;
        if (action == kHudArea)
            return AreaUsable(1);  // anything narrower than the screen at all
        return true;
    }

    void Change(Action action, int dir)
    {
        if (!Available(action))
            return;
        switch (action)
        {
        case kWidescreen:
            config::widescreen = !config::widescreen;
            Save("General", "Widescreen", config::widescreen ? "1" : "0");
            break;
        case kFov:
        case kHudSize:
        case kRadarSize:
        case kCrosshairSize:
        {
            const Slider& sl = *SliderFor(action);
            *sl.value = Step(*sl.value, sl.lo, sl.hi, dir);
            SaveFloat(sl.section, sl.key, *sl.value);
            break;
        }
        case kRoundRadar:
            config::roundRadar = !config::roundRadar;
            Save("HUD.Radar", "Round", config::roundRadar ? "1" : "0");
            break;
        case kHudArea:
            StepArea(dir);
            break;
        case kMenuLayout:
        {
            static const char* const names[] = { "edges", "centered", "stretch" };
            config::menuLayout = Cycle(config::menuLayout, 3, dir);
            Save("Menus", "Layout", names[static_cast<int>(config::menuLayout)]);
            break;
        }
        case kBorders:
        {
            static const char* const names[] = { "vanilla", "off", "pillarbox" };
            config::borders = Cycle(config::borders, 3, dir);
            Save("Cutscenes", "Bars", names[static_cast<int>(config::borders)]);
            break;
        }
        case kBorderSlide:
            config::borderSlide = config::borderSlide > 0.0f ? 0.0f : 0.5f;
            SaveFloat("Cutscenes", "BarSlide", config::borderSlide);
            break;
        case kReset:
            config::widescreen = true;
            config::horPlus = true;
            config::fovScale = 1.0f;
            config::hudScale = 1.0f;
            *SliderFor(kRadarSize)->value = 1.0f;
            *SliderFor(kCrosshairSize)->value = 1.0f;
            config::roundRadar = true;
            config::hudMaxAspect = 0.0f;
            config::menuLayout = config::MenuLayout::Edges;
            config::borders = config::Borders::Vanilla;
            config::borderSlide = 0.0f;
            Save("General", "Widescreen", "1");
            Save("Camera", "HorPlus", "1");
            Save("Camera", "FOV", "1.0");
            Save("HUD", "Scale", "1.0");
            Save("HUD.Radar", "Scale", "1.0");
            Save("HUD.Crosshair", "Scale", "1.0");
            Save("HUD.Radar", "Round", "1");
            Save("HUD", "Area", "0");
            Save("Menus", "Layout", "edges");
            Save("Cutscenes", "Bars", "vanilla");
            Save("Cutscenes", "BarSlide", "0");
            // The game's own "DEFAULT SETTINGS RESTORED" helper text (FET_HRD).
            reinterpret_cast<void(__thiscall*)(void*, int)>(0x57CD10)(Menu(), 2);
            break;
        default:
            break;
        }
    }

    // Text for the right column, or nullptr for a slider (value in `slider`).
    const char* Value(Action action, float& slider)
    {
        auto onOff = [](bool on) { return GameText(on ? "FEM_ON" : "FEM_OFF"); };
        switch (action)
        {
        case kWidescreen: return onOff(config::widescreen);
        case kFov:
        case kHudSize:
        case kRadarSize:
        case kCrosshairSize:
        {
            const Slider& sl = *SliderFor(action);
            slider = (*sl.value - sl.lo) / (sl.hi - sl.lo);
            return nullptr;
        }
        case kRoundRadar: return onOff(config::roundRadar);
        case kHudArea:
        {
            const int i = AreaIndex(config::hudMaxAspect);
            return i >= 0 ? kAreas[i].label : "CUSTOM";
        }
        case kMenuLayout:
        {
            static const char* const names[] = { "EDGES", "CENTERED", "STRETCHED" };
            return names[static_cast<int>(config::menuLayout)];
        }
        case kBorders:
        {
            static const char* const names[] = { "ORIGINAL", "OFF", "PILLARBOX" };
            return names[static_cast<int>(config::borders)];
        }
        case kBorderSlide: return onOff(config::borderSlide > 0.0f);
        default: return "";
        }
    }

    // ---- drawing ---------------------------------------------------------------

    void RowFont(bool selected, int8_t orientation, int8_t edge, bool available = true)
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

    void SliderMouse(int row, const Item& it)
    {
        if (!MouseHeld())
            return;
        const float x0 = StretchX(kSliderX), length = StretchX(kSliderLength);
        const float mx = static_cast<float>(MouseX()), my = static_cast<float>(MouseY());
        if (my < StretchY(it.y - 4.0f) || my > StretchY(it.y + 24.0f) || mx < x0 - StretchX(6.0f) || mx > x0 + length + StretchX(6.0f))
            return;

        CurrentItem() = row;
        const Slider& sl = *SliderFor(kRows[row].action);
        const float t = std::clamp((mx - x0) / length, 0.0f, 1.0f);
        const float value = std::clamp(sl.lo + std::round(t * (sl.hi - sl.lo) / kSliderStep) * kSliderStep, sl.lo, sl.hi);
        if (std::fabs(*sl.value - value) < 0.001f)
            return;
        *sl.value = value;
        SaveFloat(sl.section, sl.key, value);
    }

    bool Live();

    void DrawPage()
    {
        Page& page = g_table[kAdvancedPage];

        // Nothing to see of the crosshair unless aiming: show it here.
        if (Live())
            DrawCrosshairPreview();

        font::Style(font::kGothic);
        font::Scale(StretchX(1.3f), StretchY(2.1f));
        font::Edge(1);
        font::Orientation(font::kLeft);
        font::Color(font::kSelected);
        font::DropColor(font::kBlack);
        font::Print(StretchX(40.0f), StretchY(28.0f), "Widescreen");

        for (int i = 0; i < kRowCount; ++i)
        {
            const Item& it = page.items[i];
            const bool selected = i == CurrentItem();
            const bool available = Available(kRows[i].action);
            DrawLabel(it, selected, kRows[i].label ? kRows[i].label : GameText("FED_WIS"), available);

            float slider = 0.0f;
            if (const char* text = Value(kRows[i].action, slider))
            {
                RowFont(selected, font::kRight, 1, available);
                font::Print(Width() - StretchX(40.0f), StretchY(it.y), text);
            }
            else
            {
                SliderMouse(i, it);
                Value(kRows[i].action, slider);  // after a possible change
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
        return config::liveMenuPreview && g_open && MenuActive() && CurrentScreen() == kAdvancedPage && InGame();
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

    void PrintHelper(const char* line1, const char* hintKey)
    {
        static char text[256];
        if (hintKey)
            std::snprintf(text, sizeof(text), "%s~n~%s", line1, GameText(hintKey));
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

        const int item = CurrentItem();
        if (!g_open)
            PrintHelper(kEntryHelp, "FEH_JMP");  // CLICK LMB / RETURN - ENTER MENU
        else if (item < kRowCount && !Available(kRows[item].action))
            PrintHelper(!config::widescreen ? "NOT USED WHILE WIDESCREEN IS OFF"
                        : kRows[item].action == kHudArea ? "YOUR SCREEN IS ALREADY 4 X 3" : "NOT USED WHILE CUTSCENE BARS ARE OFF", nullptr);
        else if (item < kRowCount)
            PrintHelper(kRows[item].help, "FET_MIG");  // LEFT / RIGHT / MOUSEWHEEL - ADJUST
        else if (item == kRowCount)
            PrintHelper(kResetHelp, nullptr);
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
        {
            DrawLabel(g_table[kAdvancedPage].items[g_entryRow], CurrentItem() == g_entryRow, "WIDESCREEN SETTINGS");
        }
    }

    void __thiscall ProcessMenuOptions(void* menu, int8_t leftRight, bool* back, bool accept)
    {
        if (g_table && CurrentScreen() == kAdvancedPage)
        {
            const int8_t action = g_table[kAdvancedPage].items[CurrentItem()].action;
            if (action == kOpen && !g_open)
            {
                if (accept)
                    Open();
                return;
            }
            if (g_open)
            {
                if (action == kClose)
                {
                    if (accept)
                    {
                        Close();
                        CurrentItem() = g_entryRow;
                    }
                    return;
                }
                if (action > kOpen && action < kClose)
                {
                    const bool slider = SliderFor(static_cast<Action>(action)) != nullptr;
                    if (action == kReset)
                    {
                        if (accept)
                            Change(kReset, 1);
                        return;
                    }
                    const int dir = leftRight ? (leftRight > 0 ? 1 : -1) : (accept && !slider ? 1 : 0);
                    if (dir)
                        Change(static_cast<Action>(action), dir);
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
            Close();
            if (screen == kGoBack && CurrentScreen() == kAdvancedPage)
            {
                CurrentItem() = g_entryRow;  // Esc: back to Advanced, on our row
                return;
            }
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
