// A fence's piece entities survive a rebuild by what they are, not by where they fall in the
// emission list. The controller that calls ReconcileFencePieces needs live RenderServices; the
// reconciliation itself needs only a World and the emissions, so it is pinned here.

#include <gtest/gtest.h>

#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplineFenceSpanPiece.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Placement/FencePieceReuse.h"

#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::FenceEmission;
using Editor::FencePieceEntity;
using Editor::FencePieceIdentity;
using Editor::FencePieceRole;
using Editor::FencePoolPiece;
using Editor::FenceSpan;
using Editor::PieceAxis;
using Editor::TilePose;

namespace
{

// A fence of stations, spans and crest pieces as a rebuild emits it. Spans are addressed by run
// and ordinal; a gate is a span of the gate pool.
struct Fixture
{
    FencePoolPiece Wall;
    FencePoolPiece Gate;
    FencePoolPiece Post;
    FencePoolPiece Crest;
    std::vector<TilePose> Poses;
    std::vector<FenceSpan> Spans;
    std::vector<FenceEmission> Emissions;

    Fixture()
    {
        Wall.Renderer.meshGpuHandleId = 11u;
        Gate.Renderer.meshGpuHandleId = 22u;
        Post.Renderer.meshGpuHandleId = 33u;
        Crest.Renderer.meshGpuHandleId = 44u;
        Poses.resize(64);
        Spans.reserve(64);
    }

    void AddPost(uint32 index)
    {
        Emissions.push_back({&Poses[Emissions.size()], &Post, 1.0f, PieceAxis::Z, 7u, "Post", index, nullptr,
                             {FencePieceRole::Station, index, 0u}});
    }

    void AddSpan(uint32 run, uint32 ordinal, uint32 label, bool gate = false)
    {
        FenceSpan span;
        span.Run = run;
        span.OrdinalInRun = ordinal;
        span.IsGate = gate;
        Spans.push_back(span);
        Emissions.push_back({&Poses[Emissions.size()], gate ? &Gate : &Wall, 1.0f, PieceAxis::X,
                             gate ? 4000u : 1000u, gate ? "Gate" : "Span", label, &Spans.back(),
                             {FencePieceRole::Span, run, ordinal}});
    }

    void AddCrest(uint32 cell, bool cap, uint32 label)
    {
        Emissions.push_back({&Poses[Emissions.size()], &Crest, 1.0f, PieceAxis::X, cap ? 3000u : 2000u,
                             cap ? "Cap" : "Crest", label, nullptr,
                             {FencePieceRole::Crest, cell, cap ? 1u : 0u}});
    }
};

const Mathematics::Matrix4x4& Identity()
{
    static const Mathematics::Matrix4x4 identity{};
    return identity;
}

bool Reconcile(ECS::World& world, ECS::EntityHandle parent, const Fixture& fixture,
               std::vector<FencePieceEntity>& pieces)
{
    const bool changed = Editor::ReconcileFencePieces(world, parent, Identity(), fixture.Emissions, pieces);
    world.ProcessCommands();
    return changed;
}

ECS::EntityHandle EntityOf(const std::vector<FencePieceEntity>& pieces, FencePieceRole role, uint32 primary,
                           uint32 secondary)
{
    for (const FencePieceEntity& piece : pieces)
    {
        if (piece.Identity == FencePieceIdentity{role, primary, secondary})
            return piece.Entity;
    }
    return {};
}

std::string NameOf(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name ? std::string(name->View()) : std::string();
}

// Two runs: posts 0..2, spans (0,0) (0,1) (1,0) (1,1), one crest per span.
Fixture TwoRuns()
{
    Fixture f;
    for (uint32 i = 0; i < 3; ++i)
        f.AddPost(i);
    f.AddSpan(0, 0, 0);
    f.AddSpan(0, 1, 1);
    f.AddSpan(1, 0, 2);
    f.AddSpan(1, 1, 3);
    for (uint32 i = 0; i < 4; ++i)
        f.AddCrest(i, false, i);
    return f;
}

} // namespace

TEST(FencePieceReuse, AnIdenticalRebuildKeepsEveryEntityAndSpawnsNothing)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.CreateEntity();
    const Fixture fixture = TwoRuns();
    std::vector<FencePieceEntity> pieces;
    EXPECT_TRUE(Reconcile(world, parent, fixture, pieces));
    ASSERT_EQ(pieces.size(), fixture.Emissions.size());
    const std::vector<FencePieceEntity> first = pieces;
    EXPECT_FALSE(Reconcile(world, parent, fixture, pieces));
    ASSERT_EQ(pieces.size(), first.size());
    for (size_t i = 0; i < pieces.size(); ++i)
        EXPECT_EQ(pieces[i].Entity, first[i].Entity) << i;
}

