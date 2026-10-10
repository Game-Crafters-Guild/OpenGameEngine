#include "UI/UICompatDrawRuns.h"

#include "UI/UIPrimitive.h"

namespace GameEngine
{
namespace UI
{
namespace
{

// What a primitive needs from the run's slot tables.
enum class SlotFamily
{
    None,    // no sampled image at all (Rect, Line, Bezier, Triangle)
    Content, // one texture2D (Textured, colour-emoji Slug)
    Glyph    // a curve/band pair (Slug)
};

struct SlotRequest
{
    SlotFamily Family = SlotFamily::None;
    uint32_t Primary = 0;   // content texture, or Slug curve
    uint32_t Secondary = 0; // Slug band
};

SlotRequest ResolveSlotRequest(const UIPrimitive& prim)
{
    const uint32_t mode = prim.ModeAndFlags & kPrimModeMask;
    if (mode == static_cast<uint32_t>(PrimitiveMode::Textured))
        return {SlotFamily::Content, prim.TextureIndex, 0};
    if (mode == static_cast<uint32_t>(PrimitiveMode::Slug))
    {
        if ((prim.ModeAndFlags & kPrimColorGlyphBit) != 0u)
            return {SlotFamily::Content, prim.TextureIndex, 0};
        // Slug stores its curve texture in TextureIndex and its band texture in
        // GradColor0 (see MakeSlugPrimitive).
        return {SlotFamily::Glyph, prim.TextureIndex, prim.GradColor0};
    }
    return {};
}

// Finds an existing slot for the request, or claims a free one. Returns false
// when the family is full, which is the caller's signal to close the run.
bool AcquireSlot(UICompatDrawRun& run, const SlotRequest& request, uint32_t& outOrdinal)
{
    if (request.Family == SlotFamily::None)
    {
        outOrdinal = 0;
        return true;
    }

    if (request.Family == SlotFamily::Content)
    {
        for (uint32_t i = 0; i < run.TextureCount; ++i)
        {
            if (run.Textures[i] == request.Primary)
            {
                outOrdinal = i;
                return true;
            }
        }
        if (run.TextureCount >= kUiCompatTextureSlots)
            return false;
        run.Textures[run.TextureCount] = request.Primary;
        outOrdinal = run.TextureCount++;
        return true;
    }

    for (uint32_t i = 0; i < run.GlyphCount; ++i)
    {
        if (run.GlyphCurves[i] == request.Primary && run.GlyphBands[i] == request.Secondary)
        {
            outOrdinal = i;
            return true;
        }
    }
    if (run.GlyphCount >= kUiCompatGlyphSlots)
        return false;
    run.GlyphCurves[run.GlyphCount] = request.Primary;
    run.GlyphBands[run.GlyphCount] = request.Secondary;
    outOrdinal = run.GlyphCount++;
    return true;
}

} // namespace

void UICompatDrawRunBuilder::Build(const std::vector<UIPrimitive>& primitives,
                                   const std::vector<uint32_t>& drawOrder)
{
    m_Words.clear();
    m_Runs.clear();
    if (drawOrder.empty())
        return;

    m_Words.resize(drawOrder.size());

    UICompatDrawRun run{};
    run.First = 0;

    for (uint32_t i = 0; i < static_cast<uint32_t>(drawOrder.size()); ++i)
    {
        const uint32_t slot = drawOrder[i] & kUiDrawOrderSlotMask;
        // A draw-order entry pointing past the uploaded primitive span cannot be
        // classified; it also cannot render, so it stays in the run untextured
        // rather than splitting it.
        const SlotRequest request = slot < primitives.size()
                                        ? ResolveSlotRequest(primitives[slot])
                                        : SlotRequest{};

        uint32_t ordinal = 0;
        if (!AcquireSlot(run, request, ordinal))
        {
            run.Count = i - run.First;
            m_Runs.push_back(run);

            run = UICompatDrawRun{};
            run.First = i;
            // A fresh run always has room: one primitive needs at most one slot.
            AcquireSlot(run, request, ordinal);
        }

        m_Words[i] = slot | (ordinal << kUiDrawOrderTexSlotShift);
    }

    run.Count = static_cast<uint32_t>(drawOrder.size()) - run.First;
    m_Runs.push_back(run);
}

} // namespace UI
} // namespace GameEngine
