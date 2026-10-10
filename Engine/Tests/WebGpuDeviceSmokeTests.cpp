// WebGPU backend bring-up smoke: the factory creates a working headless
// device, the caps report the compat-profile truth (no bindless, elevated
// storage-binding limit requested), and the buffer path round-trips data.
// Everything here runs without a window or swapchain.

#include "EngineLogCapture.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/WebGpuPushConstantRing.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using namespace GameEngine::Rendering;
using namespace GameEngine;

std::unique_ptr<IDevice> CreateWebGpuDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::WebGPU;
    desc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && !device->Initialize(desc))
    {
        return nullptr;
    }
    return device;
}

// Vertex stage emits clip-space (-1,-1) (1,-1) (-1,1) from gl_VertexIndex
// (no vertex buffers); fragment stage writes opaque red. glslc, vulkan1.1.
const uint32_t kTriangleVert[] = {
0x07230203, 0x00010300, 0x000d000b, 0x00000027, 0x00000000, 0x00020011,
0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x0000001a, 0x00030047,
0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001,
0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
0x0000000b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x0000001a,
0x0000000b, 0x0000002a, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006,
0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003, 0x0000000b,
0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x0004002b, 0x00000008,
0x00000011, 0x00000003, 0x0004001c, 0x00000012, 0x00000010, 0x00000011,
0x0004002b, 0x00000006, 0x00000013, 0xbf800000, 0x0005002c, 0x00000010,
0x00000014, 0x00000013, 0x00000013, 0x0004002b, 0x00000006, 0x00000015,
0x3f800000, 0x0005002c, 0x00000010, 0x00000016, 0x00000015, 0x00000013,
0x0005002c, 0x00000010, 0x00000017, 0x00000013, 0x00000015, 0x0006002c,
0x00000012, 0x00000018, 0x00000014, 0x00000016, 0x00000017, 0x00040020,
0x00000019, 0x00000001, 0x0000000e, 0x0004003b, 0x00000019, 0x0000001a,
0x00000001, 0x00040020, 0x0000001c, 0x00000007, 0x00000012, 0x00040020,
0x0000001e, 0x00000007, 0x00000010, 0x0004002b, 0x00000006, 0x00000021,
0x00000000, 0x00040020, 0x00000025, 0x00000003, 0x00000007, 0x00050036,
0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
0x0004003b, 0x0000001c, 0x0000001d, 0x00000007, 0x0004003d, 0x0000000e,
0x0000001b, 0x0000001a, 0x0003003e, 0x0000001d, 0x00000018, 0x00050041,
0x0000001e, 0x0000001f, 0x0000001d, 0x0000001b, 0x0004003d, 0x00000010,
0x00000020, 0x0000001f, 0x00050051, 0x00000006, 0x00000022, 0x00000020,
0x00000000, 0x00050051, 0x00000006, 0x00000023, 0x00000020, 0x00000001,
0x00070050, 0x00000007, 0x00000024, 0x00000022, 0x00000023, 0x00000021,
0x00000015, 0x00050041, 0x00000025, 0x00000026, 0x0000000d, 0x0000000f,
0x0003003e, 0x00000026, 0x00000024, 0x000100fd, 0x00010038,
};
const uint32_t kTriangleFrag[] = {
0x07230203, 0x00010300, 0x000d000b, 0x0000000d, 0x00000000, 0x00020011,
0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013,
0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x0004002b,
0x00000006, 0x0000000b, 0x00000000, 0x0007002c, 0x00000007, 0x0000000c,
0x0000000a, 0x0000000b, 0x0000000b, 0x0000000a, 0x00050036, 0x00000002,
0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};
} // namespace

TEST(WebGpuDeviceSmoke, FactoryCreatesDeviceWithCompatCaps)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr) << "WebGPU device creation failed (no adapter?)";
    EXPECT_EQ(device->GetAPI(), GraphicsAPI::WebGPU);

    const auto& caps = device->GetCapabilities();
    EXPECT_FALSE(caps.supportsBindlessResources);
    EXPECT_FALSE(caps.supportsBufferDeviceAddress);
    EXPECT_FALSE(caps.supportsShaderInt64);
    EXPECT_FALSE(caps.supportsDrawIndirectCountNative);
    EXPECT_FALSE(caps.supportsAliasedStorageTextureBindings);
    // The granted storage-binding limit is logged by the device at init (the
    // elevated-limit request); the caps struct has no field for it yet.
    device->Shutdown();
}

TEST(WebGpuDeviceSmoke, BufferUploadRoundTrips)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    std::vector<uint32_t> pattern(1024);
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        pattern[i] = static_cast<uint32_t>(i * 2654435761u);
    }
    const size_t byteSize = pattern.size() * sizeof(uint32_t);

    BufferDesc desc{};
    desc.size = byteSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst |
                                       BufferUsage::TransferSrc);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "WebGpuSmoke.RoundTrip";
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    device->UpdateBuffer(buffer, 0, byteSize, pattern.data());

    // Upload-heap buffers keep a CPU shadow the map hands back; equality here
    // proves UpdateBuffer reached the same storage MapBuffer exposes.
    void* mapped = device->MapBuffer(buffer);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, pattern.data(), byteSize), 0);
    device->UnmapBuffer(buffer);

    device->DestroyBuffer(buffer);
    device->Shutdown();
}

