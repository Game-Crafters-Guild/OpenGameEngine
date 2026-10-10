// Tests for the MaterialBinder service. Exercises the name-driven set
// resolution against three sources (DrawBindings, PassResources, Material),
// the bindless texture short-circuit, and the BeginPass / EndPass /
// OnBeginFrame lifecycle against a real Vulkan device — no production callers
// are wired yet, so we validate via direct API calls.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "AssetCore/GUID.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"

#include "TestDeviceHelper.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{

// Helper: allocate a host-visible buffer with the given usage. Tests just
// need a valid handle for descriptor writes — no draw is actually issued.
BufferHandle MakeTestBuffer(IDevice& dev, size_t size, BufferUsage usage)
{
    BufferDesc desc{};
    desc.size        = size;
    desc.usage       = static_cast<uint32_t>(usage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName   = "MaterialBinderTests.Buffer";
    return dev.CreateBuffer(desc);
}

// Build a synthetic ShaderMeta declaring set 2 with two SSBO bindings —
// mirrors terrain's PatchSSBO + ParamsSSBO shape.
std::shared_ptr<ShaderMeta> MakeTerrainStyleMeta()
{
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 2;

    DescriptorBindingMeta b0{};
    b0.Binding = 0;
    b0.Name = "PatchSSBO";
    b0.Type = ShaderMetaBindingType::kStorageBuffer;
    b0.Count = 1;
    b0.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b0);

    DescriptorBindingMeta b1{};
    b1.Binding = 1;
    b1.Name = "ParamsSSBO";
    b1.Type = ShaderMetaBindingType::kStorageBuffer;
    b1.Count = 1;
    b1.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b1);

    meta->Sets.push_back(s);
    return meta;
}

// Synthesize an empty PassBindingContext suitable for hermetic tests of
// BuildSetForBinding. The cmd / pass pointers stay null — BuildSetForBinding
// doesn't need them.
PassBindingContext MakeEmptyPassContext()
{
    PassBindingContext pass{};
    pass.View          = 0;
    pass.FrameSlot     = 0;
    pass.Samples       = 1;
    return pass;
}

} // namespace

// -- Test ---------------------------------------------------------------------
// BuildSetForBinding reads the material's reflected ShaderMeta. Terrain-style
// set 2 (PatchSSBO + ParamsSSBO) with matching named DrawBindings builds a set;
// an index the meta does not declare returns an invalid handle. Two successive
// calls produce two distinct transient sets (per-draw scope, no cache), and the
// binder still builds after OnBeginFrame clears its transient caches.

