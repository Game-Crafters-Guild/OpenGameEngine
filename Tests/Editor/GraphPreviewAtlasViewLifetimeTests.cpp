// The atlas allocates a ViewRegistry camera and a view lazily, on the first
// tick that has cells to draw. Both are registry slots with no owner but this
// class, so Shutdown has to give both back — a material graph opened and closed
// repeatedly in one session would otherwise grow m_Cameras without bound.

#include "ShaderGraph/GraphPreviewAtlas.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using GameEngine::Editor::GraphPreviewAtlas;

namespace
{

constexpr uint64_t kWindowId = 11;

class GraphPreviewAtlasViewLifetimeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no Vulkan device available";
        m_Services = std::make_unique<GameEngine::Engine::Renderer::RenderServices>();
        if (!m_Services->Initialize(m_Device.get()))
        {
            m_Services.reset();
            GTEST_SKIP() << "RenderServices::Initialize failed on a device that reported ready";
        }
        m_Harness = std::make_unique<UiRgHarness>(m_Device.get());
    }

    void TearDown() override
    {
        m_Harness.reset();
        m_Services.reset();
        m_Device.reset();
    }

    /// One declare-only frame. The atlas allocates its camera and view before it
    /// reaches anything that needs a resolvable material, so a request carrying
    /// an unknown GUID is enough to drive the allocation and nothing more.
    void Tick(GraphPreviewAtlas& atlas)
    {
        GameEngine::Rendering::RenderGraph::RGFrame frame(
            m_Harness->Dev, &m_Harness->Persistent, &m_Harness->Transient, &m_Harness->Ring);
        frame.BeginFrame(++m_Harness->FrameIndex);
        atlas.TickRG(kWindowId, nullptr, frame);
    }

    size_t CameraCount() const { return m_Services->Views().GetCameras().size(); }

    std::unique_ptr<GameEngine::Rendering::IDevice> m_Device;
    std::unique_ptr<GameEngine::Engine::Renderer::RenderServices> m_Services;
    std::unique_ptr<UiRgHarness> m_Harness;
};

} // namespace

TEST_F(GraphPreviewAtlasViewLifetimeTest, ShutdownReturnsTheCameraSlotItAllocated)
{
    const size_t baseline = CameraCount();

    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    atlas.SetRequests(kWindowId, {GraphPreviewAtlas::NodeRequest{"node0", GameEngine::GUID{}}});
    Tick(atlas);

    // Guards the test itself: without an allocation there is nothing to leak and
    // the release assertion below would pass vacuously.
    ASSERT_EQ(CameraCount(), baseline + 1) << "the atlas never allocated its camera";

    atlas.Shutdown();
    EXPECT_EQ(CameraCount(), baseline);
}

TEST_F(GraphPreviewAtlasViewLifetimeTest, OpenCloseCyclesDoNotAccumulateCameras)
{
    const size_t baseline = CameraCount();
    constexpr int kCycles = 4;

    for (int i = 0; i < kCycles; ++i)
    {
        GraphPreviewAtlas atlas;
        atlas.Initialize(m_Services.get());
        atlas.SetRequests(kWindowId, {GraphPreviewAtlas::NodeRequest{"node0", GameEngine::GUID{}}});
        Tick(atlas);
        atlas.Shutdown();
    }

    EXPECT_EQ(CameraCount(), baseline);
}
