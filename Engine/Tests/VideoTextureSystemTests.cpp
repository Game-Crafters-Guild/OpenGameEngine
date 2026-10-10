// Lifetime tests for VideoTextureSystem: what happens to the MATERIAL when a
// video source goes away.
//
// The override lands on the material asset, not the entity, so every way an
// entity can stop driving a video — component removed, path cleared or
// mistyped, entity disabled — has to hand the slot back. A system that only
// releases on component removal leaves a shared material stranded on the last
// decoded frame for the rest of the session.

#include <gtest/gtest.h>

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/Components.h" // ECS::Disabled
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Video/VideoTextureSystem.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Video/VideoPlayer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

#include "TestDeviceHelper.h"
#include "StagedTestPaths.h"

namespace
{
constexpr float kTickSeconds = 1.0f / 30.0f;
// A first tick loads and auto-starts, but the first frame is produced on the
// player's own decode thread, so binding is not reachable by spinning ticks
// alone — the ticks have to leave real time for a frame to arrive. Bounded so a
// broken bind still fails the test instead of spinning.
constexpr int kMaxTicksToBind = 60;
constexpr auto kTickSpacing = std::chrono::milliseconds(5);
// How long a decoded frame is waited for before a tick that must upload one.
constexpr auto kFrameArrivalWait = std::chrono::milliseconds(120);

std::filesystem::path StagedSampleVideo()
{
    return TestPaths::StagedRoot() / "Apps" / "Editor" / "Assets" / "Sample" / "logo.mp4";
}
} // namespace

class VideoTextureSystemTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        m_rs = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_rs->Initialize(m_device.get()));
        m_system = std::make_unique<Video::VideoTextureSystem>(m_device.get(), m_rs.get());
    }

    void TearDown() override
    {
        m_system.reset();
        if (m_rs)
            m_rs->Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    // Skips (loudly, by name) when this build has no decoder — the FFmpeg
    // backend is selected at configure time and the null backend cannot load
    // anything. Never silently passes: a missing fixture is a failure, not a skip.
    //
    // NEITHER outcome stops the caller on its own: GTEST_SKIP and a failed ASSERT_*
    // both return only from the frame they appear in, and IsSkipped() is false for
    // the assertion. Callers must therefore follow this with
    // `if (IsSkipped() || HasFatalFailure()) return;` — without the second half a
    // missing fixture leaves the test running with an empty video path, where every
    // later assertion measures nothing.
    void RequireDecodableSample(std::string& outVideoPath)
    {
        const std::filesystem::path video = StagedSampleVideo();
        ASSERT_TRUE(std::filesystem::exists(video))
            << "staged fixture missing: " << video.string()
            << " (StageTestAssets should have copied it)";
        Video::VideoPlayer probe;
        if (!probe.Load(video.string()))
        {
            GTEST_SKIP() << "video decode unavailable in this build (null VideoPlayer backend); "
                            "fixture present at " << video.string();
        }
        outVideoPath = video.string();
    }

    Material* MakeMaterial(const GUID& guid)
    {
        MaterialDocument doc{};
        doc.materialName = "VideoTextureTest";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        return m_rs->Materials().RegisterMaterialFromDocument(guid, doc);
    }

    // Entity with a MeshRenderer pointing at `matGuid` and a video component.
    ECS::EntityHandle MakeVideoEntity(ECS::World& world, const GUID& matGuid,
                                      const std::string& videoPath)
    {
        ECS::EntityHandle e = world.CreateEntity();
        Components::MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        world.AddComponentImmediate(e, mr);

        Components::VideoTextureComponent vt{};
        std::snprintf(vt.videoPath, sizeof(vt.videoPath), "%s", videoPath.c_str());
        world.AddComponentImmediate(e, vt);
        return e;
    }

    // Ticks until the material slot carries a runtime texture. Returns the tick
    // count, or -1 if it never bound.
    int TickUntilBound(ECS::World& world, Material* mat, StringId slot)
    {
        for (int i = 1; i <= kMaxTicksToBind; ++i)
        {
            m_system->Update(world, kTickSeconds);
            if (mat->GetTexture(slot).IsValid())
                return i;
            std::this_thread::sleep_for(kTickSpacing);
        }
        return -1;
    }

    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<RenderServices> m_rs;
    std::unique_ptr<Video::VideoTextureSystem> m_system;
};