TEST(MaterialBinderTests, BuildSetForBinding_PerDrawScopeIsUncached)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Terrain", 16u);
    Material::TestFactory::SetShaderMeta(mat, MakeTerrainStyleMeta());

    auto patchBuf  = MakeTestBuffer(*device, 1024, BufferUsage::Storage);
    auto paramsBuf = MakeTestBuffer(*device, 256,  BufferUsage::Storage);
    DrawBindings::BufferEntry bufs[] = {
        {HashStringId("PatchSSBO"),  patchBuf,  0, 1024},
        {HashStringId("ParamsSSBO"), paramsBuf, 0, 256},
    };
    DrawBindings draw{};
    draw.Buffers = bufs;

    auto& binder = rs.Materials().Binder();
    auto pass = MakeEmptyPassContext();
    auto setN1 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(setN1.IsValid());

    auto setN2 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(setN2.IsValid());
    EXPECT_NE(setN1, setN2)
        << "Per-draw scope allocates per-call — no cross-draw cache";

    EXPECT_FALSE(binder.BuildSetForBinding(pass, mat, 3, draw).IsValid())
        << "a set index the meta does not declare must not build";

    binder.OnBeginFrame();
    EXPECT_TRUE(binder.BuildSetForBinding(pass, mat, 2, draw).IsValid())
        << "the binder must still build after OnBeginFrame";

    device->DestroyBuffer(patchBuf);
    device->DestroyBuffer(paramsBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Bindless materials short-circuit: a shader declaring the engine's bindless
// texture set shape (single CombinedImageSampler array named
// ge_BindlessTextures) returns the global bindless descriptor set directly.

TEST(MaterialBinderTests, BindlessTextureSet_ShortCircuitsToGlobalSet)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    if (!rs.Textures().IsBindlessEnabled())
        GTEST_SKIP() << "Device does not support bindless";

    const auto globalBindless = rs.Textures().BindlessTextureSet();
    ASSERT_TRUE(globalBindless.IsValid());

    // Craft a shader meta where set=1 is the bindless texture array — the
    // canonical shape declared in shadow_sampling.glsl /
    // adapter_forward.glsl.
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 1;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "ge_BindlessTextures";
    b.Type       = ShaderMetaBindingType::kCombinedImageSampler;
    b.Count      = 1; // SPIR-V reflection reports unsized sampler arrays as 1.
    b.StagesMask = (1u << 0) | (1u << 1);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Bindless", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);
    // The short-circuit is driven by the set's shape (IsBindlessTextureSet) +
    // TextureService::IsBindlessEnabled, not by any material variant keyword.

    auto& binder = rs.Materials().Binder();
    auto pass = MakeEmptyPassContext();
    DrawBindings draw{};

    auto h = binder.BuildSetForBinding(pass, mat, 1, draw);
    EXPECT_EQ(h, globalBindless)
        << "Bindless texture set must short-circuit to the engine's global set";

    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Source priority: DrawBindings outranks PassResources when both have an
// entry under the same name. The set's scope escalates to PerDraw.

TEST(MaterialBinderTests, NameDriven_DrawBindingsShadowsPassResources)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 2;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Shared";
    b.Type       = ShaderMetaBindingType::kStorageBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Shadow", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto passBuf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
    auto drawBuf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
    ASSERT_TRUE(passBuf.IsValid());
    ASSERT_TRUE(drawBuf.IsValid());

    auto pass = MakeEmptyPassContext();
    pass.PassResources.Buffers.push_back({HashStringId("Shared"), passBuf, 0, 0});

    DrawBindings::BufferEntry bufs[] = {{HashStringId("Shared"), drawBuf, 0, 256}};
    DrawBindings draw{};
    draw.Buffers = bufs;

    auto& binder = rs.Materials().Binder();

    auto h1 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(h1.IsValid());

    // Per-draw scope -> a second call must allocate a fresh handle, proving
    // the classifier saw the DrawBindings entry and did NOT fall back to
    // PassResources (which would have cached).
    auto h2 = binder.BuildSetForBinding(pass, mat, 2, draw);
    ASSERT_TRUE(h2.IsValid());
    EXPECT_NE(h1, h2)
        << "DrawBindings hit must classify as per-draw (uncached), not per-pass";

    device->DestroyBuffer(passBuf);
    device->DestroyBuffer(drawBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Per-pass cache key must include PassInstanceIndex. Two pass contexts sharing
// (view, keywords, layout) but with different instance indices must NOT alias
// to the same cached descriptor set — otherwise shadow cascades 1..N reuse
// cascade 0's set whose Cam UBO was written from cascade 0's BufferEntry.
// RenderServices encodes the depth pass type into the high 16 bits (depth
// prepass = 0, shadow cascade 0 = 1 << 16), so the prepass and cascade 0 must
// not collide either, or cascade 0 renders with the main camera's Cam UBO.

TEST(MaterialBinderTests, PerPassCache_DistinguishesByInstanceIndex)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Single-binding "Cam" UBO at set 3 — classified as per-pass since the only
    // source is PassResources (no DrawBindings, no Material UBO).
    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 3;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Cam";
    b.Type       = ShaderMetaBindingType::kUniformBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "CamCascades", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf0 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    auto camBuf1 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    auto camCascade0 = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    ASSERT_TRUE(camBuf0.IsValid());
    ASSERT_TRUE(camBuf1.IsValid());
    ASSERT_TRUE(camCascade0.IsValid());

    auto& binder = rs.Materials().Binder();
    DrawBindings draw{};

    // Instance 0 (cascade 0) writes camBuf0 into "Cam".
    auto pass0 = MakeEmptyPassContext();
    pass0.PassInstanceIndex = 0;
    pass0.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf0, 0, 0});
    auto h0 = binder.BuildSetForBinding(pass0, mat, 3, draw);
    ASSERT_TRUE(h0.IsValid());

    // Instance 1 (cascade 1) writes camBuf1 into "Cam" but shares (view,
    // keywords, layout) with pass0. Must NOT reuse pass0's handle — otherwise
    // cascade 1 renders with cascade 0's camera.
    auto pass1 = MakeEmptyPassContext();
    pass1.PassInstanceIndex = 1;
    pass1.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf1, 0, 0});
    auto h1 = binder.BuildSetForBinding(pass1, mat, 3, draw);
    ASSERT_TRUE(h1.IsValid());
    EXPECT_NE(h0, h1)
        << "Per-pass cache must key on PassInstanceIndex so shadow cascades "
           "don't alias to cascade 0's descriptor set";

    // Shadow cascade 0 as RenderServices encodes it: PassType::ShadowCascade (1) << 16.
    auto passCascade0 = MakeEmptyPassContext();
    passCascade0.PassInstanceIndex = (static_cast<uint32_t>(1) << 16) | 0u;
    passCascade0.PassResources.Buffers.push_back({HashStringId("Cam"), camCascade0, 0, 0});
    auto hCascade0 = binder.BuildSetForBinding(passCascade0, mat, 3, draw);
    ASSERT_TRUE(hCascade0.IsValid());
    EXPECT_NE(h0, hCascade0) << "the depth prepass (0) and shadow cascade 0 (1 << 16) must not share a set";
    EXPECT_NE(h1, hCascade0);

    // Same instance index in a fresh context still hits the cache.
    auto pass0Again = MakeEmptyPassContext();
    pass0Again.PassInstanceIndex = 0;
    pass0Again.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf0, 0, 0});
    auto h0Again = binder.BuildSetForBinding(pass0Again, mat, 3, draw);
    EXPECT_EQ(h0, h0Again);

    device->DestroyBuffer(camBuf0);
    device->DestroyBuffer(camBuf1);
    device->DestroyBuffer(camCascade0);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// The per-pass L1 is thread_local, so its slots outlive every binder that fills
