#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "Source/Vulkan/VulkanDevice.h"

#include <Rendering/Common/Utils.h>
#include "TestUtils.h"
#include <Rendering/Core/CommandList.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Materials/MaterialBuilder.h>
#include <Rendering/Materials/MaterialHelper.h>
#include <Rendering/Materials/ShaderReflection.h>

using namespace GameEngine::Rendering;

namespace
{
struct PerFramePC
{
    float cameraPos[3];
    float exposure; // defaults to 1.0
};
} // namespace

TEST(PBRForward, EndToEnd_ComputeLUT_Draw_Tonemap_Readback)
{
    // Headless friendly
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{};
    dd.applicationName = "PBRForwardEndToEnd";
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true;
    dd.enableDescriptorValidation = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        GTEST_SKIP() << "Device init failed";
    }

    // =====================================================================================
    // 1) Generate BRDF LUT via compute shader into RG16F 256x256 storage image
    // =====================================================================================
    const uint32_t kLutSize = 256;
    TextureDesc lutDesc{};
    lutDesc.width = kLutSize;
    lutDesc.height = kLutSize;
    lutDesc.format = (uint32_t)TextureFormat::R16G16_FLOAT;
    lutDesc.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
    lutDesc.debugName = "BRDF_LUT";
    TextureHandle brdfLut = dev->CreateTexture(lutDesc);
    ASSERT_TRUE(brdfLut.IsValid());

    std::vector<uint8_t> csBRDF = Utils::LoadShaderFile("brdf_lut.comp.spv");
    ASSERT_FALSE(csBRDF.empty());

    DescriptorSetLayoutDesc csLayout{};
    DescriptorBinding csBind{};
    csBind.binding = 0;
    csBind.type = DescriptorType::StorageImage;
    csBind.count = 1;
    csBind.shaderStages = VK_SHADER_STAGE_COMPUTE_BIT;
    csLayout.bindings.push_back(csBind);

    PipelineDesc csPipe{};
    csPipe.type = PipelineType::Compute;
    csPipe.computeShader = csBRDF;
    csPipe.descriptorSetLayouts.push_back(csLayout);
    csPipe.debugName = "BRDF_LUT_CS";
    PipelineHandle cs = dev->CreatePipeline(csPipe);
    ASSERT_NE(cs, INVALID_HANDLE);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = csLayout;
    dsDesc.transient = true;
    dsDesc.debugName = "BRDF_LUT_DS";
    DescriptorSetHandle csSet = dev->CreateDescriptorSet(dsDesc);
    ASSERT_TRUE(csSet.IsValid());
    dev->UpdateStorageImageBinding(csSet, 0, brdfLut);

    auto clc = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE((bool)clc);
    clc->Begin();
    // Ensure LUT is in UAV before dispatch
    clc->Barrier(ResourceBarrier::CreateTextureBarrier(brdfLut, ResourceState::Undefined, ResourceState::UnorderedAccess));
    clc->SetPipeline(cs);
    // Bind descriptor set using typed handles
    clc->BindDescriptorSet(0, csSet, cs);
    const uint32_t gx = (kLutSize + 15) / 16, gy = (kLutSize + 15) / 16;
    clc->Dispatch(gx, gy, 1);
    // Transition LUT to SRV for sampling in graphics pass
    clc->Barrier(ResourceBarrier::CreateTextureBarrier(brdfLut, ResourceState::UnorderedAccess, ResourceState::ShaderResource));
    clc->End();
    std::vector<CommandList*> listsC{clc.get()};
    dev->ExecuteCommandLists(listsC);

    // =====================================================================================
    // 2) Create small 1x1 textures for PBR inputs by clearing them via render pass
    //    Albedo (sRGB), Normal (linear), MetallicRoughness (linear RG), AO (linear R)
    // =====================================================================================
    TextureDesc albedoD{};
    albedoD.width = 1;
    albedoD.height = 1;
    albedoD.format = (uint32_t)TextureFormat::RGBA8_SRGB;
    albedoD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    albedoD.debugName = "Albedo_sRGB_1x1";
    TextureDesc normalD{};
    normalD.width = 1;
    normalD.height = 1;
    normalD.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    normalD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    normalD.debugName = "Normal_1x1";
    TextureDesc mrd{};
    mrd.width = 1;
    mrd.height = 1;
    mrd.format = (uint32_t)TextureFormat::R8G8_UNORM;
    mrd.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    mrd.debugName = "MR_1x1";
    TextureDesc aoD{};
    aoD.width = 1;
    aoD.height = 1;
    aoD.format = (uint32_t)TextureFormat::R8_UNORM;
    aoD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    aoD.debugName = "AO_1x1";

    TextureHandle albedo = dev->CreateTexture(albedoD);
    TextureHandle normal = dev->CreateTexture(normalD);
    TextureHandle mr = dev->CreateTexture(mrd);
    TextureHandle ao = dev->CreateTexture(aoD);
    ASSERT_TRUE(albedo.IsValid() && normal.IsValid() && mr.IsValid() && ao.IsValid());

    // Clear all four in a single multi-RT pass (values: albedo=0.5 gray, normal=(0.5,0.5,1,1), MR=(0.0,0.5,0,1), AO=1)
    auto clFill = dev->CreateCommandList(IDevice::QueueType::Graphics);
    clFill->Begin();
    RenderPassDesc rpFill{};
    rpFill.colorTargets[0] = albedo;
    rpFill.colorTargets[1] = normal;
    rpFill.colorTargets[2] = mr;
    rpFill.colorTargets[3] = ao;
    rpFill.colorTargetCount = 4;
    rpFill.clearColor[0] = true;
    rpFill.clearColorValue[0][0] = 0.5f;
    rpFill.clearColorValue[0][1] = 0.5f;
    rpFill.clearColorValue[0][2] = 0.5f;
    rpFill.clearColorValue[0][3] = 1.0f;
    rpFill.clearColor[1] = true;
    rpFill.clearColorValue[1][0] = 0.5f;
    rpFill.clearColorValue[1][1] = 0.5f;
    rpFill.clearColorValue[1][2] = 1.0f;
    rpFill.clearColorValue[1][3] = 1.0f;
    rpFill.clearColor[2] = true;
    rpFill.clearColorValue[2][0] = 0.0f;
    rpFill.clearColorValue[2][1] = 0.5f;
    rpFill.clearColorValue[2][2] = 0.0f;
    rpFill.clearColorValue[2][3] = 1.0f;
    rpFill.clearColor[3] = true;
    rpFill.clearColorValue[3][0] = 1.0f;
    rpFill.clearColorValue[3][1] = 1.0f;
    rpFill.clearColorValue[3][2] = 1.0f;
    rpFill.clearColorValue[3][3] = 1.0f;
    // Ensure content is preserved after clears
    rpFill.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    rpFill.colorStoreOp[1] = RenderPassDesc::StoreOp::Store;
    rpFill.colorStoreOp[2] = RenderPassDesc::StoreOp::Store;
    rpFill.colorStoreOp[3] = RenderPassDesc::StoreOp::Store;

    // Explicitly transition to RenderTarget before beginning render pass (no implicit transitions)
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(albedo, ResourceState::Undefined, ResourceState::RenderTarget));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(normal, ResourceState::Undefined, ResourceState::RenderTarget));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(mr, ResourceState::Undefined, ResourceState::RenderTarget));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(ao, ResourceState::Undefined, ResourceState::RenderTarget));

    clFill->BeginRenderPass(rpFill);
    clFill->EndRenderPass();
    // Transition to SRV for sampling in PBR
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(albedo, ResourceState::RenderTarget, ResourceState::ShaderResource));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(normal, ResourceState::RenderTarget, ResourceState::ShaderResource));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(mr, ResourceState::RenderTarget, ResourceState::ShaderResource));
    clFill->Barrier(ResourceBarrier::CreateTextureBarrier(ao, ResourceState::RenderTarget, ResourceState::ShaderResource));
    clFill->End();
    std::vector<CommandList*> listsF{clFill.get()};
    dev->ExecuteCommandLists(listsF);

    // =====================================================================================
    // 3) PBR forward pass -> offscreen HDR RGBA16F
    // =====================================================================================
    TextureDesc hdrD{};
    hdrD.width = 128;
    hdrD.height = 128;
    hdrD.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
    hdrD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    hdrD.debugName = "HDR_128";
    TextureHandle hdr = dev->CreateTexture(hdrD);
    ASSERT_TRUE(hdr.IsValid());

    // Reflection-driven minimal draw: sample albedo into HDR via fullscreen FS (sample_texture)
    {
        // Load shaders
        std::vector<uint8_t> vsDraw = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
        std::vector<uint8_t> fsDraw = Utils::LoadShaderFile("sample_texture.frag.spv");
        ASSERT_FALSE(vsDraw.empty());
        ASSERT_FALSE(fsDraw.empty());
        // Reflect VS+FS
        StageReflectionResult rvs{};
        StageReflectionResult rfs{};
        ReflectionOptions ro{};
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vsDraw.data()), vsDraw.size() / 4, ro, rvs, nullptr));
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fsDraw.data()), fsDraw.size() / 4, ro, rfs, nullptr));
        ShaderMeta meta = MergeStages({rvs, rfs});
        // Build pipeline from meta with RG16F color
        PipelineDesc pd{};
        pd.type = PipelineType::Graphics;
        pd.vertexShader = vsDraw;
        pd.pixelShader = fsDraw;
        pd.debugName = "Draw_SampleTex_to_HDR";
        MaterialBuilder::FormatsHint fh{};
        fh.ColorFormats = {(uint32_t)TextureFormat::R16G16B16A16_FLOAT};
        std::string err;
        bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, fh, MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &err);
        ASSERT_TRUE(ok) << err;
        // Find CombinedImageSampler from meta (set/binding)
        uint32_t drawSetIndex = 0, drawBinding = 0;
        bool drawFound = false;
        for (const auto& s : meta.Sets)
        {
            for (const auto& b : s.Bindings)
            {
                if (b.Type == ShaderMetaBindingType::kCombinedImageSampler)
                {
                    drawSetIndex = s.Set;
                    drawBinding = b.Binding;
                    drawFound = true;
                    break;
                }
            }
            if (drawFound)
                break;
        }
        ASSERT_TRUE(drawFound);
        // Compute layout ordinal for that set (MaterialBuilder orders layouts by ascending set index)
        std::vector<uint32_t> sets;
        for (const auto& s : meta.Sets)
            sets.push_back(s.Set);
        std::sort(sets.begin(), sets.end());
        sets.erase(std::unique(sets.begin(), sets.end()), sets.end());
        int layoutOrdinal = -1;
        for (size_t i = 0; i < sets.size(); ++i)
            if (sets[i] == drawSetIndex)
            {
                layoutOrdinal = (int)i;
                break;
            }
        ASSERT_GE(layoutOrdinal, 0);
        PipelineHandle drawPipe = dev->CreatePipeline(pd);
        ASSERT_NE(drawPipe, INVALID_HANDLE);
        // Descriptor set from reflection-derived layout
        DescriptorSetDesc drawDSDesc{};
        drawDSDesc.layout = pd.descriptorSetLayouts[(size_t)layoutOrdinal];
        drawDSDesc.transient = true;
        drawDSDesc.debugName = "DrawSet";
        DescriptorSetHandle drawSet = dev->CreateDescriptorSet(drawDSDesc);
        ASSERT_TRUE(drawSet.IsValid());
        // Create a basic sampler and bind according to meta
        SamplerDesc samp{};
        SamplerHandle shDraw = dev->CreateSampler(samp);
        ASSERT_TRUE(shDraw.IsValid());
        // Bind albedo texture + sampler at reflected binding
        dev->UpdateCombinedImageSamplerBinding(drawSet, drawBinding, albedo, shDraw);
        // Record pass
        auto clHdr = dev->CreateCommandList(IDevice::QueueType::Graphics);
        clHdr->Begin();
        clHdr->Barrier(ResourceBarrier::CreateTextureBarrier(hdr, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = hdr;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = false;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        clHdr->BeginRenderPass(rp);
        clHdr->SetViewport(0, 0, 128, 128);
        clHdr->SetScissor(0, 0, 128, 128);
        clHdr->SetPipeline(drawPipe);
        clHdr->BindDescriptorSet(drawSetIndex, drawSet, drawPipe);
        // Ensure push constants are initialized if required by the shader (use per-range API for correct stage flags)
        uint32_t rangeCountDraw = dev->GetPipelinePushConstantRangeCount(drawPipe);
        for (uint32_t rid = 0; rid < rangeCountDraw; ++rid)
        {
            GameEngine::Rendering::PushConstantRangeInfo info{};
            if (dev->GetPipelinePushConstantRangeInfo(drawPipe, rid, info) && info.size > 0)
            {
                std::vector<uint8_t> zero(info.size, 0);
                clHdr->SetPushConstantsById(rid, zero.data(), zero.size(), 0);
            }
        }
        clHdr->Draw(3, 1);
        clHdr->EndRenderPass();
        // Transition HDR to SRV for tonemap pass
        clHdr->Barrier(ResourceBarrier::CreateTextureBarrier(hdr, ResourceState::RenderTarget, ResourceState::ShaderResource));
        clHdr->End();
        std::vector<CommandList*> listsHdr{clHdr.get()};
        dev->ExecuteCommandLists(listsHdr);
    }

    // =====================================================================================
    // 4) Tonemap pass: HDR -> LDR RGBA8
    // =====================================================================================
    TextureDesc ldrD{};
    ldrD.width = 128;
    ldrD.height = 128;
    ldrD.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    ldrD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc);
    ldrD.debugName = "LDR_128";
    TextureHandle ldr = dev->CreateTexture(ldrD);
    ASSERT_TRUE(ldr.IsValid());

    // Reflection-driven ACES tonemap: HDR -> LDR
    std::vector<uint8_t> vsFS = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
    std::vector<uint8_t> fsTM = Utils::LoadShaderFile("tonemap.frag.spv");
    ASSERT_FALSE(vsFS.empty());
    ASSERT_FALSE(fsTM.empty());
    StageReflectionResult rvsTM{};
    StageReflectionResult rfsTM{};
    ReflectionOptions ro{};
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vsFS.data()), vsFS.size() / 4, ro, rvsTM, nullptr));
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fsTM.data()), fsTM.size() / 4, ro, rfsTM, nullptr));
    ShaderMeta tmMeta = MergeStages({rvsTM, rfsTM});
    PipelineDesc tm{};
    tm.type = PipelineType::Graphics;
    tm.vertexShader = vsFS;
    tm.pixelShader = fsTM;
    tm.debugName = "TonemapACES";
    MaterialBuilder::FormatsHint tmF{};
    tmF.ColorFormats = {(uint32_t)TextureFormat::RGBA8_UNORM};
    std::string tmErr;
    bool tmOk = MaterialBuilder::BuildPipelineDescFromMeta(tmMeta, tm, tmF, MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &tmErr);
    ASSERT_TRUE(tmOk) << tmErr;

    // Discover bindings: CombinedImageSampler or separate SampledImage+Sampler for HDR, and optional UBO for ACES params
    uint32_t tmCombinedSet = 0, tmCombinedBinding = 0;
    bool hasCombined = false;
    uint32_t tmImageSet = 0, tmImageBinding = 0;
    bool hasImage = false;
    uint32_t tmSamplerSet = 0, tmSamplerBinding = 0;
    bool hasSampler = false;
    uint32_t tmUboSet = 0, tmUboBinding = 0;
    uint32_t tmUboSize = 0;
    bool tmUboFound = false;
    for (const auto& s : tmMeta.Sets)
    {
        for (const auto& b : s.Bindings)
        {
            if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && !hasCombined)
            {
                tmCombinedSet = s.Set;
                tmCombinedBinding = b.Binding;
                hasCombined = true;
            }
            if (b.Type == ShaderMetaBindingType::kSampledImage && !hasImage)
            {
                tmImageSet = s.Set;
                tmImageBinding = b.Binding;
                hasImage = true;
            }
            if (b.Type == ShaderMetaBindingType::kSampler && !hasSampler)
            {
                tmSamplerSet = s.Set;
                tmSamplerBinding = b.Binding;
                hasSampler = true;
            }
            if (b.Type == ShaderMetaBindingType::kUniformBuffer && !tmUboFound)
            {
                tmUboSet = s.Set;
                tmUboBinding = b.Binding;
                tmUboFound = b.Block.has_value();
                if (tmUboFound)
                    tmUboSize = b.Block->Size;
            }
        }
    }
    ASSERT_TRUE(hasCombined || (hasImage && hasSampler));

    // Compute layout ordinals for involved sets
    std::vector<uint32_t> tmSets;
    for (const auto& s : tmMeta.Sets)
        tmSets.push_back(s.Set);
    std::sort(tmSets.begin(), tmSets.end());
    tmSets.erase(std::unique(tmSets.begin(), tmSets.end()), tmSets.end());
    auto OrdinalOf = [&](uint32_t setIndex)
    { for (size_t i=0;i<tmSets.size();++i) if (tmSets[i]==setIndex) return (int)i; return -1; };
    int ordCombined = hasCombined ? OrdinalOf(tmCombinedSet) : -1;
    if (hasCombined)
        ASSERT_GE(ordCombined, 0);
    int ordImage = hasImage ? OrdinalOf(tmImageSet) : -1;
    int ordSampler = hasSampler ? OrdinalOf(tmSamplerSet) : -1;
    int ordUbo = tmUboFound ? OrdinalOf(tmUboSet) : -1;

    PipelineHandle tmPipe = dev->CreatePipeline(tm);
    ASSERT_NE(tmPipe, INVALID_HANDLE);
    SamplerDesc tmSamp{};
    SamplerHandle sh = dev->CreateSampler(tmSamp);
    ASSERT_TRUE(sh.IsValid());

    // Create descriptor set(s) for texture+sampler
    DescriptorSetHandle dsTex = INVALID_HANDLE;
    DescriptorSetHandle dsSamp = INVALID_HANDLE;
    if (hasCombined)
    {
        DescriptorSetDesc d{};
        d.layout = tm.descriptorSetLayouts[(size_t)ordCombined];
        d.transient = true;
        d.debugName = "TMSet_Combined";
        dsTex = dev->CreateDescriptorSet(d);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateCombinedImageSamplerBinding(dsTex, tmCombinedBinding, hdr, sh);
    }
    else
    {
        // Sampled image
        DescriptorSetDesc dI{};
        dI.layout = tm.descriptorSetLayouts[(size_t)ordImage];
        dI.transient = true;
        dI.debugName = "TMSet_Image";
        dsTex = dev->CreateDescriptorSet(dI);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateImageBinding(dsTex, tmImageBinding, hdr);
        // Sampler (may be same set or different)
        if (tmSamplerSet == tmImageSet)
        {
            dsSamp = dsTex;
        }
        else
        {
            DescriptorSetDesc dS{};
            dS.layout = tm.descriptorSetLayouts[(size_t)ordSampler];
            dS.transient = true;
            dS.debugName = "TMSet_Sampler";
            dsSamp = dev->CreateDescriptorSet(dS);
            ASSERT_TRUE(dsSamp.IsValid());
        }
        dev->UpdateSamplerBinding(dsSamp, tmSamplerBinding, sh);
    }

    // Optional UBO
    DescriptorSetHandle dsUbo = INVALID_HANDLE;
    if (tmUboFound)
    {
        if ((hasCombined && tmUboSet == tmCombinedSet) || (!hasCombined && tmUboSet == tmImageSet))
        {
            dsUbo = dsTex;
        }
        else if (!hasCombined && tmUboSet == tmSamplerSet)
        {
            dsUbo = dsSamp;
        }
        else
        {
            DescriptorSetDesc d{};
            d.layout = tm.descriptorSetLayouts[(size_t)ordUbo];
            d.transient = true;
            d.debugName = "TMSet_UBO";
            dsUbo = dev->CreateDescriptorSet(d);
            ASSERT_TRUE(dsUbo.IsValid());
        }
        BufferDesc uboDesc{};
        uboDesc.size = tmUboSize;
        uboDesc.usage = (uint32_t)BufferUsage::Uniform;
        uboDesc.memoryUsage = BufferMemoryUsage::Upload;
        uboDesc.flags = BufferCreateFlags::PersistentlyMapped;
        BufferHandle ubo = dev->CreateBuffer(uboDesc);
        ASSERT_TRUE(ubo.IsValid());
        if (void* p = dev->MapBuffer(ubo))
        {
            std::memset(p, 0, tmUboSize);
            for (const auto& s : tmMeta.Sets)
                for (const auto& b : s.Bindings)
                    if (b.Type == ShaderMetaBindingType::kUniformBuffer && s.Set == tmUboSet && b.Binding == tmUboBinding && b.Block.has_value())
                    {
                        const auto& blk = *b.Block;
                        const uint32_t one = 0x3F800000u;
                        for (const auto& m : blk.Members)
                        {
                            uint8_t* base = (uint8_t*)p + m.Offset;
                            for (uint32_t off = 0; off + 4 <= m.Size; off += 4)
                                std::memcpy(base + off, &one, 4);
                        }
                        break;
                    }
            dev->UnmapBuffer(ubo);
        }
        dev->UpdateBufferBinding(dsUbo, tmUboBinding, ubo, 0, tmUboSize);
    }

    RenderPassDesc rpTM{};
    rpTM.colorTargets[0] = ldr;
    rpTM.colorTargetCount = 1;
    rpTM.clearColor[0] = true;
    rpTM.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    // Transition LDR to RenderTarget before tonemap pass
    auto clTM = dev->CreateCommandList(IDevice::QueueType::Graphics);
    clTM->Begin();
    clTM->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::Undefined, ResourceState::RenderTarget));

    clTM->BeginRenderPass(rpTM);
    // Set dynamic viewport and scissor for full target
    clTM->SetViewport(0, 0, 128, 128);
    clTM->SetScissor(0, 0, 128, 128);

    clTM->SetPipeline(tmPipe);
    // Bind texture/sampler sets and (if present) UBO set
    if (hasCombined)
    {
        clTM->BindDescriptorSet(tmCombinedSet, dsTex, tmPipe);
    }
    else
    {
        clTM->BindDescriptorSet(tmImageSet, dsTex, tmPipe);
        if (tmSamplerSet != tmImageSet)
            clTM->BindDescriptorSet(tmSamplerSet, dsSamp, tmPipe);
    }
    if (tmUboFound)
    {
        // If dsUbo equals an already-bound set handle, this is a no-op logically; binding same set twice is harmless
        clTM->BindDescriptorSet(tmUboSet, dsUbo, tmPipe);
    }
    // Initialize push constants if the shader expects them (use per-range API)
    uint32_t rangeCountTM = dev->GetPipelinePushConstantRangeCount(tmPipe);
    for (uint32_t rid = 0; rid < rangeCountTM; ++rid)
    {
        GameEngine::Rendering::PushConstantRangeInfo info{};
        if (dev->GetPipelinePushConstantRangeInfo(tmPipe, rid, info) && info.size > 0)
        {
            std::vector<uint8_t> ones(info.size, 0);
            const uint32_t kOne = 0x3F800000u; // 1.0f
            for (size_t off = 0; off + 4 <= ones.size(); off += 4)
                std::memcpy(ones.data() + off, &kOne, 4);
            clTM->SetPushConstantsById(rid, ones.data(), ones.size(), 0);
        }
    }
    clTM->Draw(3, 1);
    clTM->EndRenderPass();

    // Prepare for readback: transition LDR to CopySource and copy to readback buffer
    clTM->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::RenderTarget, ResourceState::CopySource));

    // Readback
    const uint32_t rbSize = 128u * 128u * 4u; // RGBA8
    BufferHandle rb = dev->CreateReadbackBuffer(rbSize);
    ASSERT_TRUE(rb.IsValid());
    clTM->CopyTextureToBuffer(ldr, rb, 128, 128);

    clTM->End();
    std::vector<CommandList*> listsTM{clTM.get()};
    dev->ExecuteCommandLists(listsTM);
    dev->WaitForIdle();

    // Validate a few pixels are non-zero (tonemap result)
    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y)
    { return (y * 128u + x) * 4u; };
    auto expectNonZero = [&](uint32_t x, uint32_t y)
    {
        size_t i = idx(x, y);
        EXPECT_TRUE(data[i + 0] > 0 || data[i + 1] > 0 || data[i + 2] > 0);
    };
    expectNonZero(0, 0);
    expectNonZero(64, 64);
    expectNonZero(127, 127);
    dev->UnmapBuffer(rb);

    // Cleanup of transient/owned resources is handled by device on shutdown; destroy the explicit ones to avoid leak assertions
    dev->DestroyBuffer(rb);
    dev->DestroySampler(sh);
    dev->DestroyTexture(ldr);
    dev->DestroyTexture(hdr);
    dev->DestroyTexture(ao);
    dev->DestroyTexture(mr);
    dev->DestroyTexture(normal);
    dev->DestroyTexture(albedo);
    dev->DestroyTexture(brdfLut);
}