TEST(WebGpuDeviceSmoke, RenderPassClearReadsBackExactColor)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    // 64 px * 4 B = 256 B rows: exactly WebGPU's COPY_BYTES_PER_ROW_ALIGNMENT.
    constexpr uint32_t kSize = 64;
    TextureDesc textureDesc{};
    textureDesc.width = kSize;
    textureDesc.height = kSize;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage =
        static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle target = device->CreateTexture(textureDesc);
    ASSERT_TRUE(target.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = kSize * kSize * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    readbackDesc.debugName = "WebGpuSmoke.ClearReadback";
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();

    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    // 0.25/0.5/0.75/1 land on exact 8-bit values (64/128/191... 0.75*255 =
    // 191.25 → use values with exact byte mappings instead).
    pass.clearColorValue[0][0] = 64.0f / 255.0f;
    pass.clearColorValue[0][1] = 128.0f / 255.0f;
    pass.clearColorValue[0][2] = 192.0f / 255.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    commandList->BeginRenderPass(pass);
    commandList->EndRenderPass();
    commandList->CopyTextureToBuffer(target, readback, kSize, kSize);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    for (uint32_t i = 0; i < kSize * kSize; ++i)
    {
        ASSERT_EQ(pixels[i * 4 + 0], 64) << "pixel " << i;
        ASSERT_EQ(pixels[i * 4 + 1], 128) << "pixel " << i;
        ASSERT_EQ(pixels[i * 4 + 2], 192) << "pixel " << i;
        ASSERT_EQ(pixels[i * 4 + 3], 255) << "pixel " << i;
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->Shutdown();
}

// The editor's icons are 32x32 RGBA8: a 128-byte row, which is exactly what
// WebGPU's 256-byte COPY_BYTES_PER_ROW_ALIGNMENT rejects. The backend restrides
// such an upload itself, so a caller passing the tight pitch every other
// backend accepts still gets its texels.
TEST(WebGpuDeviceSmoke, UnalignedUploadRowPitchRoundTrips)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kSize = 32;
    constexpr uint32_t kTightRowPitch = kSize * 4;
    constexpr uint32_t kAlignedRowPitch = 256;
    static_assert(kTightRowPitch % kAlignedRowPitch != 0, "test must exercise the repack path");

    std::vector<uint8_t> source(static_cast<size_t>(kTightRowPitch) * kSize);
    for (size_t i = 0; i < source.size(); ++i)
    {
        source[i] = static_cast<uint8_t>(i * 7u + 13u);
    }

    TextureDesc textureDesc{};
    textureDesc.width = kSize;
    textureDesc.height = kSize;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::TransferDst | TextureUsage::TransferSrc);
    textureDesc.debugName = "WebGpuSmoke.UnalignedUpload";
    TextureHandle target = device->CreateTexture(textureDesc);
    ASSERT_TRUE(target.IsValid());

    BufferDesc stagingDesc{};
    stagingDesc.size = source.size();
    stagingDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    stagingDesc.memoryUsage = BufferMemoryUsage::Upload;
    stagingDesc.debugName = "WebGpuSmoke.UnalignedStaging";
    BufferHandle staging = device->CreateBuffer(stagingDesc);
    ASSERT_TRUE(staging.IsValid());
    device->UpdateBuffer(staging, 0, source.size(), source.data());

    // The readback pitch is the caller's contract, so it is padded here: only
    // the upload side repacks.
    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kAlignedRowPitch) * kSize;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    readbackDesc.debugName = "WebGpuSmoke.UnalignedReadback";
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    commandList->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined,
                                                               ResourceState::CopyDest));
    // srcRowPitchBytes 0 = tightly packed, the default every backend resolves
    // to width * bytes-per-texel.
    commandList->CopyBufferToTextureSubresource(staging, target, 0, 0, kSize, kSize);
    commandList->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::CopyDest,
                                                               ResourceState::CopySource));
    commandList->CopyTextureSubresourceToBuffer(target, 0, 0, readback, kSize, kSize, 0, 0, 0, kAlignedRowPitch);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    for (uint32_t y = 0; y < kSize; ++y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * kAlignedRowPitch;
        const uint8_t* expected = source.data() + static_cast<size_t>(y) * kTightRowPitch;
        ASSERT_EQ(std::memcmp(row, expected, kTightRowPitch), 0) << "row " << y;
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyBuffer(staging);
    device->DestroyTexture(target);
    device->Shutdown();
}

TEST(WebGpuDeviceSmoke, ComputeDispatchThroughSpirvIngestion)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    // values[i] = i * 3 + 7 over one 64-wide workgroup; glslc-compiled from
    // the shader in the comment history of this test's PR, vulkan1.1 target
    // (the SPIR-V level naga and wgpu's frontend both accept).
    static const uint32_t kKernel[] = {
    0x07230203, 0x00010300, 0x000d000b, 0x00000021, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
    0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000b,
    0x0000000b, 0x0000001c, 0x00040047, 0x00000010, 0x00000006, 0x00000004,
    0x00030047, 0x00000011, 0x00000002, 0x00050048, 0x00000011, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000013, 0x00000021, 0x00000000,
    0x00040047, 0x00000013, 0x00000022, 0x00000000, 0x00040047, 0x00000020,
    0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000009, 0x00000006, 0x00000003, 0x00040020, 0x0000000a, 0x00000001,
    0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b,
    0x00000006, 0x0000000c, 0x00000000, 0x00040020, 0x0000000d, 0x00000001,
    0x00000006, 0x0003001d, 0x00000010, 0x00000006, 0x0003001e, 0x00000011,
    0x00000010, 0x00040020, 0x00000012, 0x0000000c, 0x00000011, 0x0004003b,
    0x00000012, 0x00000013, 0x0000000c, 0x00040015, 0x00000014, 0x00000020,
    0x00000001, 0x0004002b, 0x00000014, 0x00000015, 0x00000000, 0x0004002b,
    0x00000006, 0x00000018, 0x00000003, 0x0004002b, 0x00000006, 0x0000001a,
    0x00000007, 0x00040020, 0x0000001c, 0x0000000c, 0x00000006, 0x0004002b,
    0x00000006, 0x0000001e, 0x00000040, 0x0004002b, 0x00000006, 0x0000001f,
    0x00000001, 0x0006002c, 0x00000009, 0x00000020, 0x0000001e, 0x0000001f,
    0x0000001f, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x0000000d, 0x0000000e, 0x0000000b,
    0x0000000c, 0x0004003d, 0x00000006, 0x0000000f, 0x0000000e, 0x00050084,
    0x00000006, 0x00000019, 0x0000000f, 0x00000018, 0x00050080, 0x00000006,
    0x0000001b, 0x00000019, 0x0000001a, 0x00060041, 0x0000001c, 0x0000001d,
    0x00000013, 0x00000015, 0x0000000f, 0x0003003e, 0x0000001d, 0x0000001b,
    0x000100fd, 0x00010038,
    };

    constexpr uint32_t kElements = 64;
    constexpr size_t kByteSize = kElements * sizeof(uint32_t);

    BufferDesc storageDesc{};
    storageDesc.size = kByteSize;
    storageDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    storageDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    storageDesc.debugName = "WebGpuSmoke.ComputeOut";
    BufferHandle storage = device->CreateBuffer(storageDesc);
    ASSERT_TRUE(storage.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = kByteSize;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    DescriptorSetLayoutDesc layoutDesc{};
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::StorageBuffer;
    binding.shaderStages = kShaderStageCompute;
    layoutDesc.bindings.push_back(binding);
    const DescriptorSetLayoutId layoutId = device->InternDescriptorSetLayout(layoutDesc);

    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kKernel),
        reinterpret_cast<const uint8_t*>(kKernel) + sizeof(kKernel));
    pipelineDesc.DescriptorSetLayouts.push_back(layoutId);
    pipelineDesc.DebugName = "WebGpuSmoke.Compute";
    const ComputePipelineId pipelineId = device->InternComputePipeline(pipelineDesc);
    const PipelineHandle pipeline = device->GetOrCreateComputePipeline(pipelineId);
    ASSERT_TRUE(pipeline.IsValid()) << "compute pipeline creation failed (SPIR-V ingestion?)";

    DescriptorSetDesc setDesc{};
    setDesc.layout = layoutDesc;
    DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());

    DescriptorSetUpdate update{};
    update.binding = 0;
    update.type = DescriptorType::StorageBuffer;
    update.buffers.push_back(storage);
    update.bufferOffsets.push_back(0);
    update.bufferRanges.push_back(kByteSize);
    device->UpdateDescriptorSet(set, update);

    auto commandList = device->CreateCommandList(IDevice::QueueType::Compute);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    commandList->SetPipeline(pipeline);
    commandList->BindDescriptorSet(0, set, pipeline);
    commandList->Dispatch(1);
    commandList->CopyBuffer(storage, readback, kByteSize);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint32_t* values = static_cast<const uint32_t*>(device->MapBuffer(readback));
    ASSERT_NE(values, nullptr);
    for (uint32_t i = 0; i < kElements; ++i)
    {
        ASSERT_EQ(values[i], i * 3u + 7u) << "element " << i;
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyBuffer(storage);
    device->Shutdown();
}