// them. A slot is only safe to trust if what stamps it cannot recur once its
// writer is gone — an address recurs as soon as the allocator hands it back, and
// a per-binder frame counter starts from the same value in every binder, so
// neither identifies the writer on its own.
//
// Both binders are placement-constructed in one buffer to make that address
// reuse exact instead of incidental, and share one device so that its
// monotonically increasing descriptor-set ids make a real build impossible to
// confuse with a handle served out of a slot the dead binder left behind.

TEST(MaterialBinderTests, PerPassL1_DoesNotServeADeadBindersHandle)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto meta = std::make_shared<ShaderMeta>();
    DescriptorSetMeta s{};
    s.Set = 3;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "Cam";
    b.Type       = ShaderMetaBindingType::kUniformBuffer;
    b.Count      = 1;
    b.StagesMask = (1u << 0);
    s.Bindings.push_back(b);
    meta->Sets.push_back(s);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "L1OwnerRecycle", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf = MakeTestBuffer(*device, 256, BufferUsage::Uniform);
    ASSERT_TRUE(camBuf.IsValid());

    DrawBindings draw{};
    const auto makePass = [&]() {
        auto pass = MakeEmptyPassContext();
        pass.PassInstanceIndex = 0;
        pass.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf, 0, 0});
        return pass;
    };

    alignas(MaterialBinder) std::byte storage[sizeof(MaterialBinder)];

    auto* first = new (storage) MaterialBinder(rs, *device);
    auto passFirst = makePass();
    const auto hFirst = first->BuildSetForBinding(passFirst, mat, 3, draw);
    ASSERT_TRUE(hFirst.IsValid());
    first->~MaterialBinder();

    auto* second = new (storage) MaterialBinder(rs, *device);
    auto passSecond = makePass();
    const auto hSecond = second->BuildSetForBinding(passSecond, mat, 3, draw);
    ASSERT_TRUE(hSecond.IsValid());
    second->~MaterialBinder();

    EXPECT_NE(hFirst, hSecond)
        << "A per-pass L1 slot filled by a destroyed binder was served to a new "
           "binder occupying its address. The new binder's own per-pass cache is "
           "empty, so it never built this handle, and on a device whose "
           "descriptor-set ids only increase a genuine build cannot return the "
           "earlier one. Whatever validates an L1 slot must not be a value a "
           "later binder can repeat.";

    device->DestroyBuffer(camBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -- Test ---------------------------------------------------------------------
// Device-lost hole closure (twice-fatal: grass 'Grass' UBO + C3 EditorPreview).
// A statically-used set-2 uniform/storage buffer that resolves from neither
// DrawBindings nor PassResources must set the outUnresolvable flag, so
// BindMaterialForDraw SKIPS the draw instead of issuing it with an unbound
// descriptor — which under descriptor buffers is a garbage device address ->
// fetch fault -> device lost. When a provider exists, the flag stays false.
TEST(MaterialBinderTests, BuildSetForBindingFromMeta_FlagsUnresolvableStaticBuffer)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Set 2 declaring one SSBO with a name no source provides by default.
    DescriptorSetMeta setMeta{};
    setMeta.Set = 2;
    DescriptorBindingMeta b{};
    b.Binding    = 0;
    b.Name       = "NeedsProvider";
    b.Type       = ShaderMetaBindingType::kStorageBuffer;
    b.Count      = 1;
    b.StagesMask = 1u;
    setMeta.Bindings.push_back(b);

    Material mat = Material::TestFactory::Create(GUID::Generate(), "Unresolvable", 16u);
    Material::TestFactory::SetShaderMeta(mat, std::make_shared<ShaderMeta>());

    auto& binder = rs.Materials().Binder();

    // No provider anywhere -> flagged unresolvable (draw would be skipped).
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_TRUE(unresolvable)
            << "an unprovided statically-used SSBO must flag unresolvable so the draw is skipped "
               "instead of binding a garbage-address descriptor";
    }

    // Provider present in DrawBindings -> resolves, flag stays false.
    {
        auto buf = MakeTestBuffer(*device, 256, BufferUsage::Storage);
        ASSERT_TRUE(buf.IsValid());
        DrawBindings::BufferEntry bufs[] = {{HashStringId("NeedsProvider"), buf, 0, 256}};
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        draw.Buffers = bufs;
        bool unresolvable = false;
        auto h = binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_FALSE(unresolvable) << "a provided SSBO must not flag unresolvable";
        EXPECT_TRUE(h.IsValid());
        device->DestroyBuffer(buf);
    }

    rs.Shutdown();
    device->Shutdown();
}

