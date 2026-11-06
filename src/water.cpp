#include "config.hpp"
#include "features.hpp"
#include "mem.hpp"

#include <algorithm>
#include <cmath>

// CWorldScan::ScanWorld skips water blocks the view only clips at an edge,
// which with Hor+ are at the sides. This lists every block the view touches.
namespace
{
    struct Point { float x, y; };
    using CellFn = void(__cdecl*)(int x, int y);
    using ScanWorldFn = int(__cdecl*)(Point* points, int count, CellFn fn);

    constexpr uintptr_t kScanCall = 0x6E6E8C;  // in CWaterLevel's block scan
    constexpr uintptr_t kScanWorld = 0x72CAE0;

    double Cross(const Point& o, const Point& a, const Point& b)
    {
        return (double(a.x) - o.x) * (double(b.y) - o.y) - (double(a.y) - o.y) * (double(b.x) - o.x);
    }

    // Convex hull (counter-clockwise) of up to 5 points; returns its size.
    int Hull(Point* p, int n, Point* out)
    {
        std::sort(p, p + n, [](const Point& a, const Point& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            while (k >= 2 && Cross(out[k - 2], out[k - 1], p[i]) <= 0.0)
                --k;
            out[k++] = p[i];
        }
        for (int i = n - 2, lower = k + 1; i >= 0; --i)
        {
            while (k >= lower && Cross(out[k - 2], out[k - 1], p[i]) <= 0.0)
                --k;
            out[k++] = p[i];
        }
        return k - 1;  // last point repeats the first
    }

    // Separating axis test between the convex polygon and the unit square at
    // (cx, cy). The square's own axes are covered by the caller's bounds loop.
    bool Touches(const Point* poly, int n, int cx, int cy)
    {
        const Point corners[4] = { { float(cx), float(cy) }, { float(cx + 1), float(cy) },
                                   { float(cx + 1), float(cy + 1) }, { float(cx), float(cy + 1) } };
        for (int i = 0; i < n; ++i)
        {
            const Point& a = poly[i];
            const Point& b = poly[(i + 1) % n];
            // The polygon is counter-clockwise: it lies left of every edge.
            // The square is separated if all its corners are strictly right.
            bool allRight = true;
            for (const Point& c : corners)
                if (Cross(a, b, c) >= 0.0)
                {
                    allRight = false;
                    break;
                }
            if (allRight)
                return false;
        }
        return true;
    }

    ScanWorldFn ScanWorld() { return reinterpret_cast<ScanWorldFn>(mem::Address(kScanWorld)); }

    int __cdecl ScanWater(Point* points, int count, CellFn fn)
    {
        if (count != 5)
            return ScanWorld()(points, count, fn);

        Point in[5], hull[10];  // the hull builder uses up to 2n slots
        float minX = points[0].x, maxX = minX, minY = points[0].y, maxY = minY;
        for (int i = 0; i < 5; ++i)
        {
            in[i] = points[i];
            if (!std::isfinite(in[i].x) || !std::isfinite(in[i].y))
                return ScanWorld()(points, count, fn);
            minX = std::min(minX, in[i].x); maxX = std::max(maxX, in[i].x);
            minY = std::min(minY, in[i].y); maxY = std::max(maxY, in[i].y);
        }
        // Cell coordinates are small (12x12 grid plus the area around it);
        // anything huge means an unusual camera, left to the original.
        if (minX < -1000.0f || minY < -1000.0f || maxX > 1000.0f || maxY > 1000.0f)
            return ScanWorld()(points, count, fn);

        const int n = Hull(in, 5, hull);
        if (n < 3)
            return ScanWorld()(points, count, fn);

        for (int x = int(std::floor(minX)); x <= int(std::floor(maxX)); ++x)
            for (int y = int(std::floor(minY)); y <= int(std::floor(maxY)); ++y)
                if (Touches(hull, n, x, y))
                    fn(x, y);
        return 0;
    }
}

void InstallWaterFix()
{
    // call CWorldScan::ScanWorld (rel32 from 0x6E6E8C to 0x72CAE0)
    static const uint8_t call[] = { 0xE8, 0x4F, 0x5C, 0x04, 0x00 };
    if (mem::Matches(kScanCall, call, sizeof(call)))
        mem::WriteCall(kScanCall, reinterpret_cast<void*>(&ScanWater), sizeof(call));
    else
        Log("skip: water block scan at %08X is already patched", static_cast<unsigned>(kScanCall));
}