namespace
{
void CheckTriangleCoverage(bool indirect)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    // 64 px * 4 B rows = WebGPU's COPY_BYTES_PER_ROW_ALIGNMENT exactly.
    constexpr uint32_t kSize = 64;
    TextureDesc textureDesc{};
    textureDesc.width = kSize;
    textureDesc.height = kSize;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage =
        static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle target = device->CreateTexture(textureDesc);
    ASSERT_TRUE(target.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = kSize * kSize * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleVert),
        reinterpret_cast<const uint8_t*>(kTriangleVert) + sizeof(kTriangleVert));
    pipelineDesc.PixelShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleFrag),
        reinterpret_cast<const uint8_t*>(kTriangleFrag) + sizeof(kTriangleFrag));
    // One variable at a time: no culling, so the coverage probes answer the
    // Y-orientation question independently of winding translation.
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "WebGpuSmoke.Triangle";
    const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(pipelineDesc);

    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.ColorCount = 1;
    const PipelineHandle pipeline = device->GetOrCreateGraphicsPipeline(pipelineId, formatKey);
    ASSERT_TRUE(pipeline.IsValid()) << "graphics pipeline creation failed";

    BufferHandle indices{}, arguments{};
    if (indirect)
    {
        const uint32_t indexData[] = {0u, 1u, 2u};
        // A nonzero firstInstance is how grass selects its far-LOD slice.
        // Without indirect-first-instance, WebGPU silently drops this draw.
        const uint32_t drawArgs[] = {3u, 1u, 0u, 0u, 37u};
        BufferDesc desc{};
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.size = sizeof(indexData);
        desc.usage = static_cast<uint32_t>(BufferUsage::Index);
        indices = device->CreateBuffer(desc);
        ASSERT_TRUE(indices.IsValid());
        device->UpdateBuffer(indices, 0, sizeof(indexData), indexData);
        desc.size = sizeof(drawArgs);
        desc.usage = static_cast<uint32_t>(BufferUsage::Indirect);
        arguments = device->CreateBuffer(desc);
        ASSERT_TRUE(arguments.IsValid());
        device->UpdateBuffer(arguments, 0, sizeof(drawArgs), drawArgs);
    }

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 0.0f;
    pass.clearColorValue[0][1] = 0.0f;
    pass.clearColorValue[0][2] = 0.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    commandList->BeginRenderPass(pass);
    commandList->SetPipeline(pipeline);
    commandList->SetViewport(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize));
    commandList->SetScissor(0, 0, kSize, kSize);
    if (indirect)
    {
        commandList->SetIndexBuffer(indices, IndexType::Uint32);
        commandList->DrawIndexedIndirect(arguments, 1, 5 * sizeof(uint32_t));
    }
    else
        commandList->Draw(3);
    commandList->EndRenderPass();
    commandList->CopyTextureToBuffer(target, readback, kSize, kSize);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    auto red = [&](uint32_t x, uint32_t row) {
        const uint8_t* p = pixels + (row * kSize + x) * 4;
        return p[0] > 200 && p[1] < 50;
    };

    // The engine's cross-backend convention (Vulkan flips via negative
    // viewport; Metal is native): clip-space y=-1 lands at the BOTTOM of the
    // image, i.e. the highest readback row. The triangle covers the
    // lower-left half, so bottom-left is red and top-right is background.
    EXPECT_TRUE(red(4, kSize - 5)) << "bottom-left should be inside the triangle";
    EXPECT_TRUE(red(4, kSize / 2)) << "left edge midpoint should be inside";
    EXPECT_TRUE(red(kSize / 2, kSize - 5)) << "bottom edge midpoint should be inside";
    EXPECT_FALSE(red(kSize - 5, 4)) << "top-right must be background";

    // Coverage sanity: about half the pixels are red.
    uint32_t redCount = 0;
    for (uint32_t row = 0; row < kSize; ++row)
        for (uint32_t x = 0; x < kSize; ++x)
            redCount += red(x, row) ? 1u : 0u;
    EXPECT_GT(redCount, kSize * kSize * 40 / 100);
    EXPECT_LT(redCount, kSize * kSize * 60 / 100);

    device->UnmapBuffer(readback);
    if (indices.IsValid()) device->DestroyBuffer(indices);
    if (arguments.IsValid()) device->DestroyBuffer(arguments);
    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->Shutdown();
}

} // namespace