// -- Slot defaults --------------------------------------------------------------
// A sampled-image slot nothing provides is bound to a default of the slot's own shape: WebGPU
// validates every entry of a bind group against its layout, and a 2D colour default in a depth
// 2D-array slot (the shadow arrays a depth-only head's set 0 declared) invalidated the bind group
// and the frame's command buffer with it (#3368). A depth slot takes a reverse-Z-far depth texture
// of its view dimension and the comparison sampler; a cube slot a cube; a 2D-array slot an array.

namespace
{
DescriptorBindingMeta ImageBinding(const char* name, bool depth, bool arrayed, bool cube, bool multisample = false)
{
    DescriptorBindingMeta b{};
    b.Binding    = 9;
    b.Name       = name;
    b.Type       = ShaderMetaBindingType::kCombinedImageSampler;
    b.StagesMask = ShaderMetaStage::kFragment;
    DescriptorBindingMeta::ImageInfo image{};
    image.Dim         = 2;
    image.Depth       = depth;
    image.Arrayed     = arrayed;
    image.Cube        = cube;
    image.Multisample = multisample;
    b.Image = image;
    return b;
}
} // namespace

TEST(MaterialBinderTests, SlotDefault_HasTheSlotsShape)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto& binder = rs.Materials().Binder();
    // The sampler the binder writes beside a colour default: the material's own.
    const SamplerHandle materialSampler = rs.Textures().GetSampler(SamplerPreset::LinearRepeat);
    ASSERT_TRUE(materialSampler.IsValid());
    ASSERT_NE(materialSampler, rs.GetCascadeShadowSampler());

    const auto depthArray = binder.ResolveSlotDefault(ImageBinding("ge_shadowMapArray", true, true, false), materialSampler);
    EXPECT_EQ(depthArray.Texture, rs.GetCascadeShadowFallbackTexture());
    EXPECT_EQ(device->GetTextureFormat(depthArray.Texture), TextureFormat::D32_FLOAT);
    EXPECT_GT(device->GetTextureArrayLayers(depthArray.Texture), 1u) << "a 2D-array view";
    EXPECT_EQ(depthArray.Sampler, rs.GetCascadeShadowSampler()) << "a depth slot pairs with the comparison sampler";

    const auto depth2D = binder.ResolveSlotDefault(ImageBinding("ge_areaShadowMap", true, false, false), materialSampler);
    EXPECT_EQ(depth2D.Texture, rs.GetAreaShadowFallbackTexture());
    EXPECT_EQ(device->GetTextureFormat(depth2D.Texture), TextureFormat::D32_FLOAT);
    EXPECT_EQ(device->GetTextureArrayLayers(depth2D.Texture), 1u) << "a 2D view";
    EXPECT_EQ(depth2D.Sampler, rs.GetCascadeShadowSampler());

    const auto cube = binder.ResolveSlotDefault(ImageBinding("ge_irradianceCubeTex", false, false, true), materialSampler);
    EXPECT_TRUE(cube.Texture.IsValid());
    EXPECT_EQ(cube.Texture, rs.Textures().GetDefaultBlackCubeTexture());
    EXPECT_EQ(device->GetTextureArrayLayers(cube.Texture), 6u) << "a cube's six faces";
    EXPECT_EQ(cube.Sampler, materialSampler) << "a colour slot takes the material's sampler";

    const auto colourArray = binder.ResolveSlotDefault(ImageBinding("ge_shadowMomentsArray", false, true, false), materialSampler);
    EXPECT_TRUE(colourArray.Texture.IsValid());
    EXPECT_EQ(colourArray.Texture, rs.Textures().GetDefaultWhiteArrayTexture());
    EXPECT_EQ(colourArray.Sampler, materialSampler);

    const auto colour2D = binder.ResolveSlotDefault(ImageBinding("ge_brdfLUTTex", false, false, false), materialSampler);
    EXPECT_EQ(colour2D.Texture, rs.Textures().ResolveDefaultTexture("ge_brdfLUTTex"));
    EXPECT_NE(colour2D.Texture, depthArray.Texture);
    EXPECT_EQ(colour2D.Sampler, materialSampler);

    EXPECT_FALSE(binder.ResolveSlotDefault(ImageBinding("ge_sceneDepthMS", false, false, false, true), materialSampler).Texture.IsValid())
        << "a multisampled slot has no default; binding a single-sample one would be the wrong shape";
    EXPECT_FALSE(binder.ResolveSlotDefault(ImageBinding("ge_depthCube", true, false, true), materialSampler).Texture.IsValid())
        << "a depth cube has no default";

    rs.Shutdown();
    device->Shutdown();
}

