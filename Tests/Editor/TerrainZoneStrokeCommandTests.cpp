#include <gtest/gtest.h>

#include "SceneView/TerrainZoneStrokeCommand.h"

#include "EditorChangeNotifications.h"
#include "AssetCore/GUID.h"
#include "Components/Name.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"

#include <cstring>

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
using GameEngine::Editor::TerrainZoneStrokeCommand;
using GameEngine::Editor::ZoneStrokeState;
using GameEngine::Editor::EditorChangeNotifications;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::TerrainECS::TerrainZonePayload;
using GameEngine::TerrainECS::ZonePayloadFormat;

TerrainZonePayload MakePayload(uint32 dim, float32 fill)
{
    TerrainZonePayload p;
    p.Allocate(ZonePayloadFormat::SculptOffsetR32F, dim, dim);
    std::fill(p.Offsets.begin(), p.Offsets.end(), fill);
    return p;
}

// Build a live sculpt-zone entity + resident payload, and capture it as the
// stroke's "after" state (mirrors what CaptureZoneState does in the tool).
ZoneStrokeState MakeZone(World& world, TerrainService& svc, ECS::EntityHandle e,
                         const GUID& guid, float32 extent, float32 posX, const TerrainZonePayload& payload)
{
    Components::Transform xf{};
    xf.matrix[12] = posX;
    Components::WorldTransform wxf{};
    std::memcpy(wxf.matrix, xf.matrix, sizeof(wxf.matrix));
    Components::Name nm{};
    std::strncpy(nm.value, "SculptZone", sizeof(nm.value) - 1);
    Components::TerrainSculptZone z{};
    z.ExtentX = extent;
    z.ExtentZ = extent;
    z.PayloadRef.Set(guid);

    world.AddComponentImmediate(e, xf);
    world.AddComponentImmediate(e, wxf);
    world.AddComponentImmediate(e, nm);
    world.AddComponentImmediate(e, z);
    svc.SetZonePayload(guid, payload);

    ZoneStrokeState s;
    s.IsPaint = false;
    s.Sculpt = z;
    s.Transform = xf;
    s.WorldTransform = wxf;
    s.Name = nm;
    s.Payload = payload;
    return s;
}

class TerrainZoneStrokeCommandTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        m_World.Clear();
        m_Token = m_Notifications.SubscribeWorldStructureChanged(
            [this](const EditorChangeNotifications::WorldStructureChangedEvent&) { ++m_StructureNotifications; });
    }
    void TearDown() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    World m_World;
    EditorChangeNotifications m_Notifications;
    EditorChangeNotifications::SubscriptionToken m_Token{};
    int m_StructureNotifications = 0;
};

// C1(1): an auto-created stroke's undo/redo must PRESERVE the entity handle
// (not free-list-reuse a new one), or stacked commands capturing it break.
TEST_F(TerrainZoneStrokeCommandTests, CreatedStrokePreservesHandleAcrossUndoRedo)
{
    auto& svc = TerrainService::Get();
    const GUID guid = GUID::Generate();
    ECS::EntityHandle e = m_World.CreateEntity();
    ZoneStrokeState after = MakeZone(m_World, svc, e, guid, 16.0f, 0.0f, MakePayload(17, 3.0f));

    TerrainZoneStrokeCommand cmd(&m_World, &m_Notifications, &svc, e, guid, /*created*/ true,
                                 ZoneStrokeState{}, after);

    const std::uint32_t originalId = e.id;
    ASSERT_TRUE(m_World.IsValid(e));

    cmd.Undo();
    EXPECT_FALSE(m_World.IsValid(e)) << "created-stroke undo must remove the zone entity";
    EXPECT_EQ(svc.GetZonePayload(guid), nullptr) << "undo must evict the payload";

    cmd.Redo();
    EXPECT_TRUE(m_World.IsValid(e)) << "redo must revive the SAME handle";
    EXPECT_EQ(e.id, originalId) << "handle id must be preserved across undo/redo";
    ASSERT_NE(m_World.GetComponent<Components::TerrainSculptZone>(e), nullptr);
    EXPECT_FLOAT_EQ(m_World.GetComponent<Components::TerrainSculptZone>(e)->ExtentX, 16.0f);
    ASSERT_NE(svc.GetZonePayload(guid), nullptr);

    // Structural notification must fire on BOTH paths (event-driven hierarchy).
    EXPECT_EQ(m_StructureNotifications, 2);
}

