#include <gtest/gtest.h>
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

namespace {
struct SmallPC { int a; float b; };
struct MediumPC { float v[16]; };
}

TEST(PushConstants, TypedHelperForwardsToSetConstants) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();

    // Ensure SetPushConstants compiles and calls through without crashing
    SmallPC s{42, 3.14f};
    cl->SetPushConstants(s);

    MediumPC m{};
    for (int i=0;i<16;++i) m.v[i] = float(i);
    cl->SetPushConstants(m);

    cl->End();
}

