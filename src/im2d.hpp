#pragma once

// Wraps the RenderWare Im2D entry points every 2D draw goes through
// (sprites, rectangles, radar, glyph batches) and applies the active scope's
// transform to the vertices.
namespace im2d
{
    // Idempotent; called whenever a scope opens, since the device table only
    // exists once RenderWare is running.
    void Install();
}
