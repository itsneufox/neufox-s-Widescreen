#pragma once
#include <cstdint>

// GTA San Andreas 1.0 US addresses. 1.0 EU only differs between 0x7466D0 and
// 0x857000 (see region.hpp); none of the variables or functions read
// here lie in that range. Patches go through mem::, which translates.
namespace game
{
    inline int& ScreenWidth() { return *reinterpret_cast<int*>(0xC17044); }   // RsGlobal.maximumWidth
    inline int& ScreenHeight() { return *reinterpret_cast<int*>(0xC17048); }  // RsGlobal.maximumHeight
    inline float& AspectRatio() { return *reinterpret_cast<float*>(0xC3EFA4); } // CDraw::ms_fAspectRatio
    inline float& Fov() { return *reinterpret_cast<float*>(0x8D5038); }         // CDraw::ms_fFOV

    inline float TimeStep() { return *reinterpret_cast<float*>(0xB7CB5C); }  // CTimer::ms_fTimeStep, 1 = 1/50 s

    // TheCamera
    inline bool BordersOn() { return *reinterpret_cast<bool*>(0xB6F065); }  // TheCamera.m_bWideScreenOn (cutscenes)
    inline float& CrosshairMultX() { return *reinterpret_cast<float*>(0xB6EC14); }
    inline float& CrosshairMultY() { return *reinterpret_cast<float*>(0xB6EC10); }
    inline uint16_t ActiveCamMode()
    {
        uint8_t active = *reinterpret_cast<uint8_t*>(0xB6F081);
        return *reinterpret_cast<uint16_t*>(0xB6F1A8 + active * 0x238);
    }

    // RwEngineInstance->dOpenDevice entry points.
    inline uint8_t* RwEngine() { return *reinterpret_cast<uint8_t**>(0xC97B24); }
    constexpr uintptr_t kRenderStateGet = 0x24;
    constexpr uintptr_t kIm2DRenderTriangle = 0x2C;
    constexpr uintptr_t kIm2DRenderPrimitive = 0x30;
    constexpr uintptr_t kIm2DRenderIndexedPrimitive = 0x34;

    constexpr int kRenderStateTextureRaster = 1;

    struct Im2DVertex
    {
        float x, y, z, rhw;
        uint32_t color;
        float u, v;
    };
    static_assert(sizeof(Im2DVertex) == 28, "RwIm2DVertex layout");

    struct Rect { float left, bottom, right, top; };  // CRect
    struct RGBA { uint8_t r, g, b, a; };               // CRGBA

    inline void DrawRect(const Rect& rect, const RGBA& color)  // CSprite2d::DrawRect
    {
        reinterpret_cast<void(__cdecl*)(const Rect&, const RGBA&)>(0x727B60)(rect, color);
    }

    // CFont render state (CFont::Details).
    namespace font
    {
        inline uint32_t& Color() { return *reinterpret_cast<uint32_t*>(0xC71A60); }  // RGBA, lowest byte first
        inline uint8_t& DropRed() { return *reinterpret_cast<uint8_t*>(0xC71A97); }
        inline uint8_t& DropGreen() { return *reinterpret_cast<uint8_t*>(0xC71A98); }
        inline uint8_t& DropBlue() { return *reinterpret_cast<uint8_t*>(0xC71A99); }
        inline uint8_t& DropAlpha() { return *reinterpret_cast<uint8_t*>(0xC71A9A); }
        inline int8_t& Shadow() { return *reinterpret_cast<int8_t*>(0xC71A96); }
        inline int8_t& Edge() { return *reinterpret_cast<int8_t*>(0xC71A9B); }      // SetEdge also sets the spacing at 0xC71A9C
        inline uint8_t& IsCopy() { return *reinterpret_cast<uint8_t*>(0xC71A7E); }  // this glyph run is a shadow/edge copy
        inline bool Centred() { return *reinterpret_cast<uint8_t*>(0xC71A79) != 0; }
        inline bool RightJustified() { return *reinterpret_cast<uint8_t*>(0xC71A7A) != 0; }
        // A box with no opacity draws nothing, but servers often leave one on.
        inline bool HasBox() { return *reinterpret_cast<uint8_t*>(0xC71A7B) != 0 && *reinterpret_cast<uint8_t*>(0xC71A87) != 0; }
        inline float Slant() { return *reinterpret_cast<float*>(0xC71A6C); }
        inline float& ScaleX() { return *reinterpret_cast<float*>(0xC71A64); }
        inline float& ScaleY() { return *reinterpret_cast<float*>(0xC71A68); }
        inline float& SlantRefX() { return *reinterpret_cast<float*>(0xC71A70); }
        inline float& SlantRefY() { return *reinterpret_cast<float*>(0xC71A74); }
        inline float& WrapX() { return *reinterpret_cast<float*>(0xC71A88); }
        inline float& CentreSize() { return *reinterpret_cast<float*>(0xC71A8C); }
        inline float& RightJustifyWrap() { return *reinterpret_cast<float*>(0xC71A90); }
    }

    // CMenuManager (FrontEndMenuManager at 0xBA6748): m_nMousePosWinX is
    // written by the window procedure on WM_MOUSEMOVE.
    constexpr uintptr_t kMenuMouseWinX = 0xBA6828;
}
