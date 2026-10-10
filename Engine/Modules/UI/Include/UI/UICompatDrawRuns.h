#pragma once

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace UI
{

struct UIPrimitive;

// Fixed slot budget of the compat UI pipeline. Must match
// GE_UI_COMPAT_TEXTURE_SLOTS / GE_UI_COMPAT_GLYPH_SLOTS in
// Shaders/UI/ui_sdf_textures.glsl and the layout UITextureRegistry builds.
// Content textures, glyph curves and their paired bands total 16 sampled
// images in the fragment stage — core WebGPU's per-stage maximum, so the split
// between the two families is the only free choice. Measured on a populated
// editor frame (569 primitives): content peaks at 8/8 and glyphs at 2/4, i.e.
// icons are what closes runs and font pages have headroom to spare. Shifting
// slots from glyphs to content would buy a few draws back; at 9 runs per frame
// that is not worth spending the glyph margin on.
inline constexpr uint32_t kUiCompatTextureSlots = 8;
inline constexpr uint32_t kUiCompatGlyphSlots = 4;

// Draw-order word layout under the compat profile: the persistent primitive
// slot in the low bits, the run-local texture slot ordinal above it. Mirrors
// UI_DRAW_ORDER_SLOT_MASK / UI_DRAW_ORDER_TEX_SLOT_SHIFT in ui_sdf.vert.
inline constexpr uint32_t kUiDrawOrderSlotMask = 0x00FFFFFFu;
inline constexpr uint32_t kUiDrawOrderTexSlotShift = 24;

// One contiguous span of the draw order whose distinct textures fit the
// pipeline's fixed slots, drawn as a single instanced draw against a single
// bind group. Slot tables hold UITextureRegistry indices, not GPU handles —
// resolving those is the registry's job.
struct UICompatDrawRun
{
    uint32_t First = 0;
    uint32_t Count = 0;
    uint32_t TextureCount = 0;
    uint32_t GlyphCount = 0;
    uint32_t Textures[kUiCompatTextureSlots]{};
    uint32_t GlyphCurves[kUiCompatGlyphSlots]{};
    uint32_t GlyphBands[kUiCompatGlyphSlots]{};
};

// Partitions the UI draw order into per-texture runs for the compat profile,
// which has no binding arrays to index per primitive.
//
// This is a SCAN, not a sort. UI draw order is painter's order, so runs are
// consecutive spans and a primitive never moves: the partition only decides
// where one draw ends and the next begins. A run closes when a primitive needs
// a texture and every slot of its family is already spoken for.
class UICompatDrawRunBuilder
{
public:
    // Rebuilds Words() and Runs() from the frame's draw order. Every draw-order
    // entry produces exactly one word, so Words() is index-parallel to
    // drawOrder and can be uploaded in its place.
    void Build(const std::vector<UIPrimitive>& primitives,
               const std::vector<uint32_t>& drawOrder);

    const std::vector<uint32_t>& Words() const { return m_Words; }
    const std::vector<UICompatDrawRun>& Runs() const { return m_Runs; }

private:
    std::vector<uint32_t> m_Words;
    std::vector<UICompatDrawRun> m_Runs;
};

} // namespace UI
} // namespace GameEngine