// A sampled-image slot whose shape has no default (here a 3D texture) cannot be written, and an
// unwritten entry is an invalid bind group on WebGPU and a garbage descriptor under descriptor
// buffers: the set flags unresolvable so the draw is skipped, the same rule as an unbound buffer.
TEST(MaterialBinderTests, BuildSetForBindingFromMeta_FlagsAnUnresolvedSlotWithNoDefaultShape)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto& binder = rs.Materials().Binder();
    Material mat = Material::TestFactory::Create(GUID::Generate(), "NoDefaultShape", 16u);
    Material::TestFactory::SetShaderMeta(mat, std::make_shared<ShaderMeta>());

    DescriptorSetMeta setMeta{};
    setMeta.Set = 2;
    DescriptorBindingMeta volume = ImageBinding("gVolumeNobodyProvides", false, false, false);
    volume.Image->Dim = 3;
    setMeta.Bindings.push_back(volume);
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_TRUE(unresolvable) << "a slot with no default of its shape must skip the draw, not issue it unwritten";
    }

    // A shape that has a default resolves to it and leaves the draw in.
    setMeta.Bindings = {ImageBinding("ge_shadowMapArray", true, true, false)};
    {
        auto pass = MakeEmptyPassContext();
        DrawBindings draw{};
        bool unresolvable = false;
        binder.BuildSetForBindingFromMeta(pass, mat, setMeta, draw, &unresolvable);
        EXPECT_FALSE(unresolvable) << "a depth-array slot binds its default";
    }

    rs.Shutdown();
    device->Shutdown();
}

