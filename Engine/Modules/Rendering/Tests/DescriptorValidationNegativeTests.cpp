#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

#include "ScopedEnvVar.h"

#include <cstdlib>
#include <memory>
#include <vector>

using namespace GameEngine::Rendering;

TEST(DescriptorValidation, TypeMismatchTriggersAssert) {
#ifndef _DEBUG
    GTEST_SKIP() << "Type mismatch validation relies on debug asserts";
#else
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding0 = UniformBuffer[1]
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = 0x20;
    layout.bindings.push_back(b);
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "NegDS"; ds.transient = true;
    auto set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    // Create a texture to misuse as UBO
    TextureDesc td{}; td.width = 1; td.height = 1; td.format = (uint32_t)TextureFormat::RGBA8_UNORM; td.usage = (uint32_t)TextureUsage::ShaderResource; td.debugName = "NegTex";
    auto tex = dev->CreateTexture(td); ASSERT_TRUE(tex.IsValid());

#if GTEST_HAS_DEATH_TEST
    ASSERT_DEATH({ dev->UpdateImageBinding(set, 0, tex); }, "type mismatch");
#else
    GTEST_SKIP() << "No death-test support";
#endif

    dev->DestroyTexture(tex);
    dev->DestroyDescriptorSet(set);
#endif
}

TEST(DescriptorValidation, ArrayOverflowTriggersAssert) {
#ifndef _DEBUG
    GTEST_SKIP() << "Array overflow validation relies on debug asserts";
#else
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Layout: binding0 = Sampler[1]
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::Sampler; b.count = 1; b.shaderStages = 0x10;
    layout.bindings.push_back(b);
    DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "NegDS2"; ds.transient = true;
    auto set = dev->CreateDescriptorSet(ds);
    ASSERT_TRUE(set.IsValid());

    SamplerDesc sd{}; sd.debugName = "NegSamp"; auto s = dev->CreateSampler(sd); ASSERT_TRUE(s.IsValid());

#if GTEST_HAS_DEATH_TEST
    // Try to write 2 samplers into count=1
    ASSERT_DEATH({ dev->UpdateSamplers(set, 0, std::vector<SamplerHandle>{s, s}); }, "array bounds");
#else
    GTEST_SKIP() << "No death-test support";
#endif

    dev->DestroySampler(s);
    dev->DestroyDescriptorSet(set);
#endif
}


// --- Unresolvable handles in a descriptor write ------------------------------
//
// A SamplerHandle stays IsValid() forever — it is a plain id, and nothing
// re-stamps it when the VkDevice that owned the sampler dies. GetVkSampler then
// resolves it to VK_NULL_HANDLE, and a COMBINED_IMAGE_SAMPLER write carrying a
// null sampler is a VU violation the ICD does not tolerate: the NVIDIA driver
// dereferences the sampler object inside vkUpdateDescriptorSets and takes an
// access violation. That is a crash, not a validation message, so the write has
// to be dropped before it is submitted.
//
// These run on the LEGACY descriptor-pool path on purpose. The descriptor-buffer
// path already refuses a null handle in WriteDescriptorBufferUpdate, so a
// DB-enabled device would pass these vacuously — hence the explicit
// GE_VK_USE_DESCRIPTOR_BUFFER=0 and the IsDescriptorBufferEnabled() assertion.

namespace {

using GameEngine::Rendering::Tests::ScopedEnvVar;

// A handle that was never issued by this process's SamplerManager: IsValid() is
// true (it is not the invalid sentinel) but it resolves to no live sampler.
// Same observable state a cached handle reaches after its device is rebuilt,
// without needing to lose a device to get there.
constexpr uint64_t kUnissuedHandleId = 0x0BADC0DEull;

// The legacy descriptor path is the CALLER's scope, not this function's, so
// GE_VK_USE_DESCRIPTOR_BUFFER=0 cannot outlive a failed assertion and silently
// move every later device off descriptor buffers. Each caller asserts on
// IsDescriptorBufferEnabled(), so forgetting the scope fails loudly.
std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    // Deliberately unscoped: every device in this process wants headless mode.
    GameEngine::Rendering::Tests::SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

} // namespace