// B1. Clearing videoPath (or mistyping it) unloads the player. The entry is
// still matched by the query, so it keeps the tick stamp that the reconcile
// pass uses — nothing else will ever release it. The unload transition itself
// has to restore the slot, or the material shows the last decoded frame for the
// rest of the session and the GPU texture is never freed.
TEST_F(VideoTextureSystemTest, ClearedPathRestoresTheMaterialSlot)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuid = GUID::Generate();
    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex(slot);

    ECS::World world;
    ECS::EntityHandle e = MakeVideoEntity(world, matGuid, video);

    ASSERT_GT(TickUntilBound(world, mat, slot), 0) << "video never bound to the material";
    ASSERT_NE(mat->GetBindlessTextureIndex(slot), defaultIdx);

    // The inspector edit: clear the path.
    auto* comp = world.GetComponentForWrite<Components::VideoTextureComponent>(e);
    ASSERT_NE(comp, nullptr);
    comp->videoPath[0] = '\0';

    m_system->Update(world, kTickSeconds);

    EXPECT_FALSE(mat->GetTexture(slot).IsValid())
        << "the material is still holding the last decoded frame";
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);

    // Idempotent: further ticks neither re-bind nor re-open the dead source.
    m_system->Update(world, kTickSeconds);
    EXPECT_FALSE(mat->GetTexture(slot).IsValid());
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);

    m_rs->Textures().FlushPendingUploads();
}

TEST_F(VideoTextureSystemTest, UnloadablePathRestoresTheMaterialSlot)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuid = GUID::Generate();
    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex(slot);

    ECS::World world;
    ECS::EntityHandle e = MakeVideoEntity(world, matGuid, video);
    ASSERT_GT(TickUntilBound(world, mat, slot), 0);

    // A typo, not an empty string: the load path runs and fails.
    auto* comp = world.GetComponentForWrite<Components::VideoTextureComponent>(e);
    ASSERT_NE(comp, nullptr);
    std::snprintf(comp->videoPath, sizeof(comp->videoPath), "%s.typo", video.c_str());

    m_system->Update(world, kTickSeconds);

    EXPECT_FALSE(mat->GetTexture(slot).IsValid());
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);

    m_rs->Textures().FlushPendingUploads();
}

// A disabled entity must cost nothing: the query must not reach it at all. The
// probe is a component write only the visited path performs — diverging
// videoPath makes the system reload and clear `playing`, so an untouched
// `playing` proves the entity was never visited.
TEST_F(VideoTextureSystemTest, DisabledEntityIsNotVisited)
{
    const GUID matGuid = GUID::Generate();
    ASSERT_NE(MakeMaterial(matGuid), nullptr);

    ECS::World world;
    // No real source needed: this is about the query, not about decoding.
    ECS::EntityHandle e = MakeVideoEntity(world, matGuid, "does-not-exist.mp4");
    m_system->Update(world, kTickSeconds);

    world.AddComponentImmediate<ECS::Disabled>(e, ECS::Disabled{});
    {
        auto* comp = world.GetComponentForWrite<Components::VideoTextureComponent>(e);
        ASSERT_NE(comp, nullptr);
        comp->playing = true;
        std::snprintf(comp->videoPath, sizeof(comp->videoPath), "other-path.mp4");
    }

    m_system->Update(world, kTickSeconds);

    const auto* after = world.GetComponent<Components::VideoTextureComponent>(e);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->playing)
        << "the system reached a disabled entity (it reloaded the diverged path)";
}

// B2. A disabled entity is not in the world: it must stop decoding AND give the
// shared material back (the slot returns to its bindless default), because every
// other entity using that material is rendering whatever this one last bound.
//
// The re-enable path, which that release is what makes fragile. `playing`
// records that the auto-start already fired, and its lifetime is the ENTRY's:
// the disable released the entry, but the flag survives on the component. A
// fresh entry that trusts it never calls Play(), so its decode thread publishes
// nothing and the re-enabled entity decodes nothing ever again — the material
// sits on its authored texture with no error anywhere.
//
// Observed at the only instant that is falsifiable: whether it BINDS again. The
// flag itself is not a witness — the reset and the auto-start that consumes it
// both happen inside one Update, so `playing` reads true either way afterwards.
TEST_F(VideoTextureSystemTest, ReEnabledEntityResumesPlayback)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuid = GUID::Generate();
    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);
    const uint32_t defaultIdx = mat->GetBindlessTextureIndex(slot);

    ECS::World world;
    ECS::EntityHandle e = MakeVideoEntity(world, matGuid, video);
    ASSERT_GT(TickUntilBound(world, mat, slot), 0);
    ASSERT_TRUE(world.GetComponent<Components::VideoTextureComponent>(e)->playing);

    world.AddComponentImmediate<ECS::Disabled>(e, ECS::Disabled{});
    m_system->Update(world, kTickSeconds);
    ASSERT_FALSE(mat->GetTexture(slot).IsValid())
        << "a disabled entity is still dictating what this shared material renders";
    EXPECT_EQ(mat->GetBindlessTextureIndex(slot), defaultIdx);
    // Untouched while disabled — which is exactly why the re-enable has to
    // reset it rather than trusting it.
    ASSERT_TRUE(world.GetComponent<Components::VideoTextureComponent>(e)->playing);

    world.RemoveComponentImmediate<ECS::Disabled>(e);
    EXPECT_GT(TickUntilBound(world, mat, slot), 0)
        << "re-enabled entity never resumed: a fresh entry kept the stale auto-start flag, "
           "so playOnStart could not re-fire";

    m_rs->Textures().FlushPendingUploads();
}