TEST(PBRForward, EndToEnd_Tonemap_FromClearedHDR)
{
    using namespace GameEngine::Rendering;
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true;
    dd.enableDescriptorValidation = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // HDR offscreen target (will be cleared to bright HDR color and sampled)
    TextureDesc hdr{};
    hdr.width = 128;
    hdr.height = 128;
    hdr.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
    hdr.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle hdrTex = dev->CreateTexture(hdr);
    ASSERT_TRUE(hdrTex.IsValid());

    // Clear HDR to a non-zero bright color and store
    {
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(hdrTex, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{};
        rp.colorTargets[0] = hdrTex;
        rp.colorTargetCount = 1;
        rp.clearColor[0] = true;
        rp.clearColorValue[0][0] = 4.0f;
        rp.clearColorValue[0][1] = 2.0f;
        rp.clearColorValue[0][2] = 1.0f;
        rp.clearColorValue[0][3] = 1.0f;
        rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0, 0, 128, 128);
        cl->SetScissor(0, 0, 128, 128);
        cl->EndRenderPass();
        // Prepare for sampling
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(hdrTex, ResourceState::RenderTarget, ResourceState::ShaderResource));
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev->ExecuteCommandLists(lists);
        dev->WaitForIdle();
    }

    // LDR target for tonemap output
    TextureDesc ldrD{};
    ldrD.width = 128;
    ldrD.height = 128;
    ldrD.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    ldrD.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc);
    TextureHandle ldr = dev->CreateTexture(ldrD);
    ASSERT_TRUE(ldr.IsValid());

    // Tonemap pipeline (fullscreen no-input VS + ACES FS), reflection-driven
    std::vector<uint8_t> vsFS = Utils::LoadShaderFile("fullscreen_noinput.vert.spv");
    std::vector<uint8_t> fsTM = Utils::LoadShaderFile("tonemap.frag.spv");
    ASSERT_FALSE(vsFS.empty());
    ASSERT_FALSE(fsTM.empty());
    StageReflectionResult rvs{};
    StageReflectionResult rfs{};
    ReflectionOptions ro{};
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vsFS.data()), vsFS.size() / 4, ro, rvs, nullptr));
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fsTM.data()), fsTM.size() / 4, ro, rfs, nullptr));
    ShaderMeta meta = MergeStages({rvs, rfs});
    PipelineDesc tm{};
    tm.type = PipelineType::Graphics;
    tm.vertexShader = vsFS;
    tm.pixelShader = fsTM;
    MaterialBuilder::FormatsHint fh{};
    fh.ColorFormats = {(uint32_t)TextureFormat::RGBA8_UNORM};
    std::string err;
    bool ok = MaterialBuilder::BuildPipelineDescFromMeta(meta, tm, fh, MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &err);
    ASSERT_TRUE(ok) << err;
    PipelineHandle tmPipe = dev->CreatePipeline(tm);
    ASSERT_NE(tmPipe, INVALID_HANDLE);

    // Discover bindings: CombinedImageSampler or separate SampledImage+Sampler for HDR, and optional UBO for ACES params
    uint32_t combinedSet = 0, combinedBinding = 0;
    bool hasCombined = false;
    uint32_t imageSet = 0, imageBinding = 0;
    bool hasImage = false;
    uint32_t samplerSet = 0, samplerBinding = 0;
    bool hasSampler = false;
    uint32_t uboSet = 0, uboBinding = 0;
    uint32_t uboSize = 0;
    bool hasUbo = false;
    for (const auto& s : meta.Sets)
    {
        for (const auto& b : s.Bindings)
        {
            if (b.Type == ShaderMetaBindingType::kCombinedImageSampler && !hasCombined)
            {
                combinedSet = s.Set;
                combinedBinding = b.Binding;
                hasCombined = true;
            }
            if (b.Type == ShaderMetaBindingType::kSampledImage && !hasImage)
            {
                imageSet = s.Set;
                imageBinding = b.Binding;
                hasImage = true;
            }
            if (b.Type == ShaderMetaBindingType::kSampler && !hasSampler)
            {
                samplerSet = s.Set;
                samplerBinding = b.Binding;
                hasSampler = true;
            }
            if (b.Type == ShaderMetaBindingType::kUniformBuffer && !hasUbo)
            {
                uboSet = s.Set;
                uboBinding = b.Binding;
                hasUbo = b.Block.has_value();
                if (hasUbo)
                    uboSize = b.Block->Size;
            }
        }
    }
    ASSERT_TRUE(hasCombined || (hasImage && hasSampler));
    std::vector<uint32_t> ordSets;
    for (const auto& s : meta.Sets)
        ordSets.push_back(s.Set);
    std::sort(ordSets.begin(), ordSets.end());
    ordSets.erase(std::unique(ordSets.begin(), ordSets.end()), ordSets.end());
    auto OrdinalOf = [&](uint32_t setIndex)
    { for (size_t i=0;i<ordSets.size();++i) if (ordSets[i]==setIndex) return (int)i; return -1; };
    int ordCombined = hasCombined ? OrdinalOf(combinedSet) : -1;
    if (hasCombined)
        ASSERT_GE(ordCombined, 0);
    int ordImage = hasImage ? OrdinalOf(imageSet) : -1;
    int ordSampler = hasSampler ? OrdinalOf(samplerSet) : -1;
    int ordUbo = hasUbo ? OrdinalOf(uboSet) : -1;

    // Create descriptor set(s) and update
    SamplerDesc samp{};
    SamplerHandle sh = dev->CreateSampler(samp);
    ASSERT_TRUE(sh.IsValid());
    DescriptorSetHandle dsTex = INVALID_HANDLE;
    DescriptorSetHandle dsSamp = INVALID_HANDLE;
    if (hasCombined)
    {
        DescriptorSetDesc d{};
        d.layout = tm.descriptorSetLayouts[(size_t)ordCombined];
        d.transient = true;
        d.debugName = "TM2_Combined";
        dsTex = dev->CreateDescriptorSet(d);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateCombinedImageSamplerBinding(dsTex, combinedBinding, hdrTex, sh);
    }
    else
    {
        DescriptorSetDesc dI{};
        dI.layout = tm.descriptorSetLayouts[(size_t)ordImage];
        dI.transient = true;
        dI.debugName = "TM2_Image";
        dsTex = dev->CreateDescriptorSet(dI);
        ASSERT_TRUE(dsTex.IsValid());
        dev->UpdateImageBinding(dsTex, imageBinding, hdrTex);
        if (samplerSet == imageSet)
        {
            dsSamp = dsTex;
        }
        else
        {
            DescriptorSetDesc dS{};
            dS.layout = tm.descriptorSetLayouts[(size_t)ordSampler];
            dS.transient = true;
            dS.debugName = "TM2_Sampler";
            dsSamp = dev->CreateDescriptorSet(dS);
            ASSERT_TRUE(dsSamp.IsValid());
        }
        dev->UpdateSamplerBinding(dsSamp, samplerBinding, sh);
    }

    DescriptorSetHandle dsUbo = INVALID_HANDLE;
    if (hasUbo)
    {
        if ((hasCombined && uboSet == combinedSet) || (!hasCombined && uboSet == imageSet))
        {
            dsUbo = dsTex;
        }
        else if (!hasCombined && uboSet == samplerSet)
        {
            dsUbo = dsSamp;
        }
        else
        {
            DescriptorSetDesc d{};
            d.layout = tm.descriptorSetLayouts[(size_t)ordUbo];
            d.transient = true;
            d.debugName = "TM2_UBO";
            dsUbo = dev->CreateDescriptorSet(d);
            ASSERT_TRUE(dsUbo.IsValid());
        }
        BufferDesc uboDesc{};
        uboDesc.size = uboSize;
        uboDesc.usage = (uint32_t)BufferUsage::Uniform;
        uboDesc.memoryUsage = BufferMemoryUsage::Upload;
        uboDesc.flags = BufferCreateFlags::PersistentlyMapped;
        BufferHandle ubo = dev->CreateBuffer(uboDesc);
        ASSERT_TRUE(ubo.IsValid());
        if (void* p = dev->MapBuffer(ubo))
        {
            std::memset(p, 0, uboSize);
            for (const auto& s : meta.Sets)
                for (const auto& b : s.Bindings)
                    if (b.Type == ShaderMetaBindingType::kUniformBuffer && s.Set == uboSet && b.Binding == uboBinding && b.Block.has_value())
                    {
                        const auto& blk = *b.Block;
                        const uint32_t one = 0x3F800000u;
                        for (const auto& m : blk.Members)
                        {
                            uint8_t* base = (uint8_t*)p + m.Offset;
                            for (uint32_t off = 0; off + 4 <= m.Size; off += 4)
                                std::memcpy(base + off, &one, 4);
                        }
                        break;
                    }
            dev->UnmapBuffer(ubo);
        }
        dev->UpdateBufferBinding(dsUbo, uboBinding, ubo, 0, uboSize);
    }

    // Record tonemap pass
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rpTM{};
    rpTM.colorTargets[0] = ldr;
    rpTM.colorTargetCount = 1;
    rpTM.clearColor[0] = true;
    rpTM.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rpTM);
    cl->SetViewport(0, 0, 128, 128);
    cl->SetScissor(0, 0, 128, 128);
    cl->SetPipeline(tmPipe);
    if (hasCombined)
    {
        cl->BindDescriptorSet(combinedSet, dsTex, tmPipe);
    }
    else
    {
        cl->BindDescriptorSet(imageSet, dsTex, tmPipe);
        if (samplerSet != imageSet)
            cl->BindDescriptorSet(samplerSet, dsSamp, tmPipe);
    }
    if (hasUbo)
    {
        cl->BindDescriptorSet(uboSet, dsUbo, tmPipe);
    }
    // Initialize push constants if required (use per-range API)
    uint32_t rangeCountTM2 = dev->GetPipelinePushConstantRangeCount(tmPipe);
    for (uint32_t rid = 0; rid < rangeCountTM2; ++rid)
    {
        GameEngine::Rendering::PushConstantRangeInfo info{};
        if (dev->GetPipelinePushConstantRangeInfo(tmPipe, rid, info) && info.size > 0)
        {
            std::vector<uint8_t> ones(info.size, 0);
            const uint32_t kOne = 0x3F800000u; // 1.0f
            for (size_t off = 0; off + 4 <= ones.size(); off += 4)
                std::memcpy(ones.data() + off, &kOne, 4);
            cl->SetPushConstantsById(rid, ones.data(), ones.size(), 0);
        }
    }
    cl->Draw(3, 1);
    cl->EndRenderPass();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(ldr, ResourceState::RenderTarget, ResourceState::CopySource));

    // Readback
    const uint32_t rbSize = 128u * 128u * 4u;
    BufferHandle rb = dev->CreateReadbackBuffer(rbSize);
    ASSERT_TRUE(rb.IsValid());
    cl->CopyTextureToBuffer(ldr, rb, 128, 128);
    cl->End();
    std::vector<CommandList*> lists2{cl.get()};
    dev->ExecuteCommandLists(lists2);
    dev->WaitForIdle();

    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y)
    { return (y * 128u + x) * 4u; };
    auto expectNonZero = [&](uint32_t x, uint32_t y)
    { size_t i = idx(x,y); EXPECT_TRUE(data[i+0] > 0 || data[i+1] > 0 || data[i+2] > 0); };
    expectNonZero(0, 0);
    expectNonZero(64, 64);
    expectNonZero(127, 127);
    dev->UnmapBuffer(rb);

    dev->DestroyBuffer(rb);
    dev->DestroySampler(sh);
    dev->DestroyTexture(ldr);
    dev->DestroyTexture(hdrTex);
}