TEST(DescriptorNullHandleGuard, StaleSamplerInCombinedImageSamplerWriteIsDroppedNotSubmitted)
{
    ScopedEnvVar legacyPath("GE_VK_USE_DESCRIPTOR_BUFFER", "0");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    // Instrument check: a descriptor-buffer device would exercise a different,
    // already-guarded write path and prove nothing here.
    ASSERT_FALSE(dev->IsDescriptorBufferEnabled())
        << "GE_VK_USE_DESCRIPTOR_BUFFER=0 did not take effect; this test would be vacuous";

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{};
    b.binding = 0;
    b.type = DescriptorType::CombinedImageSampler;
    b.count = 1;
    b.shaderStages = 0x10;
    layout.bindings.push_back(b);
    layout.debugName = "NullSamplerGuardLayout";

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = layout;
    dsDesc.debugName = "NullSamplerGuardDS";
    dsDesc.transient = true;
    auto set = dev->CreateDescriptorSet(dsDesc);
    ASSERT_TRUE(set.IsValid());

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    td.usage = (uint32_t)TextureUsage::ShaderResource;
    td.debugName = "NullSamplerGuardTex";
    auto tex = dev->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    const SamplerHandle stale{kUnissuedHandleId};
    ASSERT_TRUE(stale.IsValid()) << "the whole point: a dead handle still reports valid";

    // Before the guard this faulted inside nvoglv64 with a read of NULL+0xF0 and
    // took the process with it. Surviving the call IS the assertion.
    dev->UpdateCombinedImageSamplerBinding(set, /*binding=*/0, tex, stale);

    // A real sampler through the same path must still land, so the guard cannot
    // be passing by refusing everything.
    SamplerDesc sd{};
    sd.debugName = "NullSamplerGuardSampler";
    auto live = dev->CreateSampler(sd);
    ASSERT_TRUE(live.IsValid());
    dev->UpdateCombinedImageSamplerBinding(set, /*binding=*/0, tex, live);

    dev->DestroySampler(live);
    dev->DestroyTexture(tex);
    dev->DestroyDescriptorSet(set);
    dev->Shutdown();
}

TEST(DescriptorNullHandleGuard, StaleSamplerInSamplerOnlyWriteIsDroppedNotSubmitted)
{
    ScopedEnvVar legacyPath("GE_VK_USE_DESCRIPTOR_BUFFER", "0");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    ASSERT_FALSE(dev->IsDescriptorBufferEnabled())
        << "GE_VK_USE_DESCRIPTOR_BUFFER=0 did not take effect; this test would be vacuous";

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{};
    b.binding = 0;
    b.type = DescriptorType::Sampler;
    b.count = 1;
    b.shaderStages = 0x10;
    layout.bindings.push_back(b);
    layout.debugName = "NullSamplerOnlyGuardLayout";

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = layout;
    dsDesc.debugName = "NullSamplerOnlyGuardDS";
    dsDesc.transient = true;
    auto set = dev->CreateDescriptorSet(dsDesc);
    ASSERT_TRUE(set.IsValid());

    const SamplerHandle stale{kUnissuedHandleId};
    dev->UpdateSamplers(set, /*binding=*/0, std::vector<SamplerHandle>{stale});

    SamplerDesc sd{};
    sd.debugName = "NullSamplerOnlyGuardSampler";
    auto live = dev->CreateSampler(sd);
    ASSERT_TRUE(live.IsValid());
    dev->UpdateSamplers(set, /*binding=*/0, std::vector<SamplerHandle>{live});

    dev->DestroySampler(live);
    dev->DestroyDescriptorSet(set);
    dev->Shutdown();
}
