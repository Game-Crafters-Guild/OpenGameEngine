// SkinnedShadowInvalidationTests — a bone-palette content change must advance
// the world's shadow-caster content version.
//
// Every shadow family's retain/re-render decision keys on that scalar version:
// CascadeShadowCache (directional cascades and their glass-tint twins), the
// point/spot PointShadowAtlasPlanner, and the RT shadow mask. A character
// animating in place moves no instance data, so if the palette change does not
// reach the version, every cache input compares byte-identical and the caches
// serve a layer rasterized at an older pose — the mesh animates while its
// shadow holds still. Pinning the version at this seam closes the class for all
// three consumers at once rather than per-cache.

#include "Engine/Rendering/RenderServices.h"

#include <gtest/gtest.h>

using GameEngine::Engine::Renderer::RenderServices;

namespace
{

constexpr GameEngine::uint64 kWorld = 1;
constexpr GameEngine::uint64 kOtherWorld = 2;

TEST(SkinnedShadowInvalidation, PaletteContentChangeAdvancesCasterVersion)
{
    RenderServices rs;
    const uint64_t before = rs.ShadowCasterContentVersion(kWorld);

    rs.NotifySkinPaletteContentChanged(kWorld);

    EXPECT_GT(rs.ShadowCasterContentVersion(kWorld), before)
        << "a re-posed skinned caster must invalidate the shadow caches";
}

TEST(SkinnedShadowInvalidation, EveryPaletteChangeAdvancesTheVersion)
{
    RenderServices rs;

    // One advance per notifying frame: a cache that compared equal on any
    // animated frame would retain that frame's pose.
    uint64_t previous = rs.ShadowCasterContentVersion(kWorld);
    for (int frame = 0; frame < 4; ++frame)
    {
        rs.NotifySkinPaletteContentChanged(kWorld);
        const uint64_t current = rs.ShadowCasterContentVersion(kWorld);
        EXPECT_GT(current, previous) << "frame " << frame;
        previous = current;
    }
}

TEST(SkinnedShadowInvalidation, PaletteChangeIsScopedToItsWorld)
{
    RenderServices rs;
    const uint64_t ownBefore = rs.ShadowCasterContentVersion(kWorld);
    const uint64_t otherBefore = rs.ShadowCasterContentVersion(kOtherWorld);

    rs.NotifySkinPaletteContentChanged(kWorld);

    // Positive control first: without it, a notify that bumps nothing at all
    // satisfies the scoping assertion below and the test proves nothing.
    EXPECT_GT(rs.ShadowCasterContentVersion(kWorld), ownBefore)
        << "the notified world must advance";
    EXPECT_EQ(rs.ShadowCasterContentVersion(kOtherWorld), otherBefore)
        << "an animation in one world must not invalidate another world's shadows";
}

} // namespace