// -- InvalidateStickyBindsForPipeline -----------------------------------------
// Vulkan pipeline-layout-compatibility rule: when a pipeline switch keeps the
// set-N layout but changes set-(N+1)'s, only set-(N+1) and above are disturbed.
// MaterialBinder mirrors that on its sticky-bind tracking so the next descriptor
// walk only rebinds what the GPU actually invalidated. These tests pin the rule
// so a future change that over-clears (regressing back to "fill({}) on every
// pipeline change") is caught instead of silently re-introducing redundant
// vkCmdBindDescriptorSets calls in the per-draw hot path.

namespace
{

// Synthesize a fully-populated PassBindingContext so we can observe which
// slots InvalidateStickyBindsForPipeline preserves vs clears. Sets / layouts
// at indices [0..3] get distinct non-zero values; [4..7] stay default.
PassBindingContext MakePopulatedPassContext()
{
    PassBindingContext pass{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        // Distinct, non-zero set handles + layout ids so a missed clear is
        // observable as the original value surviving.
        pass.CurrentSets[i]       = DescriptorSetHandle{0x100ull + i};
        pass.CurrentSetLayouts[i] = DescriptorSetLayoutId{1u + i};
    }
    return pass;
}

} // namespace

TEST(MaterialBinderTests, InvalidateStickyBinds_FirstDiffPreservesLowerSetsClearsHigher)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // New pipeline shares set-0's layout (id=1) but set-1's layout differs (id=99).
    // Vulkan keeps set-0 sticky and disturbs sets [1..). Set-2 / set-3 had
    // distinct layouts on the prior pipeline; they're all cleared regardless
    // because firstDiff is the lowest mismatching set.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{1u},   // same as prior pipeline -> set-0 sticky
        DescriptorSetLayoutId{99u},  // differs -> firstDiff
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    EXPECT_EQ(pass.CurrentSets[0], DescriptorSetHandle{0x100ull})
        << "Set-0 layout unchanged across pipeline change; sticky bind must survive";
    for (uint32_t i = 1; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Set " << i << " is at or above firstDiff -> sticky bind must be cleared";
    }

    // Layout tracking: untouched below firstDiff (still {1}); rewritten from
    // firstDiff upward (set-1 -> {99}; sets without an entry in the new layouts
    // span fall back to default-constructed).
    EXPECT_EQ(pass.CurrentSetLayouts[0], DescriptorSetLayoutId{1u});
    EXPECT_EQ(pass.CurrentSetLayouts[1], DescriptorSetLayoutId{99u});
    for (uint32_t i = 2; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid())
            << "Slots beyond the new pipeline's layout span must clear to default";
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_AllLayoutsMatchPreservesEverySet)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Same layout ids as the populated context for slots [0..3]; trailing
    // slots stay default, which matches the prior pipeline's defaults too.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{1u},
        DescriptorSetLayoutId{2u},
        DescriptorSetLayoutId{3u},
        DescriptorSetLayoutId{4u},
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    for (uint32_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(pass.CurrentSets[i], DescriptorSetHandle{0x100ull + i})
            << "Identical layout at every slot must preserve every sticky bind (set " << i << ")";
        EXPECT_EQ(pass.CurrentSetLayouts[i], DescriptorSetLayoutId{1u + i});
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_FirstSlotDiffersClearsAllSets)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Set-0 layout differs -> firstDiff is 0, every set is disturbed.
    const DescriptorSetLayoutId newLayouts[] = {
        DescriptorSetLayoutId{42u},
    };
    binder.InvalidateStickyBindsForPipeline(pass, newLayouts);

    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Set-0 layout differs -> Vulkan disturbs all sets, sticky bind " << i << " must clear";
    }
    EXPECT_EQ(pass.CurrentSetLayouts[0], DescriptorSetLayoutId{42u});
    for (uint32_t i = 1; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid());
    }

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, InvalidateStickyBinds_EmptyLayoutsConservativelyClearsAll)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    auto& binder = rs.Materials().Binder();
    auto pass = MakePopulatedPassContext();

    // Callers that didn't precompute layout ids fall back to clearing every
    // slot — same behavior as before the PR #74 smart-clear optimisation.
    binder.InvalidateStickyBindsForPipeline(
        pass, std::span<const DescriptorSetLayoutId>{});

    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        EXPECT_FALSE(pass.CurrentSets[i].IsValid())
            << "Empty layout span must clear every sticky bind (legacy fallback)";
        EXPECT_FALSE(pass.CurrentSetLayouts[i].IsValid())
            << "Empty layout span must also clear layout tracking";
    }

    rs.Shutdown();
    device->Shutdown();
}

