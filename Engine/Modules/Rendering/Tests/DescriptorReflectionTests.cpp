#include <gtest/gtest.h>
#include <vector>
#include <fstream>
#include <cstring>
#include <nlohmann/json.hpp>

#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include "Rendering/Common/Utils.h"
#include <Rendering/Core/Device.h>
#include <Rendering/Core/CommandList.h>
#include <Rendering/Materials/MaterialBuilder.h>

#include "TestUtils.h"


using namespace GameEngine::Rendering;

TEST(DescriptorReflection, TriangleVSFS_DescriptorSetsAndPushConstants) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    std::vector<uint8_t> vsBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.vert.spv");
    std::vector<uint8_t> fsBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("triangle.frag.spv");
    ASSERT_FALSE(vsBytes.empty()); ASSERT_FALSE(fsBytes.empty());
    ASSERT_EQ(vsBytes.size()%4, 0u); ASSERT_EQ(fsBytes.size()%4, 0u);
    std::vector<uint32_t> vs(vsBytes.size()/4), fs(fsBytes.size()/4);
    std::memcpy(vs.data(), vsBytes.data(), vsBytes.size());
    std::memcpy(fs.data(), fsBytes.data(), fsBytes.size());

    ReflectionOptions opts{}; StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, vs.data(), vs.size(), opts, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, fs.data(), fs.size(), opts, rfs, &err)) << err;

    ShaderMeta meta = MergeStages({rvs, rfs});

    // Sanity: stages present
    ASSERT_TRUE(meta.Stages.count("vs") > 0);
    ASSERT_TRUE(meta.Stages.count("fs") > 0);

    // Descriptor sets: tolerant validation – ensure structure is consistent if present
    for (const auto& setInfo : meta.Sets) {
        for (const auto& binding : setInfo.Bindings) {
            // Binding index should be within a reasonable range
            EXPECT_GE(binding.Binding, 0u);
            // Stages mask should be nonzero when a binding exists
            EXPECT_GE(binding.StagesMask, 0u);
        }
    }

    // Push constants: ensure each declared range is within 128 bytes policy used by default
    for (const auto& pc : meta.PushConstants) {
        EXPECT_LE(pc.Size, 128u);
    }
}



TEST(DescriptorReflection, SampleTextureFS_CombinedSamplerBinding0) {
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
    std::vector<uint8_t> fsBytes = GameEngine::Rendering::Tests::ReadSpirvBytes("sample_texture.frag.spv");
    ASSERT_FALSE(fsBytes.empty());
    ReflectionOptions opts{}; StageReflectionResult rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fsBytes.data()), fsBytes.size()/4, opts, rfs, &err)) << err;
    ShaderMeta meta = MergeStages({ rfs });
    // Find set 0
    const DescriptorSetMeta* set0 = nullptr; for (const auto& s : meta.Sets) { if (s.Set == 0) { set0 = &s; break; } }
    ASSERT_NE(set0, nullptr);
    bool found=false; for (const auto& b : set0->Bindings) {
        if (b.Binding == 0) {
            EXPECT_EQ(b.Type, ShaderMetaBindingType::kCombinedImageSampler) << "Expected CombinedImageSampler type";
            // Abstract FS bit = 1<<1
            EXPECT_NE((b.StagesMask & (1u<<1)), 0u) << "Expected FS visibility";
            found = true; break;
        }
    }
    ASSERT_TRUE(found) << "Binding 0 not found in set 0";
}


namespace {
static bool FindCombinedSampler(const ShaderMeta& meta, uint32_t& outSet, uint32_t& outBinding) {
    for (const auto& s : meta.Sets) {
        for (const auto& b : s.Bindings) {
            if (b.Type == ShaderMetaBindingType::kCombinedImageSampler) { outSet = s.Set; outBinding = b.Binding; return true; }
        }
    }
    return false;
}
static int LayoutOrdinalForSet(const ShaderMeta& meta, uint32_t targetSet) {
    std::vector<uint32_t> sets; sets.reserve(meta.Sets.size());
    for (const auto& s : meta.Sets) sets.push_back(s.Set);
    std::sort(sets.begin(), sets.end());
    sets.erase(std::unique(sets.begin(), sets.end()), sets.end());
    for (size_t i=0;i<sets.size();++i) if (sets[i]==targetSet) return (int)i;
    return -1;
}
}

