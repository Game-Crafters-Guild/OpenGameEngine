#include "Placement/FenceEmission.h"

#include "Components/Spline/SplinePoolSelection.h"
#include "SplineLayout/SpanMitre.h"

namespace GameEngine::Editor
{
namespace
{

// The variant piece `ordinal` of a role draws, or null for its pool piece.
const FencePoolPiece* VariantOf(const FenceEmissionPools& pools, std::span<const uint32> drawn,
                                size_t ordinal)
{
    if (ordinal >= drawn.size() || drawn[ordinal] == SplineLayout::kNoMitreVariant)
        return nullptr;
    return &pools.Variants[drawn[ordinal]];
}

uint64 VariantSignature(const FencePoolPiece& variant)
{
    return kSignatureMitreVariant | variant.Renderer.meshGpuHandleId;
}

} // namespace

std::vector<FenceEmission> BuildFenceEmissions(const FenceLayoutResult& built,
                                               const FenceEmissionPools& pools)
{
    std::vector<FenceEmission> emissions;
    emissions.reserve(built.Stations.size() + built.Spans.size() + built.Crests.size());
    const uint32 activePosts = static_cast<uint32>(pools.Posts.size());
    if (activePosts > 0u)
    {
        for (const FenceStation& station : built.Stations)
        {
            const uint32 pick = Components::SplinePoolSelect(
                pools.Seed, Components::SplinePoolRole::Post, station.Index, activePosts);
            emissions.push_back({&station.Pose, &pools.Posts[pick], 1.0f, pools.PostAxis, pick,
                                 "Post", station.Index, nullptr,
                                 {FencePieceRole::Station, station.Index, 0u}});
        }
    }
    // A gate draws its gate-pool piece and signs under its own base, so a span
    // that becomes a gate is a new mesh even where the two pools share a slot.
    uint32 spanOrdinal = 0;
    uint32 gateOrdinal = 0;
    for (size_t k = 0; k < built.Spans.size(); ++k)
    {
        const FenceSpan& span = built.Spans[k];
        const FencePoolPiece* variant = VariantOf(pools, pools.SpanVariants, k);
        const std::span<const FencePoolPiece> pool = span.IsGate ? pools.Gates : pools.Spans;
        const std::span<const FencePieceBounds> bounds =
            span.IsGate ? pools.GateBounds : pools.SpanBounds;
        const uint32 poolSignature =
            (span.IsGate ? kSignatureGateBase : kSignatureSpanBase) + span.PoolSlot;
        emissions.push_back({&span.Pose, variant ? variant : &pool[span.PoolSlot],
                             span.LengthScale, bounds[span.PoolSlot].Axis(),
                             variant ? VariantSignature(*variant) : uint64{poolSignature},
                             span.IsGate ? "Gate" : "Span",
                             span.IsGate ? gateOrdinal++ : spanOrdinal++, &span,
                             {FencePieceRole::Span, span.Run, span.OrdinalInRun}});
    }
    // A crest on the pitch grid and every cap is whole (scale 1); a crest
    // registered to its span carries that span's scale, so it ends where its
    // wall ends.
    uint32 crestOrdinal = 0;
    uint32 capOrdinal = 0;
    for (size_t k = 0; k < built.Crests.size(); ++k)
    {
        const FenceCrest& crest = built.Crests[k];
        const bool isCap = crest.IsCap;
        const FencePoolPiece* variant = VariantOf(pools, pools.CrestVariants, k);
        const std::span<const FencePoolPiece> pool = isCap ? pools.Caps : pools.Crests;
        const std::span<const FencePieceBounds> bounds =
            isCap ? pools.CapBounds : pools.CrestBounds;
        const uint32 poolSignature =
            (isCap ? kSignatureCapBase : kSignatureCrestBase) + crest.PoolSlot;
        emissions.push_back({&crest.Pose, variant ? variant : &pool[crest.PoolSlot],
                             crest.LengthScale, bounds[crest.PoolSlot].Axis(),
                             variant ? VariantSignature(*variant) : uint64{poolSignature},
                             isCap ? "Cap" : "Crest", isCap ? capOrdinal++ : crestOrdinal++, nullptr,
                             {FencePieceRole::Crest, crest.Index, isCap ? 1u : 0u}});
    }
    return emissions;
}

} // namespace GameEngine::Editor