TEST(WebGpuDeviceSmoke, TriangleDrawCoversLowerLeftHalf)
{
    CheckTriangleCoverage(false);
}

TEST(WebGpuDeviceSmoke, IndexedIndirectDrawWithNonzeroFirstInstanceRenders)
{
    CheckTriangleCoverage(true);
}

// A draw whose pipeline handle the device holds no render pipeline for (here an invalid handle; a
// destroyed handle or a failed creation alike) is refused, and a later draw with a real pipeline in
// the same pass still lands. The encoder still holds the previous draw's pipeline, and recording
// the refused draw would run that pipeline with this draw's bindings.
TEST(WebGpuDeviceSmoke, DrawWithoutARenderPipelineIsRefusedRatherThanDrawnWithThePreviousOne)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kSize = 64;
    TextureDesc textureDesc{};
    textureDesc.width = kSize;
    textureDesc.height = kSize;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle target = device->CreateTexture(textureDesc);
    ASSERT_TRUE(target.IsValid());
    BufferDesc readbackDesc{};
    readbackDesc.size = kSize * kSize * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleVert),
        reinterpret_cast<const uint8_t*>(kTriangleVert) + sizeof(kTriangleVert));
    pipelineDesc.PixelShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleFrag),
        reinterpret_cast<const uint8_t*>(kTriangleFrag) + sizeof(kTriangleFrag));
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "WebGpuSmoke.RefusedDraw";
    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.ColorCount = 1;
    const PipelineHandle pipeline =
        device->GetOrCreateGraphicsPipeline(device->InternGraphicsPipeline(pipelineDesc), formatKey);
    ASSERT_TRUE(pipeline.IsValid());

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    commandList->BeginRenderPass(pass);
    // The left half: the red triangle, drawn with a real pipeline.
    commandList->SetPipeline(pipeline);
    commandList->SetViewport(0.0f, 0.0f, kSize / 2.0f, static_cast<float>(kSize));
    commandList->SetScissor(0, 0, kSize, kSize);
    commandList->Draw(3);
    // The right half: a draw with no pipeline, which must leave the clear colour.
    commandList->SetPipeline(PipelineHandle{});
    commandList->SetViewport(kSize / 2.0f, 0.0f, kSize / 2.0f, static_cast<float>(kSize));
    commandList->Draw(3);
    // The top-right quadrant: the real pipeline again, after the refusal, in the same pass.
    commandList->SetPipeline(pipeline);
    commandList->SetViewport(kSize / 2.0f, 0.0f, kSize / 2.0f, kSize / 2.0f);
    commandList->Draw(3);
    commandList->EndRenderPass();
    commandList->CopyTextureToBuffer(target, readback, kSize, kSize);
    commandList->End();
    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    auto red = [&](uint32_t x, uint32_t row) {
        const uint8_t* p = pixels + (row * kSize + x) * 4;
        return p[0] > 200 && p[1] < 50;
    };
    EXPECT_TRUE(red(4, kSize - 5)) << "the draw with a pipeline covers the left half's lower-left";
    EXPECT_FALSE(red(kSize / 2 + 4, kSize - 5)) << "the draw without a pipeline ran the previous draw's pipeline";
    EXPECT_TRUE(red(kSize / 2 + 4, kSize / 2 - 5)) << "a draw with a real pipeline after the refusal did not land";
    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->Shutdown();
}

namespace
{
// Vertex stage passes the position (location 0) through and hands the vertex
// colour (location 4) to the fragment stage, which writes it. glslc -O,
// vulkan1.1, from:
//   layout(location = 0) in vec3 inPosition;
//   layout(location = 4) in vec4 inColor;
//   layout(location = 0) out vec4 outColor;
//   void main() { gl_Position = vec4(inPosition, 1.0); outColor = inColor; }
// and a fragment stage `outColor = inColor;`.
const uint32_t kVertexColorVert[] = {
0x07230203, 0x00010300, 0x000d000b, 0x0000001f, 0x00000000, 0x00020011,
0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000000,
0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000012, 0x0000001b,
0x0000001d, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b,
0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001,
0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004,
0x00040047, 0x00000012, 0x0000001e, 0x00000000, 0x00040047, 0x0000001b,
0x0000001e, 0x00000000, 0x00040047, 0x0000001d, 0x0000001e, 0x00000004,
0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008,
0x00000009, 0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009,
0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a,
0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c,
0x0000000d, 0x00000003, 0x00040015, 0x0000000e, 0x00000020, 0x00000001,
0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040017, 0x00000010,
0x00000006, 0x00000003, 0x00040020, 0x00000011, 0x00000001, 0x00000010,
0x0004003b, 0x00000011, 0x00000012, 0x00000001, 0x0004002b, 0x00000006,
0x00000014, 0x3f800000, 0x00040020, 0x00000019, 0x00000003, 0x00000007,
0x0004003b, 0x00000019, 0x0000001b, 0x00000003, 0x00040020, 0x0000001c,
0x00000001, 0x00000007, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000001,
0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
0x00000005, 0x0004003d, 0x00000010, 0x00000013, 0x00000012, 0x00050051,
0x00000006, 0x00000015, 0x00000013, 0x00000000, 0x00050051, 0x00000006,
0x00000016, 0x00000013, 0x00000001, 0x00050051, 0x00000006, 0x00000017,
0x00000013, 0x00000002, 0x00070050, 0x00000007, 0x00000018, 0x00000015,
0x00000016, 0x00000017, 0x00000014, 0x00050041, 0x00000019, 0x0000001a,
0x0000000d, 0x0000000f, 0x0003003e, 0x0000001a, 0x00000018, 0x0004003d,
0x00000007, 0x0000001e, 0x0000001d, 0x0003003e, 0x0000001b, 0x0000001e,
0x000100fd, 0x00010038,
};
const uint32_t kVertexColorFrag[] = {
0x07230203, 0x00010300, 0x000d000b, 0x0000000d, 0x00000000, 0x00020011,
0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x00030010,
0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
0x00040047, 0x0000000b, 0x0000001e, 0x00000000, 0x00020013, 0x00000002,
0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008,
0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
0x00040020, 0x0000000a, 0x00000001, 0x00000007, 0x0004003b, 0x0000000a,
0x0000000b, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000c,
0x0000000b, 0x0003003e, 0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};
} // namespace

