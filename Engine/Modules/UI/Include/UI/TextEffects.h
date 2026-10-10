#pragma once

#include <cstdint>

namespace GameEngine::UI {

// Text shadow, glow and outline of one text surface. The resolved style holds
// the authored values (ARGB colours, logical px); ResolveTextEffects turns them
// into packed RGBA8 colours and device px for emission. Both colour layouts keep
// alpha in the top byte, so the visibility tests read either. A transparent
// colour disables its effect, as does a shadow with no offset and no blur,
// which would paint exactly under the fill.
struct TextEffects
{
    uint32_t ShadowColor = 0;
    float ShadowOffsetX = 0.0f;
    float ShadowOffsetY = 0.0f;
    float ShadowBlur = 0.0f;
    uint32_t GlowColor = 0;
    float GlowRadius = 0.0f;
    uint32_t OutlineColor = 0;
    float OutlineWidth = 0.0f;

    [[nodiscard]] bool HasVisibleEffect() const
    {
        return HasVisibleShadow() || HasVisibleGlow() || HasVisibleOutline();
    }

    [[nodiscard]] bool HasVisibleShadow() const
    {
        return (ShadowColor & 0xff000000u) != 0
            && (ShadowOffsetX != 0.0f || ShadowOffsetY != 0.0f || ShadowBlur > 0.0f);
    }

    [[nodiscard]] bool HasVisibleGlow() const
    {
        return (GlowColor & 0xff000000u) != 0 && GlowRadius > 0.0f;
    }

    [[nodiscard]] bool HasVisibleOutline() const
    {
        return (OutlineColor & 0xff000000u) != 0 && OutlineWidth > 0.0f;
    }
};

} // namespace GameEngine::UI
