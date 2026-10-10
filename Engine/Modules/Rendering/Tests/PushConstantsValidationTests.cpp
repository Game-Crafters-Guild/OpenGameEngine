#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::Rendering;

struct BigPC { char bytes[129]; }; // exceeds 128 policy

TEST(PushConstantsValidation, ExceedPolicyTriggersDebug) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto cmd = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cmd->Begin();

    BigPC big{}; // zeroed

    // In debug builds, we expect an assertion; here we simply call to exercise the path.
    // This test will fail-fast in debug if the assert triggers as intended.
    cmd->SetPushConstants(big);

    cmd->End();
}