// C1(1) sibling: a handle captured by a later command still resolves after the
// zone's create-command undo/redo cycle (would break with a re-minted handle).
TEST_F(TerrainZoneStrokeCommandTests, StackedHandleSurvivesCreateUndoRedo)
{
    auto& svc = TerrainService::Get();
    const GUID guid = GUID::Generate();
    ECS::EntityHandle e = m_World.CreateEntity();
    ZoneStrokeState after = MakeZone(m_World, svc, e, guid, 16.0f, 0.0f, MakePayload(17, 3.0f));
    TerrainZoneStrokeCommand cmd(&m_World, &m_Notifications, &svc, e, guid, true, ZoneStrokeState{}, after);

    // Another subsystem captures the handle now.
    ECS::EntityHandle captured = e;

    cmd.Undo();
    cmd.Redo();

    EXPECT_TRUE(m_World.IsValid(captured));
    EXPECT_EQ(captured.id, e.id);
}

// C1(2) + M3: a stroke on an EXISTING zone that auto-grew must revert the
// component extent + transform + payload ATOMICALLY (not payload-only), and
// notify both ways.
TEST_F(TerrainZoneStrokeCommandTests, ExistingStrokeRevertsComponentTransformPayloadAtomically)
{
    auto& svc = TerrainService::Get();
    const GUID guid = GUID::Generate();
    ECS::EntityHandle e = m_World.CreateEntity();

    // Pre-stroke: extent 16 at x=-10, small payload filled 1.0.
    ZoneStrokeState before = MakeZone(m_World, svc, e, guid, 16.0f, -10.0f, MakePayload(17, 1.0f));

    // Post-stroke (auto-grown): extent 32 at x=+5, larger payload filled 4.0.
    ZoneStrokeState after = MakeZone(m_World, svc, e, guid, 32.0f, 5.0f, MakePayload(33, 4.0f));
    const std::uint64_t afterVersion = svc.GetZonePayload(guid)->DataVersion;

    TerrainZoneStrokeCommand cmd(&m_World, &m_Notifications, &svc, e, guid, /*created*/ false,
                                 before, after);

    cmd.Undo();
    ASSERT_NE(m_World.GetComponent<Components::TerrainSculptZone>(e), nullptr);
    EXPECT_FLOAT_EQ(m_World.GetComponent<Components::TerrainSculptZone>(e)->ExtentX, 16.0f);
    EXPECT_FLOAT_EQ(m_World.GetComponent<Components::Transform>(e)->matrix[12], -10.0f);
    ASSERT_NE(svc.GetZonePayload(guid), nullptr);
    EXPECT_EQ(svc.GetZonePayload(guid)->Width, 17u);
    EXPECT_FLOAT_EQ(svc.GetZonePayload(guid)->Offsets[0], 1.0f);
    EXPECT_GT(svc.GetZonePayload(guid)->DataVersion, afterVersion) << "restore bumps version monotonically";

    cmd.Redo();
    EXPECT_FLOAT_EQ(m_World.GetComponent<Components::TerrainSculptZone>(e)->ExtentX, 32.0f);
    EXPECT_FLOAT_EQ(m_World.GetComponent<Components::Transform>(e)->matrix[12], 5.0f);
    EXPECT_EQ(svc.GetZonePayload(guid)->Width, 33u);
    EXPECT_FLOAT_EQ(svc.GetZonePayload(guid)->Offsets[0], 4.0f);

    EXPECT_EQ(m_StructureNotifications, 2);
}
} // namespace
