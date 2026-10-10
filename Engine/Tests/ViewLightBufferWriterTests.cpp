// The per-view light uniform buffer carries the wind clock that both the depth
// prepass and the colour vertex stage deform by (instance_io.glsl reads
// Light.uTimeParams.zw), and it is one slot of a per-frame ring. It therefore
// has exactly one writer, WriteViewLightBuffer, which runs inside an acquired
// frame. A second writer in the application update phase changed those bytes
// while a frame still in flight was reading them: the canopy collapsed for a
// frame, or one mesh drew at two wind offsets at once.
//
// BeginWorldDrawFrame is that update-phase caller. This pins that it leaves the
// buffer alone.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "TestDeviceHelper.h"

#include <cstddef>
#include <cstring>

using namespace GameEngine;
using GameEngine::Engine::Renderer::ForwardLightUBO;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{
// A value no clock ever produces, so a surviving copy proves nobody wrote.
constexpr float kClockSentinel = -12345.0f;
} // namespace

TEST(ViewLightBufferWriter, BeginWorldDrawFrameDoesNotTouchThePerViewLightBuffer)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const Rendering::ViewId viewId =
        rs.Views().AllocateView("LightBufferWriterTestView", Rendering::CameraId{});

    ASSERT_TRUE(device->BeginFrame());
    rs.WriteViewLightBuffer(viewId);

    const auto* perView = rs.Views().FindPerView(viewId);
    ASSERT_NE(perView, nullptr);
    const uint32_t slot =
        device->GetFrameIndex();
    const Rendering::BufferHandle lightBuffer = perView->LightBuffers[slot];
    ASSERT_TRUE(lightBuffer.IsValid()) << "setup: the writer must have allocated this slot";

    const float sentinel[4] = {kClockSentinel, kClockSentinel, kClockSentinel, kClockSentinel};
    device->UpdateBuffer(lightBuffer, offsetof(ForwardLightUBO, uTimeParams), sizeof(sentinel),
                         sentinel);

    rs.BeginWorldDrawFrame();

    float readBack[4] = {};
    void* mapped = device->MapBuffer(lightBuffer);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(readBack,
                static_cast<const char*>(mapped) + offsetof(ForwardLightUBO, uTimeParams),
                sizeof(readBack));
    device->UnmapBuffer(lightBuffer);

    for (int lane = 0; lane < 4; ++lane)
        EXPECT_FLOAT_EQ(readBack[lane], kClockSentinel)
            << "lane " << lane
            << " changed: BeginWorldDrawFrame wrote the per-view light buffer, which it reaches "
               "from the application update phase, before the frame that last bound this slot is "
               "fenced";

    rs.Shutdown();
    device->Shutdown();
}
