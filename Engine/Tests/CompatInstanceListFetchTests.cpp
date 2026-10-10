// The compatibility profile's instance fetch, rendered. Each (material, mesh)
// batch is ONE instanced draw: the vertex stage reads
// GPUInstances[CompatInstanceList[first + gl_InstanceIndex]], with the batch's
// `first` in a push constant and firstInstance 0 (instance_io.glsl's compat
// branch; CpuDrawStreamBuilder builds the list). The probe compiles that
// branch from the source tree and draws two batches over scattered,
// out-of-order scene indices at non-zero offsets into one list.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderReflection.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef GE_RENDERER_REPO_ROOT
#error "GE_RENDERER_REPO_ROOT must be defined by CMake (source anchoring for shader contracts)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// One column per scene instance in a 64 x 1 target: the probe draws a sliver
// over column `instanceIndex` coloured by that instance's custom0.x, so the
// readback says which GPUScene record each list entry fetched.
constexpr uint32_t kColumns = 64;

constexpr const char kProbeVertex[] = R"(#version 450
#extension GL_GOOGLE_include_directive : require
#define GE_INSTANCED
#define GE_COMPAT_PROFILE
#include "instance_io.glsl"
layout(location = 0) flat out float vShade;
void main()
{
    InstanceData inst = ge_FetchInstanceData();
    const float column = 2.0 / 64.0;
    float centre = -1.0 + (float(inst.instanceIndex) + 0.5) * column;
    vec2 corners[3] = vec2[3](vec2(centre - 0.5 * column, -1.0), vec2(centre + 0.5 * column, -1.0),
                              vec2(centre, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    vShade = inst.custom0.x;
}
)";

constexpr const char kProbeFragment[] = R"(#version 450
layout(location = 0) flat in float vShade;
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = vec4(vShade, 1.0, 0.0, 1.0);
}
)";

// The binding numbers instance_io.glsl's compat branch declares on set 0.
constexpr uint32_t kGpuInstancesBinding = 29;
constexpr uint32_t kInstanceListBinding = 50;

bool CompileProbe(std::vector<uint8_t>& vertex, std::vector<uint8_t>& fragment, std::string& error)
{
    ShaderProgramCompileRequest req{};
    req.debugName = "CompatInstanceListProbe";
    const fs::path shaderRoot =
        fs::path(GE_RENDERER_REPO_ROOT) / "Engine" / "Modules" / "Rendering" / "Shaders";
    req.baseDirectory = shaderRoot / "Adapters";
    req.cacheRoot = fs::temp_directory_path() / "ge_compat_instance_list_probe_cache";
    req.includeDirs = {shaderRoot / "Includes"};
    ShaderStageCompileSpec vs{};
    vs.stage = "vs";
    vs.sourcePath = req.baseDirectory / "compat_instance_list_probe.vert";
    vs.inlineSource = kProbeVertex;
    ShaderStageCompileSpec fs_{};
    fs_.stage = "fs";
    fs_.sourcePath = req.baseDirectory / "compat_instance_list_probe.frag";
    fs_.inlineSource = kProbeFragment;
    req.stages = {vs, fs_};
    ShaderProgramCompileResult result{};
    if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &error))
        return false;
    const auto vsIt = result.stageBytes.find("vs");
    const auto fsIt = result.stageBytes.find("fs");
    if (vsIt == result.stageBytes.end() || fsIt == result.stageBytes.end())
    {
        error = "probe program produced no stage bytes";
        return false;
    }
    vertex = vsIt->second;
    fragment = fsIt->second;
    return true;
}

BufferHandle CreateFilledStorage(IDevice& device, const void* data, size_t bytes, const char* name)
{
    BufferDesc desc{};
    desc.size = bytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = name;
    const BufferHandle buffer = device.CreateBuffer(desc);
    if (buffer.IsValid())
    {
        if (void* mapped = device.MapBuffer(buffer))
        {
            std::memcpy(mapped, data, bytes);
            device.UnmapBuffer(buffer);
        }
    }
    return buffer;
}

struct Batch
{
    uint32_t First;
    uint32_t Count;
};

} // namespace

