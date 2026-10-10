#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <Rendering/Common/Utils.h>
#include "TestUtils.h"
#include <Rendering/Core/CommandList.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Materials/MaterialBuilder.h>
#include <Rendering/Materials/ShaderReflection.h>

using namespace GameEngine::Rendering;

namespace
{
struct UboBindingInfo
{
    uint32_t set = 0, binding = 0, size = 0;
    bool has = false;
};
struct SamplerBindingInfo
{
    // Either combined OR separate
    bool hasCombined = false;
    uint32_t combinedSet = 0, combinedBinding = 0;
    bool hasImage = false;
    uint32_t imageSet = 0, imageBinding = 0;
    bool hasSampler = false;
    uint32_t samplerSet = 0, samplerBinding = 0;
};

static int OrdinalOf(const ShaderMeta& meta, uint32_t setIndex)
{
    std::vector<uint32_t> ord;
    ord.reserve(meta.Sets.size());
    for (const auto& s : meta.Sets)
        ord.push_back(s.Set);
    std::sort(ord.begin(), ord.end());
    ord.erase(std::unique(ord.begin(), ord.end()), ord.end());
    for (size_t i = 0; i < ord.size(); ++i)
        if (ord[i] == setIndex)
            return (int)i;
    return -1;
}

static SamplerBindingInfo FindSamplerBindings(const ShaderMeta& meta)
{
    SamplerBindingInfo out{};
    for (const auto& s : meta.Sets)
    {
        for (const auto& b : s.Bindings)
        {
            if (!out.hasCombined && b.Type == ShaderMetaBindingType::kCombinedImageSampler)
            {
                out.hasCombined = true;
                out.combinedSet = s.Set;
                out.combinedBinding = b.Binding;
            }
            if (!out.hasImage && b.Type == ShaderMetaBindingType::kSampledImage)
            {
                out.hasImage = true;
                out.imageSet = s.Set;
                out.imageBinding = b.Binding;
            }
            if (!out.hasSampler && b.Type == ShaderMetaBindingType::kSampler)
            {
                out.hasSampler = true;
                out.samplerSet = s.Set;
                out.samplerBinding = b.Binding;
            }
        }
    }
    return out;
}

static UboBindingInfo FindFirstUbo(const ShaderMeta& meta)
{
    UboBindingInfo out{};
    for (const auto& s : meta.Sets)
        for (const auto& b : s.Bindings)
            if (b.Type == ShaderMetaBindingType::kUniformBuffer)
            {
                out.set = s.Set;
                out.binding = b.Binding;
                out.has = b.Block.has_value();
                out.size = out.has ? b.Block->Size : 0;
                return out;
            }
    return out;
}

static UboBindingInfo FindFirstStorageBuffer(const ShaderMeta& meta)
{
    UboBindingInfo out{};
    for (const auto& s : meta.Sets)
        for (const auto& b : s.Bindings)
            if (b.Type == ShaderMetaBindingType::kStorageBuffer)
            {
                out.set = s.Set;
                out.binding = b.Binding;
                out.has = true; // "has" == found here (storage blocks may omit detailed layout)
                out.size = b.Block.has_value() ? b.Block->Size : 0;
                return out;
            }
    return out;
}

static void FillAcesUboDefaultsByName(const ShaderMeta& meta, uint32_t setIdx, uint32_t bindingIdx, void* data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0)
        return;
    // Initialize zero
    std::memset(data, 0, sizeBytes);
    // If we have block layout, fill known names first, then default remaining floats to 1.0f
    for (const auto& s : meta.Sets)
        for (const auto& b : s.Bindings)
            if (b.Type == ShaderMetaBindingType::kUniformBuffer && s.Set == setIdx && b.Binding == bindingIdx && b.Block.has_value())
            {
                const auto& blk = *b.Block;
                auto setFloats = [&](uint32_t offset, uint32_t lenBytes, float value)
                {
                    uint8_t* base = reinterpret_cast<uint8_t*>(data) + offset;
                    const uint32_t bits = *reinterpret_cast<const uint32_t*>(&value);
                    for (uint32_t off = 0; off + 4 <= lenBytes; off += 4)
                        std::memcpy(base + off, &bits, 4);
                };
                // Known names we might encounter
                const char* kExposure = "exposure";
                const char* kWhite = "white";
                const char* kGamma = "gamma";
                const char* kShoulder = "shoulder";
                const char* kToe = "toe";
                const char* kSlope = "slope";
                // First pass: set by names if detected
                for (const auto& m : blk.Members)
                {
                    std::string n = m.Name;
                    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    if (n.find(kExposure) != std::string::npos)
                        setFloats(m.Offset, m.Size, 1.0f);
                    else if (n.find(kWhite) != std::string::npos)
                        setFloats(m.Offset, m.Size, 1.0f);
                    else if (n.find(kGamma) != std::string::npos)
                        setFloats(m.Offset, m.Size, 2.2f);
                    else if (n.find(kShoulder) != std::string::npos)
                        setFloats(m.Offset, m.Size, 1.0f);
                    else if (n.find(kToe) != std::string::npos)
                        setFloats(m.Offset, m.Size, 1.0f);
                    else if (n.find(kSlope) != std::string::npos)
                        setFloats(m.Offset, m.Size, 1.0f);
                }
                // Second pass: set any remaining float slots to 1.0f as a safe default
                for (const auto& m : blk.Members)
                    setFloats(m.Offset, m.Size, 1.0f);
                return;
            }
    // No block info — uniform fallback: just set to 1.0f in 4-byte chunks
    const uint32_t one = 0x3F800000u;
    for (uint32_t off = 0; off + 4 <= sizeBytes; off += 4)
        std::memcpy((uint8_t*)data + off, &one, 4);
}
} // namespace