// The engine's vertex bindings are fixed numbers (VertexLayoutBuilder.h) and a
// mesh without a stream leaves its number unused: position and colour alone
// are bindings 0 and 2, and every draw binds the colour stream as binding 2.
// The pipeline's WebGPU buffer slots are dense (colour is its slot 1), so the
// backend binds binding 2 at slot 1; bound at slot 2, slot 1 stays empty and
// the whole submit fails validation and draws nothing.
TEST(WebGpuDeviceSmoke, SparseVertexBindingsDraw)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kSize = 64;
    TextureDesc textureDesc{};
    textureDesc.width = kSize;
    textureDesc.height = kSize;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle target = device->CreateTexture(textureDesc);
    ASSERT_TRUE(target.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = kSize * kSize * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    // A full-screen triangle, green at every vertex.
    const float positions[] = {-1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f, -1.0f, 3.0f, 0.0f};
    const float colors[] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    BufferDesc vertexDesc{};
    vertexDesc.memoryUsage = BufferMemoryUsage::Upload;
    vertexDesc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    vertexDesc.size = sizeof(positions);
    BufferHandle positionBuffer = device->CreateBuffer(vertexDesc);
    vertexDesc.size = sizeof(colors);
    BufferHandle colorBuffer = device->CreateBuffer(vertexDesc);
    ASSERT_TRUE(positionBuffer.IsValid() && colorBuffer.IsValid());
    device->UpdateBuffer(positionBuffer, 0, sizeof(positions), positions);
    device->UpdateBuffer(colorBuffer, 0, sizeof(colors), colors);

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kVertexColorVert),
        reinterpret_cast<const uint8_t*>(kVertexColorVert) + sizeof(kVertexColorVert));
    pipelineDesc.PixelShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kVertexColorFrag),
        reinterpret_cast<const uint8_t*>(kVertexColorFrag) + sizeof(kVertexColorFrag));
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "WebGpuSmoke.SparseVertexBindings";
    BuildVertexLayoutFromFlags(VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasColor, pipelineDesc);
    ASSERT_EQ(pipelineDesc.VertexBindings.size(), 2u);
    ASSERT_EQ(pipelineDesc.VertexBindings[1].binding, VertexBinding::Color);
    const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(pipelineDesc);

    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.ColorCount = 1;
    const PipelineHandle pipeline = device->GetOrCreateGraphicsPipeline(pipelineId, formatKey);
    ASSERT_TRUE(pipeline.IsValid()) << "graphics pipeline creation failed";

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    commandList->BeginRenderPass(pass);
    commandList->SetPipeline(pipeline);
    commandList->SetViewport(0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize));
    commandList->SetScissor(0, 0, kSize, kSize);
    commandList->SetVertexBuffer(positionBuffer, VertexBinding::Core);
    commandList->SetVertexBuffer(colorBuffer, VertexBinding::Color);
    commandList->Draw(3);
    commandList->EndRenderPass();
    commandList->CopyTextureToBuffer(target, readback, kSize, kSize);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    const uint8_t* centre = pixels + ((kSize / 2) * kSize + kSize / 2) * 4;
    EXPECT_LT(centre[0], 50) << "the draw did not run: the target kept its black clear";
    EXPECT_GT(centre[1], 200) << "the draw did not run: the target kept its black clear";
    device->UnmapBuffer(readback);

    device->DestroyBuffer(colorBuffer);
    device->DestroyBuffer(positionBuffer);
    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->Shutdown();
}

TEST(WebGpuDeviceSmoke, TextureCreateAndDestroy)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(desc);
    ASSERT_TRUE(texture.IsValid());
    device->DestroyTexture(texture);
    device->Shutdown();
}

// The format query answers for storage the way the API does. Core WebGPU keeps
// a fixed storage-capable list; rg16float and r8unorm are not on it, rgba16float
// is. A caller that asks before creating gets the refusal here instead of an
// invalid texture at CreateTexture.
TEST(WebGpuDeviceSmoke, FormatQueryRefusesStorageOnFormatsCoreWebGpuExcludes)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    const uint32_t storage = static_cast<uint32_t>(TextureUsage::UnorderedAccess);
    const uint32_t sampled = static_cast<uint32_t>(TextureUsage::ShaderResource);
    EXPECT_FALSE(device->IsTextureFormatSupported(TextureFormat::R16G16_FLOAT, storage))
        << "rg16float is not storage-capable in core WebGPU";
    EXPECT_FALSE(device->IsTextureFormatSupported(TextureFormat::R8_UNORM, storage))
        << "r8unorm is not storage-capable in core WebGPU";
    EXPECT_TRUE(device->IsTextureFormatSupported(TextureFormat::R16G16B16A16_FLOAT, storage))
        << "rgba16float is storage-capable in core WebGPU";
    EXPECT_TRUE(device->IsTextureFormatSupported(TextureFormat::RGBA8_UNORM, storage))
        << "rgba8unorm is storage-capable in core WebGPU";
    EXPECT_TRUE(device->IsTextureFormatSupported(TextureFormat::R16G16_FLOAT, sampled))
        << "the storage rule must not leak into sampled use";
    EXPECT_EQ(device->IsTextureFormatSupported(TextureFormat::R16G16_FLOAT, storage),
              device->GetCapabilities().supportsRG16FloatStorage)
        << "the query and the capability flag disagree about rg16float storage";
    device->Shutdown();
}