// -- Variant-cache cross-thread eviction queue --------------------------------
// The four variant caches in RenderServices are written from the render thread
// only. Material unregister callbacks can fire from any thread (asset reloader,
// GC worker); they hand off via EnqueueVariantCacheEviction, and the render
// thread drains at BeginWorldDrawFrame. These tests pin the contract:
//   1. Enqueue is safe under concurrent writers.
//   2. The drain is monotonic — every enqueued pointer is consumed exactly
//      once per BeginWorldDrawFrame call, no losses, no duplicates remaining.
// Drain itself is single-threaded by design, so we don't race readers/writers
// in the test — that's the very contract the deferred queue is here to enforce.

TEST(MaterialBinderTests, VariantCacheEviction_ConcurrentEnqueueAccumulates)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ASSERT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u);

    // Fan out across 4 worker threads, each enqueueing 64 sentinel pointers.
    // Pointers are never dereferenced (Drain only does address comparison
    // against the cache key.material), so we can fabricate them from
    // integers — the real unregister path uses the actual Material*.
    constexpr int kThreads = 4;
    constexpr int kPerThread = 64;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kPerThread; ++i)
            {
                const uintptr_t bits = (static_cast<uintptr_t>(t) << 32) | static_cast<uintptr_t>(i + 1);
                rs.Materials().Variants().EnqueueEviction(reinterpret_cast<const Material*>(bits));
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);

    for (auto& w : workers)
        w.join();

    // Total = kThreads * kPerThread; mutex serialisation guarantees no losses.
    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(),
              static_cast<size_t>(kThreads * kPerThread))
        << "Concurrent enqueues must accumulate — mutex must protect push_back";

    rs.Shutdown();
    device->Shutdown();
}

TEST(MaterialBinderTests, VariantCacheEviction_BeginWorldDrawFrameDrainsQueue)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Enqueue from this thread (the test's main thread, which will also play
    // the role of render thread when we call BeginWorldDrawFrame). Variant
    // caches are empty, so Drain finds nothing to erase — what we're pinning
    // here is that the queue itself is drained back to zero each frame.
    for (uintptr_t i = 1; i <= 32; ++i)
        rs.Materials().Variants().EnqueueEviction(reinterpret_cast<const Material*>(i));

    ASSERT_EQ(rs.Materials().Variants().PendingEvictionCount(), 32u);

    rs.BeginWorldDrawFrame();

    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u)
        << "BeginWorldDrawFrame must drain the entire pending eviction queue";

    // Calling again on an empty queue must remain a no-op (Drain early-exits
    // when there's nothing pending).
    rs.BeginWorldDrawFrame();
    EXPECT_EQ(rs.Materials().Variants().PendingEvictionCount(), 0u);

    rs.Shutdown();
    device->Shutdown();
}
