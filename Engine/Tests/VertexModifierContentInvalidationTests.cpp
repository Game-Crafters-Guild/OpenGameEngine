// VertexModifierContentInvalidationTests — a submitted renderable whose
// material moves its own vertices must advance the world's per-frame content
// versions, whichever producer submitted it.
//
// A vertex modifier displaces the surface from the shared animation clock, so
// the rasterized depth differs from last frame's while every instance record
// stays byte-identical. Both depth-derived cache families key on the scalar
// versions below: the shadow caches (CascadeShadowCache, the point/spot atlas
// planner, the RT mask) and the idle-recompute-elision families (HZB/P2
// occlusion, DepthMinMax, ClusteredLightCull, SDSM).
//
// The per-view batch-key derivation is where every producer's records meet —
// the ECS extraction lane and the packages that build their own GPUScene
// instances alike — so these pin the term there rather than per producer:
// submit, derive with BuildWorldBatchKeys, then read the versions.

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WorldDrawTypes.h"

#include <gtest/gtest.h>

#include <span>

using GameEngine::Engine::Renderer::Material;
using GameEngine::Engine::Renderer::RenderServices;
using GameEngine::Engine::Renderer::WorldSubmissionRecord;
using GameEngine::Engine::Renderer::kSubmissionFlagCastShadows;
namespace Rendering = GameEngine::Rendering;

namespace
{

constexpr GameEngine::uint64 kWorld = 7;
constexpr GameEngine::uint64 kOtherWorld = 9;

Material MakeMaterial(Rendering::MaterialKeyword keywords)
{
    Material mat = Material::TestFactory::Create(GameEngine::GUID::Generate(), "Mat", 32u);
    Rendering::ShaderVariantKey key{};
    key.materialKeywords = keywords;
    Material::TestFactory::SetVariantKey(mat, key);
    return mat;
}

// A view the submission can be attributed to: the record names a view, and the
// world it belongs to is the registry's answer.
Rendering::ViewId MakeView(RenderServices& rs, GameEngine::uint64 worldId)
{
    const Rendering::ViewId viewId =
        rs.Views().AllocateView("Test.View", Rendering::CameraId{1u});
    rs.Views().SetViewWorldId(viewId, worldId);
    return viewId;
}

WorldSubmissionRecord MakeRecord(Rendering::ViewId viewId, const Material& mat, bool castsShadows)
{
    WorldSubmissionRecord rec{};
    rec.viewId = viewId;
    rec.material = &mat;
    rec.flags = castsShadows ? kSubmissionFlagCastShadows : 0u;
    return rec;
}

TEST(VertexModifierContentInvalidation, DeformingCasterAdvancesBothVersions)
{
    // Both keyword forms displace the surface in the vertex stage.
    for (const Rendering::MaterialKeyword keyword :
         {Rendering::MaterialKeyword::HasVertexMod, Rendering::MaterialKeyword::HasVertexOutputMod})
    {
        RenderServices rs;
        const Rendering::ViewId view = MakeView(rs, kWorld);
        const Material deformer = MakeMaterial(keyword);
        const WorldSubmissionRecord rec = MakeRecord(view, deformer, /*castsShadows=*/true);

        const uint64_t casterBefore = rs.ShadowCasterContentVersion(kWorld);
        const uint64_t renderBefore = rs.RenderContentVersion(kWorld);

        // A foliage scene reaches the frame as many submitted batches, and the
        // atlas planner's one-step attribution reads the size of the advance —
        // so the frame must produce exactly one.
        for (int i = 0; i < 5; ++i)
            rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
        rs.BuildWorldBatchKeys();

        EXPECT_EQ(rs.ShadowCasterContentVersion(kWorld), casterBefore + 1u)
            << "a deforming caster must invalidate the shadow caches, once for the frame";
        EXPECT_EQ(rs.RenderContentVersion(kWorld), renderBefore + 1u)
            << "a deforming renderable must invalidate the depth-derived elision families";

        // A late view (thumbnail, preview) derives its keys on its own; the
        // world has already advanced this frame and must not advance again.
        rs.BuildWorldBatchKeysForView(view);
        EXPECT_EQ(rs.ShadowCasterContentVersion(kWorld), casterBefore + 1u)
            << "one advance per world per frame, not one per derivation";
    }
}

TEST(VertexModifierContentInvalidation, NonCastingDeformerLeavesTheShadowCachesAlone)
{
    RenderServices rs;
    const Rendering::ViewId view = MakeView(rs, kWorld);
    const Material grass = MakeMaterial(Rendering::MaterialKeyword::HasVertexMod);
    const WorldSubmissionRecord rec = MakeRecord(view, grass, /*castsShadows=*/false);

    const uint64_t casterBefore = rs.ShadowCasterContentVersion(kWorld);
    const uint64_t renderBefore = rs.RenderContentVersion(kWorld);

    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
    rs.BuildWorldBatchKeys();

    // Positive control first: a submission that bumps nothing at all would
    // satisfy the shadow assertion below and prove nothing.
    EXPECT_GT(rs.RenderContentVersion(kWorld), renderBefore)
        << "wind grass still re-rasterizes its depth every frame";
    EXPECT_EQ(rs.ShadowCasterContentVersion(kWorld), casterBefore)
        << "a non-casting deformer must not force cached shadow maps to re-render";
}

TEST(VertexModifierContentInvalidation, RigidSubmissionAdvancesNeither)
{
    RenderServices rs;
    const Rendering::ViewId view = MakeView(rs, kWorld);
    const Material rigid = MakeMaterial(Rendering::MaterialKeyword::None);
    const Material wind = MakeMaterial(Rendering::MaterialKeyword::HasVertexMod);

    const uint64_t casterBefore = rs.ShadowCasterContentVersion(kWorld);
    const uint64_t renderBefore = rs.RenderContentVersion(kWorld);

    const WorldSubmissionRecord rigidRec = MakeRecord(view, rigid, /*castsShadows=*/true);
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rigidRec, 1u));
    rs.BuildWorldBatchKeys();

    EXPECT_EQ(rs.ShadowCasterContentVersion(kWorld), casterBefore)
        << "a static caster must not defeat the shadow caches";
    EXPECT_EQ(rs.RenderContentVersion(kWorld), renderBefore)
        << "a static renderable must not defeat the elision gates";

    // Positive control: the same seam does fire for a deforming material, so
    // the equalities above are the signal being silent rather than absent.
    const WorldSubmissionRecord windRec = MakeRecord(view, wind, /*castsShadows=*/true);
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&windRec, 1u));
    rs.BuildWorldBatchKeys();
    EXPECT_GT(rs.ShadowCasterContentVersion(kWorld), casterBefore);
}

TEST(VertexModifierContentInvalidation, DeformerIsScopedToItsWorld)
{
    RenderServices rs;
    const Rendering::ViewId view = MakeView(rs, kWorld);
    MakeView(rs, kOtherWorld);
    const Material wind = MakeMaterial(Rendering::MaterialKeyword::HasVertexMod);
    const WorldSubmissionRecord rec = MakeRecord(view, wind, /*castsShadows=*/true);

    const uint64_t ownBefore = rs.ShadowCasterContentVersion(kWorld);
    const uint64_t otherBefore = rs.ShadowCasterContentVersion(kOtherWorld);

    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
    rs.BuildWorldBatchKeys();

    EXPECT_GT(rs.ShadowCasterContentVersion(kWorld), ownBefore)
        << "the submitted world must advance";
    EXPECT_EQ(rs.ShadowCasterContentVersion(kOtherWorld), otherBefore)
        << "a swaying tree in one world must not invalidate another world's shadows";
}

} // namespace