// Windowed presentation: opt-in (GE_WEBGPU_WINDOWED_SMOKE=1) so CI stays
// headless. Creates a real GLFW window, a window target through the surface
// bridge, and presents 60 frames of animated clears with the probe triangle.
TEST(WebGpuDeviceSmoke, WindowedPresentation)
{
    if (std::getenv("GE_WEBGPU_WINDOWED_SMOKE") == nullptr)
    {
        GTEST_SKIP() << "set GE_WEBGPU_WINDOWED_SMOKE=1 to run the windowed pass";
    }
    struct GlfwSession
    {
        ~GlfwSession() { glfwTerminate(); }
    } session;
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(640, 480, "WebGPU smoke", nullptr, nullptr), glfwDestroyWindow);
    ASSERT_NE(window, nullptr);

    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    const WindowTargetHandle target = device->CreateWindowTarget(window.get(), 640, 480);
    ASSERT_TRUE(target.IsValid()) << "surface/window target creation failed";
    ASSERT_TRUE(device->SetActiveWindowTarget(target));

    // A bare test process needs a few event pumps before the window is
    // composited; until then the surface reports Occluded and BeginFrame
    // legitimately skips the frame.
    int rendered = 0;
    for (int frame = 0; frame < 300 && rendered < 60; ++frame)
    {
        glfwPollEvents();
        if (!device->BeginFrame())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        ++rendered;
        const TextureHandle backbuffer = device->GetCurrentSwapchainImageHandle();
        ASSERT_TRUE(backbuffer.IsValid()) << "frame " << frame;

        auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
        commandList->Begin();
        RenderPassDesc pass{};
        pass.colorTargets[0] = backbuffer;
        pass.colorTargetCount = 1;
        pass.clearColor[0] = true;
        pass.clearColorValue[0][0] = 0.1f;
        pass.clearColorValue[0][1] = 0.1f + 0.8f * (static_cast<float>(frame) / 60.0f);
        pass.clearColorValue[0][2] = 0.3f;
        pass.clearColorValue[0][3] = 1.0f;
        pass.depthTarget = INVALID_TEXTURE_HANDLE;
        pass.clearDepth = false;
        commandList->BeginRenderPass(pass);
        commandList->EndRenderPass();
        commandList->End();
        std::vector<CommandList*> lists{commandList.get()};
        device->ExecuteCommandLists(lists);
        device->Present();
    }
    EXPECT_GE(rendered, 30) << "window never became presentable";

    device->WaitForIdle();
    EXPECT_TRUE(device->DestroyWindowTarget(target));
    device->Shutdown();
}

// PipelineDesc spells push constants two ways and the range list wins when it
// is present, so a desc that used only that spelling leaves PushConstants.Size
// at zero. Everything WebGPU derives from the block reads one number, and the
// hand-built editor pipelines (SV_Gizmos_Lines and friends) use the range list
// exclusively: a zero here drops the emulated UBO's bind-group layout and
// makes every such pipeline fail creation.
TEST(WebGpuDeviceSmoke, PushConstantBlockSizedFromNamedRangesAlone)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kMatrixBytes = 64;
    constexpr uint32_t kColorBytes = 16;

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleVert),
        reinterpret_cast<const uint8_t*>(kTriangleVert) + sizeof(kTriangleVert));
    pipelineDesc.PixelShader = std::make_shared<const std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(kTriangleFrag),
        reinterpret_cast<const uint8_t*>(kTriangleFrag) + sizeof(kTriangleFrag));
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "WebGpuSmoke.NamedRangesOnly";
    NamedPushConstantRange model{};
    model.Name = "Model";
    model.Offset = 0;
    model.Size = kMatrixBytes;
    model.StageMask = kShaderStageVertex;
    NamedPushConstantRange color{};
    color.Name = "Color";
    color.Offset = kMatrixBytes;
    color.Size = kColorBytes;
    color.StageMask = kShaderStageVertex;
    pipelineDesc.NamedPushConstantRanges = {model, color};

    const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(pipelineDesc);
    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.ColorCount = 1;
    const PipelineHandle pipeline = device->GetOrCreateGraphicsPipeline(pipelineId, formatKey);
    ASSERT_TRUE(pipeline.IsValid()) << "pipeline with range-list-only push constants failed";

    PipelinePushConstantInfo info{};
    ASSERT_TRUE(device->GetPipelinePushConstantInfo(pipeline, info));
    EXPECT_EQ(info.size, kMatrixBytes + kColorBytes);

    device->Shutdown();
}

namespace
{

// Copies a persistently mapped upload buffer to a readback buffer and returns
// what the GPU actually holds. ExecuteCommandLists is what flushes declared
// ranges, so the copy has to be a real submit — reading the CPU shadow back
// would prove nothing about publication.
std::vector<uint32_t> ReadBackThroughSubmit(IDevice& device, BufferHandle source, size_t byteSize)
{
    BufferDesc readbackDesc{};
    readbackDesc.size = byteSize;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    readbackDesc.debugName = "WebGpuSmoke.FlushRangeReadback";
    BufferHandle readback = device.CreateBuffer(readbackDesc);
    if (!readback.IsValid())
    {
        return {};
    }

    auto commandList = device.CreateCommandList(IDevice::QueueType::Graphics);
    commandList->Begin();
    commandList->CopyBuffer(source, readback, byteSize);
    commandList->End();
    std::vector<CommandList*> lists{commandList.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();

    std::vector<uint32_t> out(byteSize / sizeof(uint32_t), 0);
    if (const void* mapped = device.MapBuffer(readback))
    {
        std::memcpy(out.data(), mapped, byteSize);
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return out;
}

} // namespace

// The contract's two halves, on one buffer: a mapping publishes in full until
// its first submit — which is what keeps a producer that writes once at
// initialization correct without declaring anything — and from then on
// publishes exactly the ranges it declares.
TEST(WebGpuDeviceSmoke, PersistentMappingPublishesWholeBufferThenDeclaredRanges)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kElements = 4096;
    constexpr size_t kByteSize = kElements * sizeof(uint32_t);

    BufferDesc desc{};
    desc.size = kByteSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "WebGpuSmoke.PersistentRing";
    BufferHandle ring = device->CreateBuffer(desc);
    ASSERT_TRUE(ring.IsValid());

    // Mapped once and never unmapped, the way every ring in the engine holds
    // its upload buffer.
    auto* mapped = static_cast<uint32_t*>(device->MapBuffer(ring));
    ASSERT_NE(mapped, nullptr);

    for (uint32_t i = 0; i < kElements; ++i)
    {
        mapped[i] = i + 1u;
    }
    std::vector<uint32_t> published = ReadBackThroughSubmit(*device, ring, kByteSize);
    ASSERT_EQ(published.size(), kElements);
    for (uint32_t i = 0; i < kElements; ++i)
    {
        ASSERT_EQ(published[i], i + 1u) << "undeclared first write, element " << i;
    }

    // Two disjoint regions, declared in the reverse of the order they were
    // written: the backend sorts and merges, so declaration order is free.
    constexpr uint32_t kLowBegin = 8;
    constexpr uint32_t kLowCount = 16;
    constexpr uint32_t kHighBegin = 3000;
    constexpr uint32_t kHighCount = 32;
    for (uint32_t i = 0; i < kLowCount; ++i)
    {
        mapped[kLowBegin + i] = 0xA0000000u | i;
    }
    for (uint32_t i = 0; i < kHighCount; ++i)
    {
        mapped[kHighBegin + i] = 0xB0000000u | i;
    }
    // An element the producer writes but never declares, to show the ranges
    // are what publishes rather than the submit.
    constexpr uint32_t kUndeclared = 1500;
    mapped[kUndeclared] = 0xDEADBEEFu;

    device->FlushMappedRange(ring, kHighBegin * sizeof(uint32_t), kHighCount * sizeof(uint32_t));
    device->FlushMappedRange(ring, kLowBegin * sizeof(uint32_t), kLowCount * sizeof(uint32_t));

    published = ReadBackThroughSubmit(*device, ring, kByteSize);
    ASSERT_EQ(published.size(), kElements);
    for (uint32_t i = 0; i < kLowCount; ++i)
    {
        EXPECT_EQ(published[kLowBegin + i], 0xA0000000u | i) << "low range, element " << i;
    }
    for (uint32_t i = 0; i < kHighCount; ++i)
    {
        EXPECT_EQ(published[kHighBegin + i], 0xB0000000u | i) << "high range, element " << i;
    }
    EXPECT_EQ(published[kUndeclared], kUndeclared + 1u)
        << "an undeclared write reached the GPU, so the flush is not range-driven";

    device->DestroyBuffer(ring);
    device->Shutdown();
}

// A producer that maps, writes and unmaps without declaring anything is the
// pattern used all over the engine for one-shot uploads. Ending the mapping
// publishes the whole buffer, so those callers need no migration.
TEST(WebGpuDeviceSmoke, UnmapPublishesWholeBufferWithoutDeclaredRanges)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kElements = 256;
    constexpr size_t kByteSize = kElements * sizeof(uint32_t);

