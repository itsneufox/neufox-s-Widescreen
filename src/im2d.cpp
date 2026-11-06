#include "im2d.hpp"
#include "mem.hpp"
#include "game.hpp"
#include "scope.hpp"
#include "screen.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace im2d
{
    using game::Im2DVertex;
    using PrimitiveFn = int(__cdecl*)(int type, Im2DVertex* verts, int count);
    using IndexedFn = int(__cdecl*)(int type, Im2DVertex* verts, int count, uint16_t* indices, int indexCount);
    using TriangleFn = int(__cdecl*)(Im2DVertex* verts, int count, int a, int b, int c);

    static PrimitiveFn g_primitive;
    static IndexedFn g_indexed;
    static TriangleFn g_triangle;

    static std::vector<Im2DVertex> g_scratch;

    struct Bounds
    {
        float minX, maxX, minY, maxY;
    };

    static Bounds Measure(const Im2DVertex* verts, int count)
    {
        Bounds b{ verts[0].x, verts[0].x, verts[0].y, verts[0].y };
        for (int i = 1; i < count; ++i)
        {
            b.minX = std::min(b.minX, verts[i].x);
            b.maxX = std::max(b.maxX, verts[i].x);
            b.minY = std::min(b.minY, verts[i].y);
            b.maxY = std::max(b.maxY, verts[i].y);
        }
        return b;
    }

    static uint8_t* BoundRaster()
    {
        using RenderStateGetFn = int(__cdecl*)(int state, void* value);
        auto get = *reinterpret_cast<RenderStateGetFn*>(game::RwEngine() + game::kRenderStateGet);
        uint8_t* raster = nullptr;
        if (!get(game::kRenderStateTextureRaster, &raster))
            return nullptr;
        return raster;
    }

    // Aspect ratio of the texture being drawn, or 0 if there is none. Square
    // textures hold 4:3 artwork squeezed into a power-of-two size.
    static float TextureAspect()
    {
        uint8_t* raster = BoundRaster();
        if (!raster)
            return 0.0f;
        int w = *reinterpret_cast<int*>(raster + 0x0C), h = *reinterpret_cast<int*>(raster + 0x10);
        if (w <= 0 || h <= 0)
            return 0.0f;
        return w == h ? screen::kBaseAspect : static_cast<float>(w) / static_cast<float>(h);
    }

    // Maps the range [from0, from1] of each vertex's `field` onto [to0, to1].
    template <float Im2DVertex::*field>
    static void Remap(Im2DVertex* verts, int count, float from0, float from1, float to0, float to1)
    {
        if (from1 - from0 <= 0.0f)
            return;
        const float k = (to1 - to0) / (from1 - from0);
        for (int i = 0; i < count; ++i)
            verts[i].*field = to0 + (verts[i].*field - from0) * k;
    }

    static void Crop(Im2DVertex* verts, int count, float Im2DVertex::*uv, float keep)
    {
        float lo = verts[0].*uv, hi = lo;
        for (int i = 1; i < count; ++i)
        {
            lo = std::min(lo, verts[i].*uv);
            hi = std::max(hi, verts[i].*uv);
        }
        const float mid = 0.5f * (lo + hi);
        for (int i = 0; i < count; ++i)
            verts[i].*uv = mid + (verts[i].*uv - mid) * keep;
    }

    // Full-screen artwork keeps its own aspect ratio. Returns false to draw
    // the batch unchanged (untextured fades, or stretching wanted).
    static bool FitArtwork(Im2DVertex* out, int count, const Bounds& b, scope::Fit fit)
    {
        const float w = screen::Width(), h = screen::Height();
        const bool fullHeight = b.minY <= 0.5f && b.maxY >= h - 0.5f;
        const float art = fullHeight && fit != scope::Fit::Stretch ? TextureAspect() : 0.0f;
        if (art <= 0.0f)
            return false;

        const float screenAspect = w / h;
        if (art > screenAspect)
            Crop(out, count, &Im2DVertex::u, screenAspect / art);
        else if (fit == scope::Fit::Crop)
            Crop(out, count, &Im2DVertex::v, art / screenAspect);
        else
            Remap<&Im2DVertex::x>(out, count, b.minX, b.maxX, 0.5f * (w - h * art), 0.5f * (w + h * art));
        return true;
    }

    static void Track(scope::Frame* f, const Bounds& b)
    {
        if (!f->drew)
        {
            f->drew = true;
            f->minX = b.minX; f->maxX = b.maxX; f->minY = b.minY; f->maxY = b.maxY;
            return;
        }
        f->minX = std::min(f->minX, b.minX); f->maxX = std::max(f->maxX, b.maxX);
        f->minY = std::min(f->minY, b.minY); f->maxY = std::max(f->maxY, b.maxY);
    }

    // Returns the vertices to draw: the caller's own when nothing applies,
    // otherwise a transformed copy (callers reuse their buffers).
    static Im2DVertex* Transform(Im2DVertex* verts, int count)
    {
        scope::Frame* f = scope::Top();
        if (!f || count <= 0 || (f->xf.Identity() && f->xf.fit == scope::Fit::Stretch))
            return verts;
        const scope::Xform& xf = f->xf;
        const Bounds b = Measure(verts, count);

        g_scratch.assign(verts, verts + count);
        Im2DVertex* out = g_scratch.data();

        const bool fullWidth = b.minX <= 0.5f && b.maxX >= screen::Width() - 0.5f;
        if (fullWidth && (xf.flags & scope::kKeepFullWidth))
        {
            if (!FitArtwork(out, count, b, xf.fit))
                return verts;
        }
        else
        {
            scope::Xform el = xf;
            el.ax = scope::AnchorX(xf, b.minX, b.maxX, b.minY, b.maxY);
            // Masks such as the black frame around the menu map start at the
            // screen edge; narrowed, they would uncover what they hide.
            const float w = screen::Width();
            const bool pin = (xf.flags & scope::kPinScreenEdges) && !BoundRaster();
            for (int i = 0; i < count; ++i)
            {
                const float x = out[i].x;
                // Only corners exactly on an edge: the wide map frame's inner
                // edge can lie beyond the screen before the transform.
                const bool atEdge = pin && (std::fabs(x) <= 0.5f || std::fabs(x - w) <= 0.5f);
                out[i].x = atEdge ? x : el.X(x);
                out[i].y = el.Y(out[i].y);
            }
        }

        if (xf.flags & scope::kFillSides)
            Track(f, Measure(out, count));
        return out;
    }

    // Another mod can sit in the chain around these wrappers and draw
    // through RenderWare again; only the outermost call is transformed, so
    // nothing is corrected twice.
    static int g_inside = 0;
    static bool g_nestedLogged = false;

    template <class F>
    static int Guarded(Im2DVertex* verts, int count, F draw)
    {
        if (g_inside)
        {
            if (!g_nestedLogged)
            {
                g_nestedLogged = true;
                Log("note: 2D drawing re-entered through another mod; drawn once, untransformed");
            }
            return draw(verts);
        }
        ++g_inside;
        int r = draw(Transform(verts, count));
        --g_inside;
        return r;
    }

    static int __cdecl Primitive(int type, Im2DVertex* verts, int count)
    {
        return Guarded(verts, count, [&](Im2DVertex* v) { return g_primitive(type, v, count); });
    }

    static int __cdecl Indexed(int type, Im2DVertex* verts, int count, uint16_t* indices, int indexCount)
    {
        return Guarded(verts, count, [&](Im2DVertex* v) { return g_indexed(type, v, count, indices, indexCount); });
    }

    static int __cdecl Triangle(Im2DVertex* verts, int count, int a, int b, int c)
    {
        return Guarded(verts, count, [&](Im2DVertex* v) { return g_triangle(v, count, a, b, c); });
    }

    static const char* Owner(const void* fn)
    {
        static char name[MAX_PATH];
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<const char*>(fn), &mod) ||
            !GetModuleFileNameA(mod, name, MAX_PATH))
            return "?";
        const char* slash = std::strrchr(name, '\\');
        return slash ? slash + 1 : name;
    }

    // Hooks a RwEngineInstance entry once. Called on every scope entry so a
    // RenderWare device restart (which rebuilds the table) is picked up, but
    // never wraps a chain that already leads back here.
    template <class Fn>
    static void Wrap(const char* what, uintptr_t offset, Fn& original, Fn wrapper)
    {
        Fn& slot = *reinterpret_cast<Fn*>(game::RwEngine() + offset);
        if (slot == wrapper)
            return;
        if (original && slot != original)
        {
            static uint32_t logged = 0;
            const uint32_t bit = 1u << (offset / 4 % 32);
            if (!(logged & bit))
            {
                logged |= bit;
                Log("note: %s was hooked by %s after this mod; leaving it chained", what, Owner(reinterpret_cast<const void*>(slot)));
            }
            return;
        }
        if (!original)
            Log("%s: hooking (previous owner %s)", what, Owner(reinterpret_cast<const void*>(slot)));
        original = slot;
        slot = wrapper;
    }

    void Install()
    {
        if (!game::RwEngine())
            return;
        Wrap("Im2DRenderPrimitive", game::kIm2DRenderPrimitive, g_primitive, &Primitive);
        Wrap("Im2DRenderIndexedPrimitive", game::kIm2DRenderIndexedPrimitive, g_indexed, &Indexed);
        Wrap("Im2DRenderTriangle", game::kIm2DRenderTriangle, g_triangle, &Triangle);
    }
}