TEST(TonemapACES, Params_Defaults_Draw_1x1_NonZero)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true;
    dd.enableDescriptorValidation = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        GTEST_SKIP() << "Device init failed";
    }

    // 1) Prepare HDR source: clear to non-zero color
    TextureDesc hdrD{};
    hdrD.width = 1;
    hdrD.height = 1;
    hdrD.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
    hdrD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle hdr = dev->CreateTexture(hdrD);
    ASSERT_TRUE(hdr.IsValid());
    {
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(hdr, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = hdr;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.clearColorValue[0][0] = 4.0f;
        rp.clearColorValue[0][1] = 2.0f;
        rp.clearColorValue[0][2] = 1.0f;
        rp.clearColorValue[0][3] = 1.0f;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0, 0, 1, 1);
        cl->SetScissor(0, 0, 1, 1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(hdr, ResourceState::RenderTarget, ResourceState::ShaderResource));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev->ExecuteCommandLists(lists);
        dev->WaitForIdle();
    }

    // 2) LDR target
    TextureDesc ldrD{};
    ldrD.width = 1;
    ldrD.height = 1;
    ldrD.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    ldrD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle ldr = dev->CreateTexture(ldrD);
    ASSERT_TRUE(ldr.IsValid());

    // 3) Build pipeline: fullscreen_noinput VS + tonemap.frag
    auto vs = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
    auto fs = Utils::LoadShaderFile("tonemap.frag.spv");
    ASSERT_FALSE(vs.empty());
    ASSERT_FALSE(fs.empty());
    StageReflectionResult rvs{}, rfs{};
    ReflectionOptions ro{};
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()), vs.size() / 4, ro, rvs, nullptr));
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()), fs.size() / 4, ro, rfs, nullptr));
    ShaderMeta meta = MergeStages({rvs, rfs});

    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.debugName = "ACES_TM_1x1";
    MaterialBuilder::FormatsHint fh{};
    fh.ColorFormats = {(uint32_t)TextureFormat::RGBA8_UNORM};
    std::string err;
    ASSERT_TRUE(MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, fh, MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &err)) << err;
    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_NE(pipe, INVALID_HANDLE);

    // 4) Discover and bind descriptors
    auto sampInfo = FindSamplerBindings(meta);
    ASSERT_TRUE(sampInfo.hasCombined || (sampInfo.hasImage && sampInfo.hasSampler));
    auto uboInfo = FindFirstUbo(meta);

    SamplerDesc sdesc{};
    SamplerHandle sh = dev->CreateSampler(sdesc);
    ASSERT_TRUE(sh.IsValid());

    DescriptorSetHandle dsTex = INVALID_HANDLE;
    DescriptorSetHandle dsSamp = INVALID_HANDLE;
    DescriptorSetHandle dsUbo = INVALID_HANDLE;
    if (sampInfo.hasCombined)
    {
        int ord = OrdinalOf(meta, sampInfo.combinedSet);
        ASSERT_GE(ord, 0);
        DescriptorSetDesc d{};
        d.layout = pd.descriptorSetLayouts[(size_t)ord];
        d.transient = true;
        d.debugName = "ACES_TM_Combined";
        dsTex = dev->CreateDescriptorSet(d);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateCombinedImageSamplerBinding(dsTex, sampInfo.combinedBinding, hdr, sh);
    }
    else
    {
        int ordI = OrdinalOf(meta, sampInfo.imageSet);
        int ordS = OrdinalOf(meta, sampInfo.samplerSet);
        ASSERT_GE(ordI, 0);
        ASSERT_GE(ordS, 0);
        DescriptorSetDesc dI{};
        dI.layout = pd.descriptorSetLayouts[(size_t)ordI];
        dI.transient = true;
        dI.debugName = "ACES_TM_Image";
        dsTex = dev->CreateDescriptorSet(dI);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateImageBinding(dsTex, sampInfo.imageBinding, hdr);
        if (sampInfo.samplerSet == sampInfo.imageSet)
        {
            dsSamp = dsTex;
        }
        else
        {
            DescriptorSetDesc dS{};
            dS.layout = pd.descriptorSetLayouts[(size_t)ordS];
            dS.transient = true;
            dS.debugName = "ACES_TM_Sampler";
            dsSamp = dev->CreateDescriptorSet(dS);
            ASSERT_TRUE(dsSamp.IsValid());
        }
        dev->UpdateSamplerBinding(dsSamp, sampInfo.samplerBinding, sh);
    }

    if (uboInfo.has)
    {
        int ordU = OrdinalOf(meta, uboInfo.set);
        ASSERT_GE(ordU, 0);
        // Reuse existing set if UBO shares the same set
        if ((sampInfo.hasCombined && uboInfo.set == sampInfo.combinedSet) || (!sampInfo.hasCombined && uboInfo.set == sampInfo.imageSet))
            dsUbo = dsTex;
        else if (!sampInfo.hasCombined && uboInfo.set == sampInfo.samplerSet)
            dsUbo = dsSamp;
        else
        {
            DescriptorSetDesc dU{};
            dU.layout = pd.descriptorSetLayouts[(size_t)ordU];
            dU.transient = true;
            dU.debugName = "ACES_TM_UBO";
            dsUbo = dev->CreateDescriptorSet(dU);
            ASSERT_TRUE(dsUbo.IsValid());
        }
        BufferDesc ubod{};
        ubod.size = uboInfo.size;
        ubod.usage = (uint32_t)BufferUsage::Uniform;
        ubod.memoryUsage = BufferMemoryUsage::Upload;
        ubod.flags = BufferCreateFlags::PersistentlyMapped;
        BufferHandle ubo = dev->CreateBuffer(ubod);
        ASSERT_TRUE(ubo.IsValid());
        if (void* p = dev->MapBuffer(ubo))
        {
            FillAcesUboDefaultsByName(meta, uboInfo.set, uboInfo.binding, p, uboInfo.size);
            dev->UnmapBuffer(ubo);
        }
        dev->UpdateBufferBinding(dsUbo, uboInfo.binding, ubo, 0, uboInfo.size);
    }

    // tonemap.frag declares a uExposure storage buffer (auto-exposure scale) and statically
    // references it, so descriptor validation requires it bound even when useAutoExposure==0.
    // Bind a valid buffer (exposureScale=1.0) so the draw produces a sane, non-zero result
    // regardless of the generically-filled useAutoExposure push-constant value.
    auto ssboInfo = FindFirstStorageBuffer(meta);
    if (ssboInfo.has)
    {
        DescriptorSetHandle dsSsbo = INVALID_HANDLE;
        if (sampInfo.hasCombined && ssboInfo.set == sampInfo.combinedSet)
            dsSsbo = dsTex;
        else if (!sampInfo.hasCombined && ssboInfo.set == sampInfo.imageSet)
            dsSsbo = dsTex;
        else if (!sampInfo.hasCombined && ssboInfo.set == sampInfo.samplerSet)
            dsSsbo = dsSamp;
        ASSERT_TRUE(dsSsbo.IsValid()) << "uExposure expected to share a bound descriptor set";

        const uint32_t kSsboBytes = 16u; // {float exposureScale; uint valid; float pad0,pad1}
        BufferDesc sbd{};
        sbd.size = kSsboBytes;
        sbd.usage = (uint32_t)BufferUsage::Storage;
        sbd.memoryUsage = BufferMemoryUsage::Upload;
        sbd.flags = BufferCreateFlags::PersistentlyMapped;
        BufferHandle ssbo = dev->CreateBuffer(sbd);
        ASSERT_TRUE(ssbo.IsValid());
        if (void* p = dev->MapBuffer(ssbo))
        {
            const float scale = 1.0f;
            const uint32_t valid = 1u;
            std::memcpy(p, &scale, 4);
            std::memcpy(reinterpret_cast<uint8_t*>(p) + 4, &valid, 4);
            dev->UnmapBuffer(ssbo);
        }
        dev->UpdateStorageBufferBinding(dsSsbo, ssboInfo.binding, ssbo, 0, kSsboBytes);
    }

    // 5) Record draw
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rpTM{};
    rpTM.colorTargets[0] = ldr;
    rpTM.colorTargetCount = 1;
    rpTM.clearColor[0] = true;
    rpTM.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rpTM);
    cl->SetViewport(0, 0, 1, 1);
    cl->SetScissor(0, 0, 1, 1);
    cl->SetPipeline(pipe);
    if (sampInfo.hasCombined)
        cl->BindDescriptorSet(sampInfo.combinedSet, dsTex, pipe);
    else
    {
        cl->BindDescriptorSet(sampInfo.imageSet, dsTex, pipe);
        if (sampInfo.samplerSet != sampInfo.imageSet)
            cl->BindDescriptorSet(sampInfo.samplerSet, dsSamp, pipe);
    }
    if (uboInfo.has)
        cl->BindDescriptorSet(uboInfo.set, dsUbo, pipe);
    // Non-zero push constant defaults, in case shader expects them
    uint32_t pcCount = dev->GetPipelinePushConstantRangeCount(pipe);
    for (uint32_t rid = 0; rid < pcCount; ++rid)
    {
        PushConstantRangeInfo info{};
        if (dev->GetPipelinePushConstantRangeInfo(pipe, rid, info) && info.size > 0)
        {
            std::vector<uint8_t> ones(info.size, 0);
            const uint32_t kOne = 0x3F800000u;
            for (size_t off = 0; off + 4 <= ones.size(); off += 4)
                std::memcpy(ones.data() + off, &kOne, 4);
            cl->SetPushConstantsById(rid, ones.data(), ones.size(), 0);
        }
    }
    cl->Draw(3, 1);
    cl->EndRenderPass();
    cl->End();
    {
        std::vector<CommandList*> lists{cl.get()};
        dev->ExecuteCommandLists(lists);
        dev->WaitForIdle();
    }

    // 6) Readback and assert
    TextureDesc rbD{};
    rbD.width = 1;
    rbD.height = 1;
    rbD.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    rbD.usage = (uint32_t)TextureUsage::TransferDst; // just for nomenclature
    BufferHandle readback = dev->CreateReadbackBuffer(4);
    {
        auto cl2 = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl2->Begin();
        cl2->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::RenderTarget, ResourceState::CopySource));
        cl2->CopyTextureToBuffer(ldr, readback, 1, 1, 0, 0, 0, 0);
        cl2->End();
        std::vector<CommandList*> lists{cl2.get()};
        dev->ExecuteCommandLists(lists);
        dev->WaitForIdle();
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(dev->MapBuffer(readback));
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p[0] > 0 || p[1] > 0 || p[2] > 0) << "Expected non-zero RGB output after ACES tonemap";
    dev->UnmapBuffer(readback);
}