    BufferDesc desc{};
    desc.size = kByteSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName = "WebGpuSmoke.OneShotUpload";
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    auto* mapped = static_cast<uint32_t*>(device->MapBuffer(buffer));
    ASSERT_NE(mapped, nullptr);
    for (uint32_t i = 0; i < kElements; ++i)
    {
        mapped[i] = 0xC0000000u | i;
    }
    device->UnmapBuffer(buffer);

    const std::vector<uint32_t> published = ReadBackThroughSubmit(*device, buffer, kByteSize);
    ASSERT_EQ(published.size(), kElements);
    for (uint32_t i = 0; i < kElements; ++i)
    {
        ASSERT_EQ(published[i], 0xC0000000u | i) << "element " << i;
    }

    device->DestroyBuffer(buffer);
    device->Shutdown();
}

// A non-zero FillBuffer has no encoder command in WebGPU, and the engine fills
// with one wherever a buffer is seeded before use: SDSM's 0xFFFFFFFF min
// sentinel, occlusion culling's previous-visibility reset, indirect-argument
// and cursor priming. Silently dropping those leaves the buffer at zero, which
// every one of those consumers reads as a legitimate value — a wrong picture,
// not a missing one. Pins that the bytes actually land.
TEST(WebGpuDeviceSmoke, NonZeroFillBufferLandsInTheBuffer)
{
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    constexpr uint32_t kElements = 64;
    constexpr size_t kByteSize = kElements * sizeof(uint32_t);
    constexpr uint32_t kSentinel = 0xFFFFFFFFu;

    BufferDesc storageDesc{};
    storageDesc.size = kByteSize;
    storageDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc |
                                              BufferUsage::TransferDst);
    storageDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    storageDesc.debugName = "WebGpuSmoke.FillTarget";
    BufferHandle storage = device->CreateBuffer(storageDesc);
    ASSERT_TRUE(storage.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = kByteSize;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(commandList, nullptr);
    commandList->Begin();
    commandList->FillBuffer(storage, 0, kByteSize, kSentinel);
    commandList->CopyBuffer(storage, readback, kByteSize);
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    device->ExecuteCommandLists(lists);
    device->WaitForIdle();

    const uint32_t* values = static_cast<const uint32_t*>(device->MapBuffer(readback));
    ASSERT_NE(values, nullptr);
    for (uint32_t i = 0; i < kElements; ++i)
    {
        ASSERT_EQ(values[i], kSentinel) << "element " << i;
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyBuffer(storage);
    device->Shutdown();
}

namespace
{

// Ends `commandList` with a copy of `target`, submits it and returns the texels.
std::vector<uint8_t> ReadTarget(IDevice& device, CommandList& commandList, TextureHandle target,
                                uint32_t width, uint32_t height)
{
    BufferDesc readbackDesc{};
    readbackDesc.size = width * height * 4u;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    const BufferHandle readback = device.CreateBuffer(readbackDesc);
    if (!readback.IsValid())
        return {};
    commandList.CopyTextureToBuffer(target, readback, width, height);
    commandList.End();
    std::vector<CommandList*> lists{&commandList};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    std::vector<uint8_t> pixels(readbackDesc.size, 0u);
    if (const void* mapped = device.MapBuffer(readback))
    {
        std::memcpy(pixels.data(), mapped, pixels.size());
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return pixels;
}

// The browser's push-constant transport, written as the shader cook writes it:
// the block is a uniform at group 3 that the backend's ring binds per draw.
constexpr const char kRingProbeVertWgsl[] = R"(
struct Block { shade : vec4f }
@group(3) @binding(0) var<uniform> block : Block;
struct Out { @builtin(position) position : vec4f, @location(0) shade : vec4f }
@vertex
fn main(@builtin(vertex_index) index : u32) -> Out {
    var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    var out : Out;
    out.position = vec4f(corners[index], 0.0, 1.0);
    out.shade = block.shade;
    return out;
}
)";

constexpr const char kRingProbeFragWgsl[] = R"(
@fragment
fn main(@location(0) shade : vec4f) -> @location(0) vec4f {
    return shade;
}
)";

std::shared_ptr<const std::vector<uint8_t>> WgslBytes(const char* text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    return std::make_shared<const std::vector<uint8_t>>(bytes, bytes + std::strlen(text));
}

// Sets an environment variable for one scope; the device reads it at creation.
class ScopedEnvironmentVariable
{
  public:
    ScopedEnvironmentVariable(const char* name, const char* value) : m_Name(name)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
    ~ScopedEnvironmentVariable()
    {
#ifdef _WIN32
        _putenv_s(m_Name, "");
#else
        unsetenv(m_Name);
#endif
    }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

  private:
    const char* m_Name;
};

bool LineContains(const std::string& line, std::string_view fragment)
{
    return line.find(fragment) != std::string::npos;
}

} // namespace