// SF1. Two video entities on one material would each rebind their own handle
// into the same slot every tick. Every flip is a Material::MarkDirty, so the
// process-wide content epoch moves every frame and PackMaterialSSBO repacks the
// whole scene's materials forever. Single ownership makes the steady-state tick
// epoch-free, which is what the rebind comment in Update() claims.
TEST_F(VideoTextureSystemTest, TwoEntitiesOnOneMaterialDoNotChurnTheContentEpoch)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuid = GUID::Generate();
    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    ECS::World world;
    MakeVideoEntity(world, matGuid, video);
    MakeVideoEntity(world, matGuid, video);

    ASSERT_GT(TickUntilBound(world, mat, slot), 0);
    // Settle: both entries have decided who owns the slot by now.
    for (int i = 0; i < 4; ++i)
        m_system->Update(world, kTickSeconds);

    const uint64_t before = Material::GetGlobalContentEpoch();
    m_system->Update(world, kTickSeconds);
    m_system->Update(world, kTickSeconds);
    EXPECT_EQ(Material::GetGlobalContentEpoch(), before)
        << "two video entities are fighting over one material slot; every tick repacks "
           "every material in the scene";

    m_rs->Textures().FlushPendingUploads();
}

// ── Upload shape ──────────────────────────────────────────────────────────────

// Ticking far faster than the source's frame rate must not cost a queue
// submission per tick. Under a design that decodes inside the tick, every tick
// produces a frame and therefore an upload; here the great majority of ticks find
// nothing due and submit nothing at all.
TEST_F(VideoTextureSystemTest, TicksWithNoDueFrameCostNoSubmission)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuid = GUID::Generate();
    Material* mat = MakeMaterial(matGuid);
    ASSERT_NE(mat, nullptr);

    ECS::World world;
    MakeVideoEntity(world, matGuid, video);
    ASSERT_GT(TickUntilBound(world, mat, slot), 0);

    // Tick flat out over a window that contains only a handful of source frames.
    constexpr int kTicks = 400;
    const uint64 submitsBefore = m_system->GetUploadSubmitCount();
    for (int i = 0; i < kTicks; ++i)
        m_system->Update(world, kTickSeconds);
    const uint64 submits = m_system->GetUploadSubmitCount() - submitsBefore;

    EXPECT_LT(submits, static_cast<uint64>(kTicks) / 4u)
        << submits << " submissions across " << kTicks
        << " ticks — uploads are tracking the tick rate, not the source's frame rate";

    m_rs->Textures().FlushPendingUploads();
}

// Two video entities that both have a frame ready share ONE queue submission:
// entity count is a cost in copies, never in submissions.
TEST_F(VideoTextureSystemTest, EveryEntitySharesOneSubmissionPerTick)
{
    std::string video;
    RequireDecodableSample(video);
    if (IsSkipped() || HasFatalFailure()) return;
    const StringId slot = "albedoMap"_sid;
    const GUID matGuidA = GUID::Generate();
    const GUID matGuidB = GUID::Generate();
    Material* matA = MakeMaterial(matGuidA);
    Material* matB = MakeMaterial(matGuidB);
    ASSERT_NE(matA, nullptr);
    ASSERT_NE(matB, nullptr);

    ECS::World world;
    MakeVideoEntity(world, matGuidA, video);
    MakeVideoEntity(world, matGuidB, video);
    ASSERT_GT(TickUntilBound(world, matA, slot), 0);
    ASSERT_GT(TickUntilBound(world, matB, slot), 0);

    // Two rounds. The first is a warm-up: the staging ring is sized from the
    // first frame it ever saw, so the tick where a second source first coincides
    // can overflow it once and the recorded demand grows it for the next cycle.
    for (int round = 0; round < 2; ++round)
    {
        std::this_thread::sleep_for(kFrameArrivalWait);
        m_system->Update(world, kTickSeconds);
    }
    std::this_thread::sleep_for(kFrameArrivalWait);

    const uint64 submitsBefore = m_system->GetUploadSubmitCount();
    const uint64 uploadsBefore = m_system->GetUploadedFrameCount();
    m_system->Update(world, kTickSeconds);
    const uint64 submits = m_system->GetUploadSubmitCount() - submitsBefore;
    const uint64 uploads = m_system->GetUploadedFrameCount() - uploadsBefore;

    EXPECT_EQ(uploads, 2u) << "both entities should have had a frame ready";
    EXPECT_EQ(submits, 1u) << "two entities cost " << submits << " queue submissions";

    m_rs->Textures().FlushPendingUploads();
}
