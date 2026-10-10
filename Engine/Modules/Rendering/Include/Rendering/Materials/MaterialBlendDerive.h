#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialBlend.h"

#include <optional>

namespace GameEngine::Rendering
{

// Resolved PSO blend/depth-write state derived from a material's alpha mode plus
// its optional explicit blend authoring. This is the single source of truth the
// pipeline builder consumes (InternBaseMaterialPipeline) and the unit tests
// exercise directly — no device needed.
//
// Equality is the material system's "did the fixed-function half of the PSO
// change" test on re-registration: comparing the derived state rather than the
// authored fields keeps the recompile exact, since an alpha mode can discard an
// authored field entirely.
struct DerivedBlendState
{
    bool BlendEnable = false;
    ColorBlendAttachmentState Attachment{};
    bool DepthWriteEnable = true;
    bool DepthTestEnable = true;

    friend bool operator==(const DerivedBlendState&, const DerivedBlendState&) = default;
};

inline BlendFactor ToDeviceBlendFactor(MaterialBlendFactor f)
{
    // The authoring enum mirrors the device enum value-for-value (MaterialBlend.h).
    // EVERY enumerator is pinned so a reordering or middle insertion on either
    // side fails to compile instead of silently remapping factors.
#define GE_BLEND_FACTOR_PIN(name) \
    static_assert(static_cast<int>(MaterialBlendFactor::name) == static_cast<int>(BlendFactor::name))
    GE_BLEND_FACTOR_PIN(Zero);
    GE_BLEND_FACTOR_PIN(One);
    GE_BLEND_FACTOR_PIN(SrcColor);
    GE_BLEND_FACTOR_PIN(OneMinusSrcColor);
    GE_BLEND_FACTOR_PIN(DstColor);
    GE_BLEND_FACTOR_PIN(OneMinusDstColor);
    GE_BLEND_FACTOR_PIN(SrcAlpha);
    GE_BLEND_FACTOR_PIN(OneMinusSrcAlpha);
    GE_BLEND_FACTOR_PIN(DstAlpha);
    GE_BLEND_FACTOR_PIN(OneMinusDstAlpha);
    GE_BLEND_FACTOR_PIN(ConstantColor);
    GE_BLEND_FACTOR_PIN(OneMinusConstantColor);
    GE_BLEND_FACTOR_PIN(ConstantAlpha);
    GE_BLEND_FACTOR_PIN(OneMinusConstantAlpha);
    GE_BLEND_FACTOR_PIN(AlphaSaturate);
#undef GE_BLEND_FACTOR_PIN
    return static_cast<BlendFactor>(f);
}

inline BlendOp ToDeviceBlendOp(MaterialBlendOp op)
{
#define GE_BLEND_OP_PIN(name) \
    static_assert(static_cast<int>(MaterialBlendOp::name) == static_cast<int>(BlendOp::name))
    GE_BLEND_OP_PIN(Add);
    GE_BLEND_OP_PIN(Subtract);
    GE_BLEND_OP_PIN(ReverseSubtract);
    GE_BLEND_OP_PIN(Min);
    GE_BLEND_OP_PIN(Max);
#undef GE_BLEND_OP_PIN
    return static_cast<BlendOp>(op);
}

// Derive the color-blend attachment and depth-write flag for a material.
//
// - Opaque / Mask: no blend, depth write on (unless overridden).
// - Blend: blend enabled from `blend` (or the historical hardwired equation when
//   `blend` is absent), depth write off (unless overridden).
// - `zWriteOverride`, when present, always wins — it is the authored depth-write
//   force for both directions (e.g. depth-writing foliage blend, or a depth-less
//   opaque decal).
// - `zTestOverride`, when present, wins over the historical always-on depth test.
//   Absent keeps DepthTestEnable true so existing materials stay byte-identical.
//
// With `blend`, `zWriteOverride` and `zTestOverride` all absent this reproduces
// the pre-T1 pipeline state byte-for-byte, which is what keeps existing materials
// identical.
inline DerivedBlendState DeriveMaterialBlendState(MaterialAlphaMode alphaMode,
                                                  const std::optional<MaterialBlendState>& blend,
                                                  std::optional<bool> zWriteOverride,
                                                  std::optional<bool> zTestOverride)
{
    DerivedBlendState out{};

    if (alphaMode == MaterialAlphaMode::Blend)
    {
        const MaterialBlendState bs = blend.value_or(MaterialBlendState{});
        out.BlendEnable = true;
        out.Attachment.blendEnable = true;
        out.Attachment.srcColorBlendFactor = ToDeviceBlendFactor(bs.SrcColorFactor);
        out.Attachment.dstColorBlendFactor = ToDeviceBlendFactor(bs.DstColorFactor);
        out.Attachment.colorBlendOp = ToDeviceBlendOp(bs.ColorOp);
        out.Attachment.srcAlphaBlendFactor = ToDeviceBlendFactor(bs.SrcAlphaFactor);
        out.Attachment.dstAlphaBlendFactor = ToDeviceBlendFactor(bs.DstAlphaFactor);
        out.Attachment.alphaBlendOp = ToDeviceBlendOp(bs.AlphaOp);
        out.DepthWriteEnable = false;
    }
    else
    {
        out.DepthWriteEnable = true;
    }

    if (zWriteOverride)
        out.DepthWriteEnable = *zWriteOverride;
    if (zTestOverride)
        out.DepthTestEnable = *zTestOverride;

    return out;
}

} // namespace GameEngine::Rendering
