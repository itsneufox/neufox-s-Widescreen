#include "config.hpp"
#include "mem.hpp"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <string>
#include <cstdio>

namespace config
{
    bool widescreen = true;
    float forcedAspect = 0.0f;
    bool horPlus = true;
    float fovScale = 1.0f;
    bool proportionalSprites = true;
    bool waterFix = true;
    bool wideMap = true;
    bool liveMenuPreview = true;

    bool hud = true;
    float hudScale = 1.0f, hudWidth = 1.0f, hudHeight = 1.0f;
    float hudMaxAspect = 0.0f;
    bool scriptDrawing = false;
    HudPartSettings hudParts[static_cast<int>(HudPart::Count)];
    bool roundRadar = true;

    bool frontend = true;
    MenuLayout menuLayout = MenuLayout::Edges;
    float menuMaxAspect = 0.0f;
    Fit menuBackground = Fit::Crop;
    Fit loadingScreens = Fit::Pillarbox;
    Fit videos = Fit::Pillarbox;

    Borders borders = Borders::Vanilla;
    float borderSize = 1.0f;
    float borderSlide = 0.0f;

    bool skipIntro = false;

    char logPath[260] = "";

    static char g_ini[MAX_PATH];
    static bool g_liveReload = true;
    static FILETIME g_iniTime;
    static DWORD g_lastPoll;

    static const char* const kPartSections[] = {
        "HUD.Radar", "HUD.PlayerInfo", "HUD.Wanted", "HUD.Timers", "HUD.ZoneNames", "HUD.MissionTitle",
        "HUD.HelpText", "HUD.Subtitles", "HUD.Messages", "HUD.RadioName", "HUD.Crosshair",
    };
    static_assert(sizeof(kPartSections) / sizeof(*kPartSections) == static_cast<int>(HudPart::Count));

    static void ReadString(const char* section, const char* key, const char* def, char* out, DWORD size)
    {
        GetPrivateProfileStringA(section, key, def, out, size, g_ini);
        // Allow trailing "; comment" on the same line.
        if (char* c = std::strchr(out, ';'))
            *c = '\0';
        for (size_t n = std::strlen(out); n && (out[n - 1] == ' ' || out[n - 1] == '\t'); --n)
            out[n - 1] = '\0';
    }

    static bool ReadBool(const char* section, const char* key, bool def)
    {
        char buf[16];
        ReadString(section, key, def ? "1" : "0", buf, sizeof(buf));
        return std::atoi(buf) != 0;
    }

    static float ReadFloat(const char* section, const char* key, float def, float lo, float hi)
    {
        char buf[32];
        ReadString(section, key, "", buf, sizeof(buf));
        if (!buf[0])
            return def;
        return std::clamp(static_cast<float>(std::atof(buf)), lo, hi);
    }

    template <class E, size_t N>
    static E ReadEnum(const char* section, const char* key, E def, const char* const (&names)[N])
    {
        char buf[16];
        ReadString(section, key, "", buf, sizeof(buf));
        for (size_t i = 0; i < N; ++i)
            if (!_stricmp(buf, names[i]))
                return static_cast<E>(i);
        return def;
    }

    static Fit ReadFit(const char* section, const char* key, Fit def)
    {
        static const char* const names[] = { "stretch", "crop", "pillarbox" };
        return ReadEnum(section, key, def, names);
    }

    // "auto"/"0", "16:9", "21:9" or a plain ratio such as "2.37".
    static float ReadAspect(const char* section, const char* key)
    {
        char s[32];
        ReadString(section, key, "auto", s, sizeof(s));
        if (!*s || !_stricmp(s, "auto"))
            return 0.0f;
        const char* colon = std::strchr(s, ':');
        float w = static_cast<float>(std::atof(s));
        float h = colon ? static_cast<float>(std::atof(colon + 1)) : 1.0f;
        return (w > 0.0f && h > 0.0f) ? std::clamp(w / h, 1.0f, 8.0f) : 0.0f;
    }

    static void ReadLive()
    {
        widescreen = ReadBool("General", "Widescreen", true);
        horPlus = ReadBool("Camera", "HorPlus", true);
        forcedAspect = ReadAspect("General", "AspectRatio");
        fovScale = ReadFloat("Camera", "FOV", 1.0f, 0.5f, 2.0f);

        hudScale = ReadFloat("HUD", "Scale", 1.0f, 0.25f, 4.0f);
        hudWidth = ReadFloat("HUD", "Width", 1.0f, 0.25f, 4.0f);
        hudHeight = ReadFloat("HUD", "Height", 1.0f, 0.25f, 4.0f);
        hudMaxAspect = ReadAspect("HUD", "Area");
        for (int i = 0; i < static_cast<int>(HudPart::Count); ++i)
        {
            HudPartSettings& p = hudParts[i];
            p.scale = ReadFloat(kPartSections[i], "Scale", 1.0f, 0.1f, 8.0f);
            p.width = ReadFloat(kPartSections[i], "Width", 1.0f, 0.1f, 8.0f);
            p.offsetX = ReadFloat(kPartSections[i], "OffsetX", 0.0f, -4000.0f, 4000.0f);
            p.offsetY = ReadFloat(kPartSections[i], "OffsetY", 0.0f, -4000.0f, 4000.0f);
        }
        roundRadar = ReadBool("HUD.Radar", "Round", true);

        static const char* const layouts[] = { "edges", "centered", "stretch" };
        menuLayout = ReadEnum("Menus", "Layout", MenuLayout::Edges, layouts);
        menuMaxAspect = ReadAspect("Menus", "Area");
        menuBackground = ReadFit("Menus", "Backgrounds", Fit::Crop);
        loadingScreens = ReadFit("Menus", "LoadingScreens", Fit::Pillarbox);
        videos = ReadFit("Menus", "Videos", Fit::Pillarbox) == Fit::Stretch ? Fit::Stretch : Fit::Pillarbox;

        static const char* const borderModes[] = { "vanilla", "off", "pillarbox" };
        borders = ReadEnum("Cutscenes", "Bars", Borders::Vanilla, borderModes);
        borderSize = ReadFloat("Cutscenes", "BarSize", 1.0f, 0.0f, 2.0f);
        borderSlide = ReadFloat("Cutscenes", "BarSlide", 0.0f, 0.0f, 5.0f);
    }