TEST(CompatInstanceListFetch, EachBatchDrawsItsOwnListedSceneInstances)
{
    if (!ShaderCompileService::IsCompilerAvailable())
        GTEST_SKIP() << "no shader compiler in this build";
    std::vector<uint8_t> vertexSpv;
    std::vector<uint8_t> fragmentSpv;
    std::string error;
    ASSERT_TRUE(CompileProbe(vertexSpv, fragmentSpv, error)) << error;

    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        GTEST_SKIP() << "no graphics device";

    ReflectionOptions ro{};
    StageReflectionResult rvs{};
    StageReflectionResult rfs{};
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(vertexSpv.data()),
                             vertexSpv.size() / 4, ro, rvs, &error))
        << error;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(fragmentSpv.data()),
                             fragmentSpv.size() / 4, ro, rfs, &error))
        << error;
    const ShaderMeta meta = MergeStages({rvs, rfs});

    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vertexSpv;
    pd.pixelShader = fragmentSpv;
    pd.debugName = "CompatInstanceListProbe";
    MaterialBuilder::FormatsHint formats{};
    formats.ColorFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
    ASSERT_TRUE(MaterialBuilder::BuildPipelineDescFromMeta(meta, pd, formats, MaterialBuilder::MergeMode::Auto,
                                                           MaterialBuilder::PushConstantPolicy{}, &error))
        << error;
    const PipelineHandle pipeline = dev->CreatePipeline(pd);
    ASSERT_NE(pipeline, INVALID_HANDLE) << "probe pipeline creation failed";
    ASSERT_FALSE(pd.descriptorSetLayouts.empty()) << "the probe reflected no set 0";

    // Scene instance i carries custom0.x = (i + 1) / 255, its column's red.
    std::vector<GPUInstance> instances(kColumns);
    for (uint32_t i = 0; i < kColumns; ++i)
        instances[i].custom0 = Vector4(static_cast<float>(i + 1u) / 255.0f, 0.0f, 0.0f, 0.0f);
    // Entry 0 is padding no batch reads; batch A is entries [1, 4), batch B [4, 6).
    const std::vector<uint32_t> list = {63u, 40u, 7u, 21u, 33u, 2u};
    const Batch batches[] = {{1u, 3u}, {4u, 2u}};
    const std::vector<uint32_t> drawn = {40u, 7u, 21u, 33u, 2u};

    const size_t instanceBytes = instances.size() * sizeof(GPUInstance);
    const size_t listBytes = list.size() * sizeof(uint32_t);
    const BufferHandle instanceBuffer =
        CreateFilledStorage(*dev, instances.data(), instanceBytes, "CompatInstanceListProbe.GPUInstances");
    const BufferHandle listBuffer =
        CreateFilledStorage(*dev, list.data(), listBytes, "CompatInstanceListProbe.List");
    ASSERT_TRUE(instanceBuffer.IsValid() && listBuffer.IsValid());

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = pd.descriptorSetLayouts[0];
    dsDesc.transient = true;
    dsDesc.debugName = "CompatInstanceListProbe.Set0";
    const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
    ASSERT_TRUE(ds.IsValid());
    dev->UpdateStorageBufferBinding(ds, kGpuInstancesBinding, instanceBuffer, 0, instanceBytes);
    dev->UpdateStorageBufferBinding(ds, kInstanceListBinding, listBuffer, 0, listBytes);

    TextureDesc dstDesc{};
    dstDesc.width = kColumns;
    dstDesc.height = 1;
    dstDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    dstDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                    static_cast<uint32_t>(TextureUsage::TransferSrc);
    const TextureHandle dst = dev->CreateTexture(dstDesc);
    ASSERT_TRUE(dst.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::Undefined, ResourceState::RenderTarget));
    RenderPassDesc rp{};
    rp.colorTargets[0] = dst;
    rp.colorTargetCount = 1;
    rp.clearColor[0] = true;
    rp.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    cl->BeginRenderPass(rp);
    cl->SetPipeline(pipeline);
    cl->BindDescriptorSet(0, ds, pipeline);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kColumns), 1.0f);
    cl->SetScissor(0, 0, kColumns, 1);
    for (const Batch& batch : batches)
    {
        cl->SetConstants(0, 0, sizeof(batch.First), &batch.First);
        cl->Draw(3, batch.Count, 0, 0);
    }
    cl->EndRenderPass();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dst, ResourceState::RenderTarget, ResourceState::CopySource));
    const BufferHandle readback = dev->CreateReadbackBuffer(kColumns * 4u);
    cl->CopyTextureToBuffer(dst, readback, kColumns, 1);
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    const auto* pixels = static_cast<const uint8_t*>(dev->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    for (uint32_t column = 0; column < kColumns; ++column)
    {
        const bool listed = std::find(drawn.begin(), drawn.end(), column) != drawn.end();
        const uint32_t red = pixels[column * 4u];
        if (listed)
            EXPECT_EQ(red, column + 1u) << "scene instance " << column << " drew the wrong record";
        else
            EXPECT_EQ(red, 0u) << "scene instance " << column << " is in no batch but drew";
    }
    dev->UnmapBuffer(readback);

    dev->DestroyBuffer(readback);
    dev->DestroyBuffer(listBuffer);
    dev->DestroyBuffer(instanceBuffer);
    dev->DestroyTexture(dst);
    dev->Shutdown();
}
