#include "textdraw.hpp"

#include "features.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "screen.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// SA-MP draws textdraws through the game's own CFont::PrintString and
// CSprite2d::Draw, so they are recognised by the return address (samp.dll),
// with no offsets tied to one SA-MP version. Screens up to 16:9 are never changed:
// servers design for the 16:9 stretch.
//
// A panel is many textdraws (boxes, text, icons) and has to narrow as one piece.
// Each frame's textdraws are collected, and those that overlap form a group, used
// on the next frame. A whole group narrows around one anchor: its own centre, so
// it stays where the server put it, or the screen edge if it is out at one. So
// nothing inside a panel moves relative to the rest, whatever its alignment.
namespace textdraw
{
    namespace
    {
        constexpr float kReference = 16.0f / 9.0f;

        uintptr_t g_samp = 0, g_sampEnd = 0;

        struct Box
        {
            float left, right, top, bottom;
            bool textOnly = false;  // plain text with no box: its extent is only text
        };
        std::vector<Box> g_seen, g_groups;  // this frame's textdraws, last frame's groups
        uint32_t g_frame = 0;

        // Boxes and sprites join only what they really cover, because a panel's edge
        // can just touch something unrelated, or pieces of one strip: same row, same
        // height, end to end (a line made of segments). Plain text also joins text
        // that nearly touches it, as one line or as a stack: a logo can be several
        // words with small gaps, and a list can be many separate lines.
        bool Joined(const Box& a, const Box& b)
        {
            const bool coverX = a.left < b.right && b.left < a.right, coverY = a.top < b.bottom && b.top < a.bottom;
            if (coverX && coverY)
                return true;
            if (!a.textOnly || !b.textOnly)
            {
                const float unitX = screen::Width() / 640.0f, unitY = screen::Height() / 448.0f;
                return !a.textOnly && !b.textOnly && std::fabs(a.top - b.top) <= unitY && std::fabs(a.bottom - b.bottom) <= unitY &&
                       a.left <= b.right + 2.0f * unitX && b.left <= a.right + 2.0f * unitX;
            }
            const float gapX = 8.0f * screen::Width() / 640.0f, gapY = 2.0f * screen::Height() / 448.0f;
            return (coverY && a.left < b.right + gapX && b.left < a.right + gapX) ||
                   (coverX && a.top < b.bottom + gapY && b.top < a.bottom + gapY);
        }

        void BuildGroups()
        {
            g_groups.clear();
            for (Box box : g_seen)
            {
                // Swallow every group it touches; it grows, so look again from the start.
                for (size_t i = 0; i < g_groups.size();)
                {
                    const Box& g = g_groups[i];
                    if (!Joined(g, box))
                    {
                        ++i;
                        continue;
                    }
                    box.left = std::min(box.left, g.left);
                    box.right = std::max(box.right, g.right);
                    box.top = std::min(box.top, g.top);
                    box.bottom = std::max(box.bottom, g.bottom);
                    box.textOnly = box.textOnly && g.textOnly;
                    g_groups.erase(g_groups.begin() + static_cast<std::ptrdiff_t>(i));
                    i = 0;
                }
                g_groups.push_back(box);
            }
            g_seen.clear();
        }

        // Backgrounds and bars across the screen are not part of any panel.
        bool Spans(const Box& b) { return b.right - b.left >= 0.9f * screen::Width(); }

        Placement PlacementOf(const Box& box)
        {
            const float k = Narrowing();
            if (Spans(box))
                return { 0.0f, 1.0f };

            // The group it overlaps most. A click area can be another size than what
            // is drawn, so it need not lie fully inside, or even be centred on it.
            Box group = box;
            float best = -1.0f;
            for (const Box& g : g_groups)
            {
                const float w = std::min(box.right, g.right) - std::max(box.left, g.left);
                const float h = std::min(box.bottom, g.bottom) - std::max(box.top, g.top);
                if (w < 0.0f || h < 0.0f)
                    continue;
                // Flat areas still count: a little extra keeps their overlap above zero.
                const float area = (w + 1.0f) * (h + 1.0f);
                if (area > best)
                {
                    best = area;
                    group = g;
                }
            }
            if (Spans(group))
                return { 0.0f, 1.0f };  // pieces that add up to the whole width stay whole

            const float w = screen::Width();
            if (config::textdraws == config::Textdraws::Box)
                return { 0.5f * w, k };
            if (group.right <= 0.3f * w)
                return { 0.0f, k };
            if (group.left >= 0.7f * w)
                return { w, k };
            return { 0.5f * (group.left + group.right), k };
        }

        // SA-MP's hit test: GetCursorPos, ScreenToClient, then PtInRect against
        // the rectangle it stored at draw time, which is in unmapped coordinates.
        BOOL WINAPI HitTest(const RECT* rect, POINT point)
        {
            if (Narrowing() < 1.0f)
            {
                const Placement p = PlacementOf({ static_cast<float>(rect->left), static_cast<float>(rect->right),
                                                  static_cast<float>(rect->top), static_cast<float>(rect->bottom) });
                point.x = std::lround(p.ax + (static_cast<float>(point.x) - p.ax) / p.k);
            }
            return PtInRect(rect, point);
        }