// A wall made a gate keeps its entity: same span, new mesh, new label, and the address its
// inspector reads says it is a gate now.
TEST(FencePieceReuse, AWallMadeAGateKeepsItsEntityAndTakesTheGatesMeshAndName)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.CreateEntity();
    std::vector<FencePieceEntity> pieces;
    Reconcile(world, parent, TwoRuns(), pieces);
    const ECS::EntityHandle picked = EntityOf(pieces, FencePieceRole::Span, 1u, 0u);
    ASSERT_TRUE(picked.IsValid());
    EXPECT_EQ(NameOf(world, picked), "Span 2");

    Fixture gated;
    for (uint32 i = 0; i < 3; ++i)
        gated.AddPost(i);
    gated.AddSpan(0, 0, 0);
    gated.AddSpan(0, 1, 1);
    gated.AddSpan(1, 0, 0, true);
    gated.AddSpan(1, 1, 2);
    for (uint32 i = 0; i < 4; ++i)
        if (i != 2u)
            gated.AddCrest(i, false, i < 2u ? i : i - 1u);
    Reconcile(world, parent, gated, pieces);

    EXPECT_EQ(EntityOf(pieces, FencePieceRole::Span, 1u, 0u), picked);
    ASSERT_TRUE(world.IsValid(picked));
    EXPECT_EQ(world.GetComponent<Components::MeshRenderer>(picked)->meshGpuHandleId, 22u);
    EXPECT_EQ(NameOf(world, picked), "Gate 0");
    const auto* address = world.GetComponent<Components::SplineFenceSpanPiece>(picked);
    ASSERT_NE(address, nullptr);
    EXPECT_TRUE(address->IsGate);
    EXPECT_EQ(address->Run, 1u);
    EXPECT_EQ(address->OrdinalInRun, 0u);
}

// An opening removes one span from the list, and everything after it moves up one place. Each
// surviving piece keeps its own entity, and the removed span's entity is destroyed rather than
// handed to its neighbour.
TEST(FencePieceReuse, AnOpeningDestroysItsSpanAndLeavesEveryOtherPieceItsEntity)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.CreateEntity();
    std::vector<FencePieceEntity> pieces;
    Reconcile(world, parent, TwoRuns(), pieces);
    const std::vector<FencePieceEntity> before = pieces;
    const ECS::EntityHandle opened = EntityOf(before, FencePieceRole::Span, 1u, 0u);
    const ECS::EntityHandle neighbour = EntityOf(before, FencePieceRole::Span, 1u, 1u);

    Fixture open;
    for (uint32 i = 0; i < 3; ++i)
        open.AddPost(i);
    open.AddSpan(0, 0, 0);
    open.AddSpan(0, 1, 1);
    open.AddSpan(1, 1, 2);
    for (uint32 i = 0; i < 4; ++i)
        if (i != 2u)
            open.AddCrest(i, false, i < 2u ? i : i - 1u);
    EXPECT_TRUE(Reconcile(world, parent, open, pieces));

    EXPECT_FALSE(world.IsValid(opened));
    EXPECT_EQ(EntityOf(pieces, FencePieceRole::Span, 1u, 1u), neighbour);
    const auto* address = world.GetComponent<Components::SplineFenceSpanPiece>(neighbour);
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->Run, 1u);
    EXPECT_EQ(address->OrdinalInRun, 1u) << "the neighbour took the opened span's address";
    for (const FencePieceEntity& piece : pieces)
    {
        const ECS::EntityHandle same =
            EntityOf(before, piece.Identity.Role, piece.Identity.Primary, piece.Identity.Secondary);
        EXPECT_EQ(piece.Entity, same) << "a surviving piece changed entity";
    }
}

// A run that gains or loses spans keeps the entities of the spans it still has and spawns or
// destroys only the difference; only span pieces carry a span address.
TEST(FencePieceReuse, AGrowingOrShrinkingListSpawnsAndDestroysOnlyTheDifference)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.CreateEntity();
    std::vector<FencePieceEntity> pieces;
    Reconcile(world, parent, TwoRuns(), pieces);
    const std::vector<FencePieceEntity> before = pieces;

    Fixture grown = TwoRuns();
    grown.AddSpan(1, 2, 4);
    grown.AddCrest(4, false, 4);
    EXPECT_TRUE(Reconcile(world, parent, grown, pieces));
    ASSERT_EQ(pieces.size(), before.size() + 2u);
    for (const FencePieceEntity& old : before)
        EXPECT_EQ(EntityOf(pieces, old.Identity.Role, old.Identity.Primary, old.Identity.Secondary), old.Entity);

    const ECS::EntityHandle added = EntityOf(pieces, FencePieceRole::Span, 1u, 2u);
    EXPECT_TRUE(Reconcile(world, parent, TwoRuns(), pieces));
    EXPECT_FALSE(world.IsValid(added));
    EXPECT_EQ(pieces.size(), before.size());

    for (const FencePieceEntity& piece : pieces)
    {
        const bool isSpan = piece.Identity.Role == FencePieceRole::Span;
        EXPECT_EQ(world.GetComponent<Components::SplineFenceSpanPiece>(piece.Entity) != nullptr, isSpan);
    }
}

// Two caps can close the two sides of one cell; both are kept, matched in order.
TEST(FencePieceReuse, PiecesSharingAnIdentityAreEachKept)
{
    ECS::World world;
    const ECS::EntityHandle parent = world.CreateEntity();
    Fixture caps;
    caps.AddCrest(5, true, 0);
    caps.AddCrest(5, true, 1);
    std::vector<FencePieceEntity> pieces;
    Reconcile(world, parent, caps, pieces);
    ASSERT_EQ(pieces.size(), 2u);
    ASSERT_NE(pieces[0].Entity, pieces[1].Entity);
    const std::vector<FencePieceEntity> before = pieces;
    EXPECT_FALSE(Reconcile(world, parent, caps, pieces));
    EXPECT_EQ(pieces[0].Entity, before[0].Entity);
    EXPECT_EQ(pieces[1].Entity, before[1].Entity);
}