// More draws in one frame than the push-constant ring has slots: the draws past
// the limit are refused and the frame stays valid. The overflow lands in a
// second render pass, where group 3 has not been bound yet, which is where a
// recorded draw without its block invalidates the whole command buffer (the
// browser's "No bind group set at group index 3"). The next frame reports the
// refusal once, naming the limit, the count, the pipeline and the pass.
TEST(WebGpuDeviceSmoke, PushConstantRingRefusesDrawsPastItsSlotsAndKeepsTheFrameValid)
{
    const ScopedEnvironmentVariable emulate("GE_WEBGPU_EMULATE_PUSH_CONSTANTS", "1");
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = WgslBytes(kRingProbeVertWgsl);
    pipelineDesc.PixelShader = WgslBytes(kRingProbeFragWgsl);
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.PushConstants.Size = 4 * sizeof(float);
    pipelineDesc.PushConstants.StageMask = kShaderStageVertex;
    pipelineDesc.DebugName = "WebGpuSmoke.RingProbe";
    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.ColorCount = 1;
    const PipelineHandle pipeline =
        device->GetOrCreateGraphicsPipeline(device->InternGraphicsPipeline(pipelineDesc), formatKey);
    ASSERT_TRUE(pipeline.IsValid()) << "ring probe pipeline creation failed";

    // Two 64 x 1 targets side by side in one readback: the accepted pass draws
    // the left half, the overflowing pass the right.
    constexpr uint32_t kSize = 64;
    TextureDesc targetDesc{};
    targetDesc.width = kSize;
    targetDesc.height = 1;
    targetDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    targetDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    const TextureHandle accepted = device->CreateTexture(targetDesc);
    const TextureHandle overflow = device->CreateTexture(targetDesc);
    ASSERT_TRUE(accepted.IsValid() && overflow.IsValid());

    constexpr uint32_t kRingSlots = kWebGpuPushConstantSlotsPerFrame;
    constexpr uint32_t kRefused = 37;
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    constexpr const char* kLiveLine = "ring probe capture live";

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
        Logger::Log::Info(kLiveLine);
        ASSERT_TRUE(device->BeginFrame());
        auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
        commandList->Begin();
        const auto drawPass = [&](const char* name, TextureHandle target, uint32_t draws, const float* shade) {
            commandList->BeginEvent(name);
            RenderPassDesc pass{};
            pass.colorTargets[0] = target;
            pass.colorTargetCount = 1;
            pass.clearColor[0] = true;
            pass.clearColorValue[0][2] = 1.0f;
            pass.clearColorValue[0][3] = 1.0f;
            pass.depthTarget = INVALID_TEXTURE_HANDLE;
            commandList->BeginRenderPass(pass);
            commandList->SetPipeline(pipeline);
            commandList->SetViewport(0.0f, 0.0f, static_cast<float>(kSize), 1.0f);
            commandList->SetScissor(0, 0, kSize, 1);
            commandList->SetConstants(0, 0, 4 * sizeof(float), shade);
            for (uint32_t draw = 0; draw < draws; ++draw)
                commandList->Draw(3, 1);
            commandList->EndRenderPass();
            commandList->EndEvent();
        };
        drawPass("RingProbeAccepted", accepted, kRingSlots, red);
        drawPass("RingProbeOverflow", overflow, kRefused, green);
        const std::vector<uint8_t> overflowPixels = ReadTarget(*device, *commandList, overflow, kSize, 1);
        auto readList = device->CreateCommandList(IDevice::QueueType::Graphics);
        readList->Begin();
        const std::vector<uint8_t> acceptedPixels = ReadTarget(*device, *readList, accepted, kSize, 1);
        ASSERT_EQ(overflowPixels.size(), kSize * 4u);
        ASSERT_EQ(acceptedPixels.size(), kSize * 4u);
        // The command buffer executed: the accepted pass is red, and the
        // overflowing pass shows its clear (blue) with none of its refused
        // draws (green). An invalid command buffer leaves both unwritten.
        EXPECT_EQ(acceptedPixels[0], 255u) << "the accepted draws did not land";
        EXPECT_EQ(acceptedPixels[2], 0u);
        EXPECT_EQ(overflowPixels[1], 0u) << "a refused draw was recorded";
        EXPECT_EQ(overflowPixels[2], 255u) << "the overflowing pass's clear did not land";
        device->Present();
        ASSERT_TRUE(device->BeginFrame());
        device->Present();
        Logger::Log::Flush();
    }
    ASSERT_EQ(std::count_if(lines.begin(), lines.end(),
                            [&](const std::string& l) { return l == kLiveLine; }),
              1)
        << "the log capture was not live";
    const auto report = std::find_if(lines.begin(), lines.end(), [&](const std::string& l) {
        return LineContains(l, "refused " + std::to_string(kRefused) + " draws or dispatches last frame");
    });
    ASSERT_NE(report, lines.end()) << "no refusal report naming the count";
    EXPECT_TRUE(LineContains(*report, std::to_string(kRingSlots) + " slots")) << *report;
    EXPECT_TRUE(LineContains(*report, "WebGpuSmoke.RingProbe")) << *report;
    EXPECT_TRUE(LineContains(*report, "RingProbeOverflow")) << *report;
    EXPECT_EQ(std::count_if(lines.begin(), lines.end(),
                            [](const std::string& l) { return LineContains(l, "draws or dispatches last frame"); }),
              1)
        << "the refusal is reported once per run of refusing frames";

    device->DestroyTexture(overflow);
    device->DestroyTexture(accepted);
    device->Shutdown();
}
