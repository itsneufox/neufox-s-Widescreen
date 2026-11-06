#include "config.hpp"
#include "features.hpp"
#include "mem.hpp"
#include "region.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#ifndef NWS_VERSION
#define NWS_VERSION "dev"
#endif

extern "C" const char g_defaultIni[];  // defaults.S

namespace
{
    // First start (only the .asi was copied): write the documented ini.
    // An existing ini is never touched.
    void WriteDefaultIni(const char* path)
    {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return;
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return;  // read-only folder: the built-in defaults still apply
        DWORD written;
        WriteFile(f, g_defaultIni, static_cast<DWORD>(std::strlen(g_defaultIni)), &written, nullptr);
        CloseHandle(f);
    }

    bool IsSupportedGame()
    {
        // 1.0 US or 1.0 EU (addresses are translated by region::FromUS).
        return region::Detect() != region::Exe::Unknown;
    }

    // ThirteenAG's fix corrects the same things, so with both everything is
    // corrected twice. Looked for in the game folder, scripts\ and modloader.
    char g_otherFix[MAX_PATH * 2];

    bool Exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

    bool FindOtherWidescreenFix(const char* gameDir)
    {
        static const char kAsi[] = "GTASA.WidescreenFix.asi";
        char* path = g_otherFix;
        const size_t size = sizeof(g_otherFix);

        for (const char* sub : { "", "scripts\\" })
        {
            std::snprintf(path, size, "%s%s%s", gameDir, sub, kAsi);
            if (Exists(path))
                return true;
        }

        char pattern[MAX_PATH * 2];
        std::snprintf(pattern, sizeof(pattern), "%smodloader\\*", gameDir);
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA(pattern, &fd);
        bool found = false;
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                // modloader skips folders whose name starts with '.'
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                    continue;
                std::snprintf(path, size, "%smodloader\\%s\\%s", gameDir, fd.cFileName, kAsi);
                found = Exists(path);
            } while (!found && FindNextFileA(find, &fd));
            FindClose(find);
        }
        if (found)
            return true;

        // Loaded from somewhere else (another ASI loader folder).
        if (HMODULE mod = GetModuleHandleA(kAsi))
        {
            if (!GetModuleFileNameA(mod, path, MAX_PATH))
                std::snprintf(path, size, "%s", kAsi);
            return true;
        }
        return false;
    }

    DWORD WINAPI WarnConflict(LPVOID)
    {
        char text[1024];
        std::snprintf(text, sizeof(text),
                      "Incompatible mod found!\n\nDelete ThirteenAG's Widescreen Fix and restart.\n\n%s",
                      g_otherFix);
        MessageBoxA(nullptr, text, "neufox's Widescreen", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        return 0;
    }

    void Init(HMODULE self)
    {
        char ini[MAX_PATH];
        DWORD n = GetModuleFileNameA(self, ini, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return;
        if (char* dot = std::strrchr(ini, '.'))
            std::strcpy(dot, ".ini");
        WriteDefaultIni(ini);
        config::Load(ini);

        if (!IsSupportedGame())
        {
            Log("gta_sa.exe is not version 1.0 (US or EU); nothing was patched");
            return;
        }

        char gameDir[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
        if (len && len < MAX_PATH)
        {
            if (char* slash = std::strrchr(gameDir, '\\'))
                slash[1] = '\0';
            if (FindOtherWidescreenFix(gameDir))
            {
                Log("ThirteenAG's fix is installed at %s; nothing was patched", g_otherFix);
                // Not from DllMain itself: the loader lock is held here.
                if (HANDLE t = CreateThread(nullptr, 0, &WarnConflict, nullptr, 0, nullptr))
                    CloseHandle(t);
                return;
            }
        }

        InstallCameraFixes();
        if (config::waterFix)
            InstallWaterFix();
        InstallCutsceneBorders();
        if (config::skipIntro)
            InstallSkipIntro();
        InstallFontHooks();
        InstallTextdrawFix();
        if (config::hud)
            InstallHudScopes();
        if (config::frontend)
            InstallFrontendFixes();
        Log("neufox's Widescreen %s installed on %s (aspect %s, Hor+ %d, HUD scale %.2f)", NWS_VERSION, region::Name(region::Detect()),
            config::forcedAspect > 0.0f ? "forced" : "auto", config::horPlus, config::hudScale);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        Init(instance);
    }
    return TRUE;
}