    static bool IniTime(FILETIME& out)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &data))
            return false;
        out = data.ftLastWriteTime;
        return true;
    }

    void Load(const char* iniPath)
    {
        std::strncpy(g_ini, iniPath, sizeof(g_ini) - 1);

        proportionalSprites = ReadBool("Camera", "RoundCoronas", true);
        waterFix = ReadBool("Camera", "WaterFix", true);
        hud = ReadBool("HUD", "Enable", true);
        scriptDrawing = ReadBool("HUD", "ScriptDrawing", false);
        frontend = ReadBool("Menus", "Enable", true);
        wideMap = ReadBool("Menus", "WideMap", true);
        liveMenuPreview = ReadBool("Menus", "LivePreview", true);
        skipIntro = ReadBool("Startup", "SkipIntro", false);
        g_liveReload = ReadBool("Debug", "LiveReload", true);

        if (ReadBool("Debug", "Log", false))
        {
            std::strncpy(logPath, iniPath, sizeof(logPath) - 1);
            if (char* dot = std::strrchr(logPath, '.'))
                std::strcpy(dot, ".log");
        }

        ReadLive();
        IniTime(g_iniTime);
    }

    // Replaces `key = value` in [section] keeping the file's own layout
    // (spacing, comments, line endings). False if the key is not there.
    static bool RewriteLine(const char* section, const char* key, const char* value)
    {
        FILE* f = std::fopen(g_ini, "rb");
        if (!f)
            return false;
        std::string text;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
            text.append(buf, n);
        std::fclose(f);

        auto trim = [](std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
            return v;
        };
        auto same = [](std::string_view a, std::string_view b) {
            return a.size() == b.size() && !_strnicmp(a.data(), b.data(), a.size());
        };

        bool inSection = false;
        for (size_t pos = 0; pos < text.size();)
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            const std::string_view line = trim(std::string_view(text).substr(pos, end - pos));
            if (!line.empty() && line.front() == '[')
            {
                const size_t close = line.find(']');
                inSection = close != std::string_view::npos && same(trim(line.substr(1, close - 1)), section);
            }
            else if (inSection && !line.empty() && line.front() != ';')
            {
                const size_t eq = line.find('=');
                if (eq != std::string_view::npos && same(trim(line.substr(0, eq)), key))
                {
                    // Keep everything up to the value and any trailing "; comment".
                    const size_t lineStart = pos + (line.data() - (text.data() + pos));
                    const size_t valueStart = text.find('=', lineStart) + 1;
                    size_t valueEnd = text.find(';', valueStart);
                    size_t lineEnd = end;
                    if (lineEnd > valueStart && text[lineEnd - 1] == '\r')
                        --lineEnd;
                    if (valueEnd == std::string::npos || valueEnd > lineEnd)
                        valueEnd = lineEnd;
                    else
                        while (valueEnd > valueStart && text[valueEnd - 1] == ' ')
                            --valueEnd;  // keep the spacing before the comment
                    text.replace(valueStart, valueEnd - valueStart, std::string(" ") + value);

                    f = std::fopen(g_ini, "wb");
                    if (!f)
                        return false;
                    std::fwrite(text.data(), 1, text.size(), f);
                    std::fclose(f);
                    return true;
                }
            }
            pos = end + 1;
        }
        return false;
    }

    void Write(const char* section, const char* key, const char* value)
    {
        if (!g_ini[0])
            return;
        if (!RewriteLine(section, key, value))
            WritePrivateProfileStringA(section, key, value, g_ini);  // key missing: let Windows add it
        IniTime(g_iniTime);  // our own change: nothing to reload
    }

    void Poll()
    {
        if (!g_liveReload)
            return;
        DWORD now = GetTickCount();
        if (now - g_lastPoll < 1000)
            return;
        g_lastPoll = now;

        FILETIME t;
        if (!IniTime(t) || CompareFileTime(&t, &g_iniTime) == 0)
            return;
        g_iniTime = t;
        ReadLive();
        Log("settings reloaded");
    }
}