        // Finds that one call by what follows it, the same in every version:
        //   test eax, eax; je ..; mov eax, [esp+..]; mov [eax+8], <id>
        // and what comes before: push ecx (y); push edx (x); push eax (rect).
        // Anything else is left alone: the arguments must be exactly PtInRect's.
        //   R3 and later, DL: call [PtInRect]
        //   R1:               push esi (junk the stub removes); call <stub>
        //   R2:               call <stub>; a junk byte the stub skips
        // Returns the bytes to replace, or 0 if this is not the call.
        size_t CallBefore(const uint8_t* tail, const uint8_t*& call)
        {
            const auto pushes = [](const uint8_t* end) { return end[-3] == 0x51 && end[-2] == 0x52 && end[-1] == 0x50; };
            const auto inSamp = [](const uint8_t* e8) {
                const uintptr_t target = reinterpret_cast<uintptr_t>(e8) + 5 + *reinterpret_cast<const int32_t*>(e8 + 1);
                return target >= g_samp && target < g_sampEnd;
            };

            const uint8_t* p = tail - 6;
            if (p[0] == 0xFF && p[1] == 0x15 && pushes(p))
            {
                const uintptr_t slot = *reinterpret_cast<const uint32_t*>(p + 2);
                if (slot < g_samp || slot + 4 > g_sampEnd ||
                    *reinterpret_cast<void* const*>(slot) != reinterpret_cast<const void*>(&PtInRect))
                    return 0;
                call = p;
                return 6;
            }
            if (p[0] == 0x56 && p[1] == 0xE8 && pushes(p) && inSamp(p + 1))  // R1
            {
                call = p;
                return 6;
            }
            if (p[0] == 0xE8 && pushes(p) && inSamp(p))  // R2
            {
                call = p;
                return 6;
            }
            return 0;
        }

        void InstallHitTest(const IMAGE_NT_HEADERS* nt)
        {
            const uint8_t* site = nullptr;
            size_t siteLength = 0;
            int matches = 0;

            const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
            {
                if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || section->Misc.VirtualSize < 32)
                    continue;
                const uint8_t* const start = reinterpret_cast<const uint8_t*>(g_samp + section->VirtualAddress);
                const uint8_t* const end = start + section->Misc.VirtualSize - 12;
                for (const uint8_t* p = start + 9; p < end; ++p)
                {
                    if (p[0] != 0x85 || p[1] != 0xC0 || p[2] != 0x74 || p[4] != 0x8B || p[5] != 0x44 || p[6] != 0x24 ||
                        p[8] != 0x66 || p[9] != 0x89 || p[11] != 0x08)
                        continue;
                    const uint8_t* call = nullptr;
                    if (const size_t length = CallBefore(p, call))
                    {
                        site = call;
                        siteLength = length;
                        ++matches;
                    }
                }
            }

            if (matches == 1)
                mem::WriteCall(reinterpret_cast<uintptr_t>(site), reinterpret_cast<const void*>(&HitTest), siteLength);
            else
                Log("textdraws: click test not found in this samp.dll (%d matches); clickable textdraws stay where they would be unscaled", matches);
        }

        void FindSamp()
        {
            const HMODULE mod = GetModuleHandleA("samp.dll");
            if (!mod)
                return;
            const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            g_samp = base;
            g_sampEnd = base + nt->OptionalHeader.SizeOfImage;
            InstallHitTest(nt);
        }

        using SpriteDrawFn = void(__thiscall*)(void* sprite, const game::Rect* rect, const game::RGBA* color);
        SpriteDrawFn g_spriteDraw;

        void __thiscall SpriteDraw(void* sprite, const game::Rect* rect, const game::RGBA* color)
        {
            // Also used by SA-MP to fill a 512x512 render target: not on screen.
            const bool renderTarget = rect->left == 0.0f && rect->top == 0.0f && rect->right == 512.0f && rect->bottom == 512.0f;
            if (renderTarget || !FromSamp(__builtin_return_address(0)))
                return g_spriteDraw(sprite, rect, color);
            const Placement p = Place(std::min(rect->left, rect->right), std::max(rect->left, rect->right),
                                      std::min(rect->top, rect->bottom), std::max(rect->top, rect->bottom), false);
            game::Rect mapped = *rect;
            mapped.left = p.X(rect->left);
            mapped.right = p.X(rect->right);
            g_spriteDraw(sprite, &mapped, color);
        }
    }

    float Narrowing()
    {
        if (!config::widescreen || config::textdraws == config::Textdraws::Off)
            return 1.0f;
        return std::min(1.0f, kReference / screen::Aspect());
    }

    bool FromSamp(const void* returnAddress)
    {
        if (Narrowing() >= 1.0f)
            return false;
        if (!g_samp)
            FindSamp();
        const uintptr_t at = reinterpret_cast<uintptr_t>(returnAddress);
        return g_samp && at >= g_samp && at < g_sampEnd;
    }

    Placement Place(float left, float right, float top, float bottom, bool textOnly)
    {
        // A frozen counter must not let the list grow without end.
        const uint32_t frame = *reinterpret_cast<const uint32_t*>(0xB7CB4C);  // CTimer::m_FrameCounter
        if (frame != g_frame || g_seen.size() >= 4096)
        {
            g_frame = frame;
            BuildGroups();
        }
        const Box box{ left, right, top, bottom, textOnly };
        if (!Spans(box))
            g_seen.push_back(box);
        return PlacementOf(box);
    }
}

void InstallTextdrawFix()
{
    using namespace textdraw;
    static const uint8_t spriteDraw[] = { 0x8B, 0x44, 0x24, 0x08, 0x56 };  // mov eax, [esp+8]; push esi
    g_spriteDraw = reinterpret_cast<SpriteDrawFn>(
        mem::Detour("CSprite2d::Draw", 0x728350, spriteDraw, sizeof(spriteDraw), reinterpret_cast<void*>(&SpriteDraw)));
}