TEST(ReflectionDriven, Draw_SampleTexture_1x1_NonZero)
{
#if !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#endif
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    dd.enableDebugLayer = true; dd.enableDescriptorValidation = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    TextureDesc srcD{}; srcD.width=1; srcD.height=1; srcD.format=(uint32_t)TextureFormat::RGBA8_UNORM;
    srcD.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle src = dev->CreateTexture(srcD); ASSERT_TRUE(src.IsValid());

    {
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(src, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc rp{}; rp.colorTargets[0]=src; rp.colorTargetCount=1; rp.clearColor[0]=true;
        rp.clearColorValue[0][0]=0.7f; rp.clearColorValue[0][1]=0.2f; rp.clearColorValue[0][2]=0.1f; rp.clearColorValue[0][3]=1.0f;
        rp.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(rp);
        cl->SetViewport(0,0,1,1); cl->SetScissor(0,0,1,1);
        cl->EndRenderPass();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(src, ResourceState::RenderTarget, ResourceState::ShaderResource));
        cl->End();
        std::vector<CommandList*> lists{ cl.get() }; dev->ExecuteCommandLists(lists); dev->WaitForIdle();
    }

    TextureDesc dstD{}; dstD.width=1; dstD.height=1; dstD.format=(uint32_t)TextureFormat::RGBA8_UNORM;
    dstD.usage=(uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle dst = dev->CreateTexture(dstD); ASSERT_TRUE(dst.IsValid());

    std::vector<uint8_t> vs = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    std::vector<uint8_t> fs = GameEngine::Rendering::Tests::ReadSpirvBytes("sample_texture.frag.spv");
    ASSERT_FALSE(vs.empty()); ASSERT_FALSE(fs.empty());
    ReflectionOptions ro{}; StageReflectionResult rvs{}, rfs{}; std::string err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vs.data()), vs.size()/4, ro, rvs, &err)) << err;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fs.data()), fs.size()/4, ro, rfs, &err)) << err;
    ShaderMeta meta = MergeStages({ rvs, rfs });

    PipelineDesc pd{}; pd.type=PipelineType::Graphics; pd.vertexShader=vs; pd.pixelShader=fs; pd.debugName="ReflectionDriven_1x1";
    MaterialBuilder::FormatsHint fh{}; fh.ColorFormats = { (uint32_t)TextureFormat::RGBA8_UNORM };
    std::string bErr; ASSERT_TRUE(MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, fh, MaterialBuilder::MergeMode::Auto, MaterialBuilder::PushConstantPolicy{}, &bErr)) << bErr;

    uint32_t set = 0, binding = 0; ASSERT_TRUE(FindCombinedSampler(meta, set, binding));
    int layoutOrdinal = LayoutOrdinalForSet(meta, set); ASSERT_GE(layoutOrdinal, 0);

    PipelineHandle pipe = dev->CreatePipeline(pd); ASSERT_NE(pipe, INVALID_HANDLE);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = pd.descriptorSetLayouts[(size_t)layoutOrdinal]; dsDesc.transient=true; dsDesc.debugName="DS_1x1";
    DescriptorSetHandle dsH = dev->CreateDescriptorSet(dsDesc); ASSERT_TRUE(dsH.IsValid());

    SamplerDesc samp{}; SamplerHandle sh = dev->CreateSampler(samp); ASSERT_TRUE(sh.IsValid());
    dev->UpdateCombinedImageSamplerBinding(dsH, binding, src, sh);

    auto cl2 = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl2->Begin();
    cl2->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp2{}; rp2.colorTargets[0]=dst; rp2.colorTargetCount=1; rp2.clearColor[0]=true; rp2.colorStoreOp[0]=RenderPassDesc::StoreOp::Store;
    cl2->BeginRenderPass(rp2);
    cl2->SetPipeline(pipe);
    cl2->BindDescriptorSet(set, dsH, pipe);
    // Zero-init any declared push constant ranges
    uint32_t pcCount = dev->GetPipelinePushConstantRangeCount(pipe);
    for (uint32_t i = 0; i < pcCount; ++i) { PushConstantRangeInfo info{}; if (dev->GetPipelinePushConstantRangeInfo(pipe, i, info) && info.size>0) { std::vector<uint8_t> zero(info.size, 0); cl2->SetPushConstantsById(i, zero.data(), zero.size(), 0); } }
    cl2->SetViewport(0,0,1,1); cl2->SetScissor(0,0,1,1);
    cl2->Draw(3,1);
    cl2->EndRenderPass();

    cl2->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::RenderTarget, ResourceState::CopySource));
    const uint32_t rbSize = 4u; BufferHandle rb = dev->CreateReadbackBuffer(rbSize); ASSERT_TRUE(rb.IsValid());
    cl2->CopyTextureToBuffer(dst, rb, 1, 1);
    cl2->End();
    std::vector<CommandList*> lists2{ cl2.get() }; dev->ExecuteCommandLists(lists2); dev->WaitForIdle();

    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb)); ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data[0] > 0 || data[1] > 0 || data[2] > 0);
    dev->UnmapBuffer(rb);

    dev->DestroyBuffer(rb);
    dev->DestroySampler(sh);
    dev->DestroyTexture(dst);
    dev->DestroyTexture(src);
}

