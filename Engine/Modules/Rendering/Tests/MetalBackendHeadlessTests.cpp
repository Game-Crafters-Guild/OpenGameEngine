// Headless Metal backend tests: resources, copies, sync (milestone 2) and the
// SPIRV-Cross pipeline + argument-buffer descriptor path (milestone 3).

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include "Mathematics/HalfFloat.h"

#include "../Source/Metal/MetalArgumentBufferLayout.h"
#include "../Source/Metal/MetalDevice.h"
#include "../Source/Metal/MetalShaderTranslator.h"
#include "PushBlockMslLayout.h"

#if RENDERING_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

#include <objc/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

std::unique_ptr<IDevice> CreateHeadlessMetalDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Metal;
    desc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && device->Initialize(desc) && device->GetAPI() == GraphicsAPI::Metal)
    {
        return device;
    }
    return nullptr;
}

#if RENDERING_HAS_SHADERC
std::vector<uint8_t> CompileGlsl(const char* source, shaderc_shader_kind kind, const char* name)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    const auto result = compiler.CompileGlslToSpv(source, kind, name, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        ADD_FAILURE() << "GLSL compile failed: " << result.GetErrorMessage();
        return {};
    }
    const auto* begin = reinterpret_cast<const uint8_t*>(result.cbegin());
    const auto* end = reinterpret_cast<const uint8_t*>(result.cend());
    return std::vector<uint8_t>(begin, end);
}
#endif

// Metal's own answer for the GPU the engine's device runs on.
bool GpuSamplesBlockCompression(IDevice& device)
{
    return static_cast<MetalDevice&>(device).GetMTLDevice()->supportsBCTextureCompression();
}

constexpr size_t kBc7BlockBytes = 16;
using Bc7Block = std::array<uint8_t, kBc7BlockBytes>;

void WriteBlockBits(Bc7Block& block, uint32_t& bit, uint32_t value, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i, ++bit)
    {
        if ((value >> i) & 1u)
        {
            block[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        }
    }
}

// A BC7 mode 6 block whose two endpoints are the same color and whose sixteen
// indices are 0, so every texel decodes to exactly (r, g, b, a). Mode 6 keeps
// 7 bits per channel and one shared p-bit per endpoint as the lowest bit, so
// the four channels must all be odd.
Bc7Block SolidBc7Block(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Bc7Block block{};
    uint32_t bit = 0;
    constexpr uint32_t kMode6 = 1u << 6;
    WriteBlockBits(block, bit, kMode6, 7);
    for (const uint8_t channel : {r, g, b, a})
    {
        WriteBlockBits(block, bit, channel >> 1, 7); // endpoint 0
        WriteBlockBits(block, bit, channel >> 1, 7); // endpoint 1
    }
    WriteBlockBits(block, bit, 1u, 1); // p-bit, endpoint 0
    WriteBlockBits(block, bit, 1u, 1); // p-bit, endpoint 1
    // The 63 index bits stay 0.
    return block;
}

// The color a test chain's block of shade `shade` decodes to in `format`.
// For BC7 every channel stays odd, as SolidBc7Block needs. BC4 stores only red,
// and the sampler returns it as (red, 0, 0, 1).
std::array<uint8_t, 4> BcShadeColor(TextureFormat format, uint8_t shade)
{
    if (format == TextureFormat::BC4_UNORM)
    {
        return {shade, 0, 0, 255};
    }
    return {shade, static_cast<uint8_t>(256 - shade), 129, 255};
}

// One block of `format` (BC7_UNORM, 16 bytes, or BC4_UNORM, 8 bytes) whose
// every texel decodes to exactly BcShadeColor(format, shade). A BC4 block
// whose two endpoints are equal is in its six-value mode, where index 0 is
// endpoint 0 itself; its 48 index bits stay 0.
std::vector<uint8_t> SolidBcBlock(TextureFormat format, uint8_t shade)
{
    if (format == TextureFormat::BC4_UNORM)
    {
        std::vector<uint8_t> block(BytesPerBlock(format), 0);
        block[0] = shade; // endpoint 0
        block[1] = shade; // endpoint 1
        return block;
    }
    const std::array<uint8_t, 4> color = BcShadeColor(format, shade);
    const Bc7Block solid = SolidBc7Block(color[0], color[1], color[2], color[3]);
    return std::vector<uint8_t>(solid.begin(), solid.end());
}

struct BcMipChain
{
    std::vector<std::vector<uint8_t>> Levels; // block bytes of each level, as uploaded
    std::vector<uint8_t> FirstBlockShades;    // the shade of each level's top-left block
    std::vector<uint8_t> LastBlockShades;     // the shade of each level's bottom-right block
};

// Fills a `format` texture (BC7_UNORM or BC4_UNORM) of `width` x `height` and
// `mipLevels` levels, every 4x4 block a distinct solid color, the way
// TextureService uploads a cooked BC artifact: UploadTextureMips, each level
// tightly packed.
BcMipChain UploadDistinctBcMipChain(IDevice& device, TextureHandle texture, TextureFormat format, uint32_t width,
                                    uint32_t height, uint32_t mipLevels)
{
    BcMipChain chain;
    chain.Levels.resize(mipLevels);
    std::vector<TextureMipUploadEntry> entries(mipLevels);
    uint8_t shade = 1;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        const uint32_t levelWidth = std::max(1u, width >> mip);
        const uint32_t levelHeight = std::max(1u, height >> mip);
        const uint32_t blocks = ((levelWidth + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim) *
                                ((levelHeight + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim);
        chain.FirstBlockShades.push_back(shade);
        for (uint32_t block = 0; block < blocks; ++block, shade += 2)
        {
            const std::vector<uint8_t> solid = SolidBcBlock(format, shade);
            chain.Levels[mip].insert(chain.Levels[mip].end(), solid.begin(), solid.end());
        }
        chain.LastBlockShades.push_back(static_cast<uint8_t>(shade - 2));
        entries[mip] = {chain.Levels[mip].data(), chain.Levels[mip].size(), levelWidth, levelHeight};
    }
    UploadTextureMips(&device, texture, entries.data(), mipLevels, format);
    return chain;
}

} // namespace

TEST(MetalResources, BufferUpdateCopyReadback)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr size_t kSize = 256;
    BufferDesc desc{};
    desc.size = kSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle src = device->CreateBuffer(desc);
    BufferHandle dst = device->CreateBuffer(desc);
    ASSERT_TRUE(src.IsValid());
    ASSERT_TRUE(dst.IsValid());

    std::vector<uint8_t> pattern(kSize);
    for (size_t i = 0; i < kSize; ++i)
    {
        pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    device->UpdateBuffer(src, 0, kSize, pattern.data());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBuffer(src, dst, kSize);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(dst));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, pattern.data(), kSize), 0);
    device->UnmapBuffer(dst);

    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Shutdown();
}

// UpdateBufferRanges is all or nothing: a batch that carries one invalid
// non-empty range (out of bounds, offset + size wrapping, or no data) writes
// none of its ranges, as on Vulkan. GPUScene uploads its instance and scatter
// rows this way, so a valid prefix landing ahead of a bad range would leave a
// half-applied batch on the GPU.
TEST(MetalResources, UpdateBufferRangesWritesAWholeBatchOrNothing)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr size_t kSize = 256;
    BufferDesc desc{};
    desc.size = kSize;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    std::array<uint8_t, kSize> expected{};
    expected.fill(0xA5);
    device->UpdateBuffer(buffer, 0, expected.size(), expected.data());

    // A valid sparse batch lands every range and leaves the gaps alone; an
    // empty range with no data is skipped, not rejected.
    const std::array<uint8_t, 4> value{1, 2, 3, 4};
    const std::array<BufferUpdateRange, 4> valid{{
        {252, value.size(), value.data()}, {0, value.size(), value.data()},
        {63, value.size(), value.data()}, {128, 0, nullptr}}};
    device->UpdateBufferRanges(buffer, valid);
    for (const BufferUpdateRange& range : valid)
    {
        if (range.size != 0)
        {
            std::memcpy(expected.data() + range.offset, range.data, range.size);
        }
    }
    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(buffer));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, expected.data(), kSize), 0) << "the valid sparse batch did not land as written";

    // Each batch below leads with a valid range; the later bad range must stop
    // it from landing. The buffer is reset before each batch, so a failure
    // names its own batch and not one an earlier batch broke.
    const std::array<uint8_t, 4> badValue{9, 9, 9, 9};
    const std::array<BufferUpdateRange, 2> pastTheEnd{{
        {0, badValue.size(), badValue.data()}, {kSize - 1, value.size(), value.data()}}};
    const std::array<BufferUpdateRange, 2> wrapping{{
        {0, badValue.size(), badValue.data()}, {SIZE_MAX, value.size(), value.data()}}};
    const std::array<BufferUpdateRange, 2> noData{{
        {0, badValue.size(), badValue.data()}, {32, 1, nullptr}}};
    const std::array<std::pair<const char*, std::span<const BufferUpdateRange>>, 3> rejected{{
        {"a range past the end", pastTheEnd}, {"a wrapping offset", wrapping}, {"a range with no data", noData}}};
    for (const auto& [why, batch] : rejected)
    {
        device->UpdateBuffer(buffer, 0, expected.size(), expected.data());
        device->UpdateBufferRanges(buffer, batch);
        EXPECT_EQ(std::memcmp(mapped, expected.data(), kSize), 0)
            << "a batch with " << why << " still wrote its valid prefix";
    }

    device->UnmapBuffer(buffer);
    device->DestroyBuffer(buffer);
    device->Shutdown();
}

// Metal buffers are shared storage. The device reports where one landed, so
// MeshGPURegistry's pool-residency log (which stays silent while the backend
// cannot answer) reports on Metal as it does on Vulkan.
TEST(MetalResources, BufferMemoryResidencyReportsSharedStorage)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    EXPECT_FALSE(device->GetBufferMemoryResidency(BufferHandle{}).reported)
        << "an unknown buffer must be unreported, not a buffer with no device-local memory";

    BufferDesc desc{};
    desc.size = 64 * 1024;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
    desc.debugName = "MetalResidencyProbe";
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    const IDevice::BufferMemoryResidency residency = device->GetBufferMemoryResidency(buffer);
    MTL::Device* mtl = static_cast<MetalDevice&>(*device).GetMTLDevice();
    EXPECT_TRUE(residency.reported);
    EXPECT_TRUE(residency.hostVisible) << "shared storage is CPU-visible";
    EXPECT_TRUE(residency.hostCoherent) << "shared storage needs no flush";
    EXPECT_EQ(residency.deviceLocal, mtl->hasUnifiedMemory())
        << "shared storage is the GPU's own memory exactly when memory is unified";
    EXPECT_GT(residency.heapSizeBytes, 0u);
    const auto& topology = device->GetCapabilities().memoryTopology;
    ASSERT_TRUE(topology.has_value());
    if (residency.deviceLocal)
    {
        EXPECT_EQ(residency.heapSizeBytes, topology->deviceLocalHeapBytesTotal)
            << "a device-local buffer lands in the heap the topology declares";
    }

    device->DestroyBuffer(buffer);
    EXPECT_FALSE(device->GetBufferMemoryResidency(buffer).reported) << "a destroyed buffer must be unreported";
    device->Shutdown();
}

TEST(MetalResources, FillBufferArbitraryU32)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr size_t kCount = 64;
    BufferDesc desc{};
    desc.size = kCount * 4;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    BufferHandle buffer = device->CreateBuffer(desc);
    ASSERT_TRUE(buffer.IsValid());

    constexpr uint32_t kValue = 0xDEADBEEFu; // not byte-replicable: exercises the compute path
    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->FillBuffer(buffer, 0, desc.size, kValue);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint32_t* mapped = static_cast<const uint32_t*>(device->MapBuffer(buffer));
    ASSERT_NE(mapped, nullptr);
    for (size_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(mapped[i], kValue) << "element " << i;
    }
    device->DestroyBuffer(buffer);
    device->Shutdown();
}

TEST(MetalResources, TextureUploadAndReadback)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kDim = 4;
    TextureDesc texDesc{};
    texDesc.width = kDim;
    texDesc.height = kDim;
    texDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    texDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferSrc |
                                          TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(texDesc);
    ASSERT_TRUE(texture.IsValid());

    std::vector<uint8_t> pixels(kDim * kDim * 4);
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        pixels[i] = static_cast<uint8_t>(i * 5 + 1);
    }

    IDevice::TextureUploadRequest request{};
    request.Texture = texture;
    request.Pixels = pixels.data();
    request.Width = kDim;
    request.Height = kDim;
    request.RowPitchBytes = kDim * 4;
    const IDevice::GpuSyncToken token = device->SubmitTextureUploads(&request, 1);
    ASSERT_TRUE(token.IsValid());
    ASSERT_TRUE(device->WaitGpuSyncToken(token, 5'000'000'000ull));

    BufferDesc readbackDesc{};
    readbackDesc.size = pixels.size();
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyTextureToBuffer(texture, readback, kDim, kDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, pixels.data(), pixels.size()), 0);

    device->DestroyBuffer(readback);
    device->DestroyTexture(texture);
    device->Shutdown();
}

// The texture service publishes this capability to the asset layer, which
// adopts a block-compressed cook artifact only when it is true: a device that
// reports false loads every texture as RGBA8, four to eight times the bytes.
TEST(MetalDeviceCapabilities, BlockCompressionIsWhatTheGpuReports)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    EXPECT_EQ(device->GetCapabilities().supportsTextureCompressionBC, GpuSamplesBlockCompression(*device));
    device->Shutdown();
}

namespace
{
// A cooked BC level is tightly packed rows of 4x4 blocks (row pitch 0 in
// UploadTextureMips), ceil(width / 4) blocks to a row. Every level must land
// whole in both directions. The 10x6 level is two block rows of three blocks,
// so a row pitch that rounds the width down, or counts the wrong number of
// bytes per block, moves the second row. The 2x1 and 1x1 tails are one partial
// block each. The region is non-square, so a swapped width and height cannot
// pass.
void ExpectTightlyPackedBcMipChainRoundTrips(TextureFormat format)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    if (!GpuSamplesBlockCompression(*device))
    {
        device->Shutdown();
        GTEST_SKIP() << "This GPU does not sample BC";
    }

    constexpr uint32_t kWidth = 20;
    constexpr uint32_t kHeight = 12;
    constexpr uint32_t kMipLevels = 5; // 20x12, 10x6, 5x3, 2x1, 1x1
    TextureDesc texDesc{};
    texDesc.width = kWidth;
    texDesc.height = kHeight;
    texDesc.mipLevels = kMipLevels;
    texDesc.format = static_cast<uint32_t>(format);
    texDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferSrc |
                                          TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(texDesc);
    ASSERT_TRUE(texture.IsValid());
    const std::vector<std::vector<uint8_t>> levels =
        UploadDistinctBcMipChain(*device, texture, format, kWidth, kHeight, kMipLevels).Levels;

    size_t totalBytes = 0;
    for (const auto& level : levels)
    {
        totalBytes += level.size();
    }
    BufferDesc readbackDesc{};
    readbackDesc.size = totalBytes;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    size_t offset = 0;
    for (uint32_t mip = 0; mip < kMipLevels; ++mip)
    {
        cl->CopyTextureSubresourceToBuffer(texture, mip, 0, readback, std::max(1u, kWidth >> mip),
                                           std::max(1u, kHeight >> mip), 0, 0, offset, 0);
        offset += levels[mip].size();
    }
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    offset = 0;
    for (uint32_t mip = 0; mip < kMipLevels; ++mip)
    {
        EXPECT_EQ(std::memcmp(mapped + offset, levels[mip].data(), levels[mip].size()), 0) << "mip " << mip;
        offset += levels[mip].size();
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyTexture(texture);
    device->Shutdown();
}
} // namespace

// BC7: 16-byte blocks.
TEST(MetalResources, TightlyPackedBc7MipChainUploadsAndReadsBackEveryLevel)
{
    ExpectTightlyPackedBcMipChainRoundTrips(TextureFormat::BC7_UNORM);
}

// BC4: 8-byte blocks, as BC1 has. A row that sizes every block at 16 bytes
// passes for BC7 and fails here.
TEST(MetalResources, TightlyPackedBc4MipChainUploadsAndReadsBackEveryLevel)
{
    ExpectTightlyPackedBcMipChainRoundTrips(TextureFormat::BC4_UNORM);
}

// A zero slice pitch means tightly packed slices: ceil(height / 4) rows of
// blocks, not `height` rows. Only a copy of more than one slice reads it, so
// this copies an 8x8x2 BC4 volume (two rows of two blocks per slice) up and
// back down in one copy each way, with zero row and slice pitches. The source
// and readback buffers are four slices long, so a slice pitch counted in texel
// rows still copies inside them and the test fails on the bytes, not on a
// bounds check.
TEST(MetalResources, TightlyPackedBc4VolumeCopiesEverySlice)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    if (!GpuSamplesBlockCompression(*device))
    {
        device->Shutdown();
        GTEST_SKIP() << "This GPU does not sample BC";
    }

    constexpr uint32_t kDim = 8;
    constexpr uint32_t kSlices = 2;
    constexpr size_t kSliceBytes = 2 * 2 * 8; // two rows of two 8-byte blocks
    constexpr size_t kBufferBytes = kSliceBytes * kDim; // room for slices kDim rows apart
    TextureDesc texDesc{};
    texDesc.width = kDim;
    texDesc.height = kDim;
    texDesc.depth = kSlices;
    texDesc.format = static_cast<uint32_t>(TextureFormat::BC4_UNORM);
    texDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferSrc |
                                          TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(texDesc);
    ASSERT_TRUE(texture.IsValid());

    std::vector<uint8_t> blocks(kBufferBytes, 0);
    for (size_t block = 0; block < kSlices * kSliceBytes / 8; ++block)
    {
        const std::vector<uint8_t> solid = SolidBcBlock(TextureFormat::BC4_UNORM, static_cast<uint8_t>(block * 2 + 1));
        std::copy(solid.begin(), solid.end(), blocks.begin() + static_cast<std::ptrdiff_t>(block * 8));
    }
    BufferDesc bufferDesc{};
    bufferDesc.size = kBufferBytes;
    bufferDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle source = device->CreateBuffer(bufferDesc);
    BufferHandle readback = device->CreateBuffer(bufferDesc);
    ASSERT_TRUE(source.IsValid());
    ASSERT_TRUE(readback.IsValid());
    device->UpdateBuffer(source, 0, kBufferBytes, blocks.data());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(source, texture, 0, 0, kDim, kDim, 0, 0, kSlices, 0);
    cl->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, kDim, kDim, 0, 0, 0, 0, kSlices, 0);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    for (uint32_t slice = 0; slice < kSlices; ++slice)
    {
        EXPECT_EQ(std::memcmp(mapped + slice * kSliceBytes, blocks.data() + slice * kSliceBytes, kSliceBytes), 0)
            << "slice " << slice;
    }
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyBuffer(source);
    device->DestroyTexture(texture);
    device->Shutdown();
}

TEST(MetalResources, TimelineSemaphoreSignalAndWait)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    SemaphoreHandle semaphore = device->CreateTimelineSemaphore(0);
    ASSERT_TRUE(semaphore.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Graphics, {cl.get()}, {}, {{semaphore, 5}}));
    ASSERT_TRUE(device->WaitTimelineSemaphoreValue(semaphore, 5, 5'000'000'000ull));

    uint64_t value = 0;
    ASSERT_TRUE(device->GetTimelineSemaphoreValue(semaphore, value));
    EXPECT_GE(value, 5u);

    // Cross-queue: compute waits on the value the graphics queue published.
    auto computeCl = device->CreateCommandList(IDevice::QueueType::Compute);
    computeCl->Begin();
    computeCl->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Compute, {computeCl.get()}, {{semaphore, 5}},
                                    {{semaphore, 6}}));
    ASSERT_TRUE(device->WaitTimelineSemaphoreValue(semaphore, 6, 5'000'000'000ull));

    device->DestroySemaphore(semaphore);
    device->Shutdown();
}

// The draw stream reduces a GPU->CPU readback mirror only when this poll says the
// newest graphics submission has completed: it must read false while a copy into
// the mirror is still pending, and never read true before the copy has landed.
TEST(MetalResources, PreviousFrameGraphicsCompleteFollowsTheNewestGraphicsSubmission)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    // Nothing submitted yet: no mirror can be stale.
    EXPECT_TRUE(device->IsPreviousFrameGraphicsComplete());

    // Large enough that the copy is still running long after Metal schedules its
    // command buffer (about 0.5 ms on an M4 Max), while the poll below reads in
    // microseconds: a count dropped at commit or schedule time instead of at
    // completion reads complete before the copy has landed.
    constexpr size_t kSize = size_t(64) << 20;
    BufferDesc sourceDesc{};
    sourceDesc.size = kSize;
    sourceDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle source = device->CreateBuffer(sourceDesc);
    BufferHandle mirror = device->CreateReadbackBuffer(kSize, "PreviousFrameComplete.Mirror");
    ASSERT_TRUE(source.IsValid());
    ASSERT_TRUE(mirror.IsValid());
    // Both mapped before the held submission: a fatal check between that
    // submission and the release below would leave the submit worker blocked on
    // `gate`, and the device teardown would wait on it forever.
    auto* sourceBytes = static_cast<uint8_t*>(device->MapBuffer(source));
    auto* mirrorBytes = static_cast<uint8_t*>(device->MapBuffer(mirror));
    ASSERT_NE(sourceBytes, nullptr);
    ASSERT_NE(mirrorBytes, nullptr);
    std::memset(mirrorBytes, 0, kSize);

    // A copy lands in no defined order, so the check samples the whole range:
    // 4,096 evenly spaced bytes and the last one.
    const auto mirrorHolds = [mirrorBytes](uint8_t value)
    {
        for (size_t offset = 0; offset < kSize; offset += kSize / 4096)
        {
            if (mirrorBytes[offset] != value)
            {
                return false;
            }
        }
        return mirrorBytes[kSize - 1] == value;
    };
    // Spins without sleeping, so the first true is read as soon as it can be.
    const auto pollUntilComplete = [&device]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool complete = device->IsPreviousFrameGraphicsComplete();
        while (!complete && std::chrono::steady_clock::now() < deadline)
        {
            complete = device->IsPreviousFrameGraphicsComplete();
        }
        return complete;
    };

    // The copy waits on `gate`, so the submit worker holds it uncommitted: it is
    // submitted graphics work that cannot have run.
    constexpr uint8_t kHeldValue = 0x5A;
    std::memset(sourceBytes, kHeldValue, kSize);
    SemaphoreHandle gate = device->CreateTimelineSemaphore(0);
    ASSERT_TRUE(gate.IsValid());
    auto copy = device->CreateCommandList(IDevice::QueueType::Graphics);
    copy->Begin();
    copy->CopyBuffer(source, mirror, kSize);
    copy->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Graphics, {copy.get()}, {{gate, 1}}, {}));

    EXPECT_FALSE(device->IsPreviousFrameGraphicsComplete());
    EXPECT_TRUE(mirrorHolds(0)) << "the held copy ran";

    auto release = device->CreateCommandList(IDevice::QueueType::Compute);
    release->Begin();
    release->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Compute, {release.get()}, {}, {{gate, 1}}));

    // Completion is reported from the command buffer's completion handler, which
    // Metal runs asynchronously after the GPU finishes.
    ASSERT_TRUE(pollUntilComplete()) << "still incomplete 5 s after the gate opened";
    EXPECT_TRUE(mirrorHolds(kHeldValue)) << "reported complete before the held copy landed";
    device->WaitForIdle();

    // Unheld copies, polled from the moment each is submitted.
    for (uint8_t value = 1; value <= 3; ++value)
    {
        std::memset(sourceBytes, value, kSize);
        auto next = device->CreateCommandList(IDevice::QueueType::Graphics);
        next->Begin();
        next->CopyBuffer(source, mirror, kSize);
        next->End();
        ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Graphics, {next.get()}, {}, {}));
        ASSERT_TRUE(pollUntilComplete()) << "copy " << static_cast<int>(value) << " still incomplete after 5 s";
        EXPECT_TRUE(mirrorHolds(value)) << "reported complete before copy " << static_cast<int>(value) << " landed";
        device->WaitForIdle();
    }

    device->UnmapBuffer(mirror);
    device->UnmapBuffer(source);
    device->DestroySemaphore(gate);
    device->DestroyBuffer(source);
    device->DestroyBuffer(mirror);
    device->Shutdown();
}

#if RENDERING_HAS_SHADERC

TEST(MetalSpirvPipeline, TranslatorProducesValidMsl)
{
    static const char* kComp = R"(
#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Out { uint values[]; } uOut;
layout(push_constant) uniform Push { uint base; } uPush;
void main() { uOut.values[gl_GlobalInvocationID.x] = uPush.base + gl_GlobalInvocationID.x; }
)";
    std::vector<uint8_t> spv = CompileGlsl(kComp, shaderc_compute_shader, "translate.comp");
    ASSERT_FALSE(spv.empty());

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);

    const std::vector<const DescriptorSetLayoutDesc*> layouts{&layout};
    const MetalShaderTranslation translation =
        TranslateSpirvToMsl(spv, MetalShaderStage::Compute, layouts, nullptr);
    ASSERT_TRUE(translation.Success) << "translation error: " << translation.Error;
    EXPECT_EQ(translation.LocalSizeX, 16u);
    EXPECT_NE(translation.Msl.find("main0"), std::string::npos) << translation.Msl;
}

// The whole DescriptorType::AccelerationStructure chain in one pass: SPIR-V
// reflection must classify a descriptor-bound accelerationStructureEXT as its
// own type (it used to fall through to UniformBuffer, silently), the Metal
// argument-buffer layout must give it a slot in the shared id space, and the
// translator must emit a real raytracing::acceleration_structure member at
// exactly that [[id(n)]]. A regression in any link renders garbage without any
// API validation error, which is why the whole chain is asserted end to end.
TEST(MetalSpirvPipeline, DescriptorBoundAccelerationStructureTranslates)
{
    static const char* kComp = R"(
#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 8) in;
layout(std430, set = 0, binding = 0) buffer Out { vec4 values[]; } uOut;
layout(set = 0, binding = 1) uniform sampler2D uTex;
layout(set = 0, binding = 2) uniform accelerationStructureEXT uTlas;
void main()
{
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsOpaqueEXT, 0xFFu,
                          vec3(0.0), 0.01, vec3(0.0, 0.0, 1.0), 100.0);
    while (rayQueryProceedEXT(rq)) { }
    float t = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT
                  ? -1.0 : rayQueryGetIntersectionTEXT(rq, true);
    uOut.values[gl_GlobalInvocationID.x] = vec4(textureLod(uTex, vec2(0.5), 0.0).rgb, t);
}
)";
    const std::vector<uint8_t> spv = CompileGlsl(kComp, shaderc_compute_shader, "rayquery.comp");
    ASSERT_FALSE(spv.empty());

#if RENDERING_ENABLE_SPIRV_REFLECTION
    ReflectionOptions opts{};
    StageReflectionResult reflected{};
    std::string reflectError;
    ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute,
                             reinterpret_cast<const uint32_t*>(spv.data()), spv.size() / 4, opts,
                             reflected, &reflectError))
        << reflectError;
    const ShaderMeta meta = MergeStages({reflected});
    ASSERT_EQ(meta.Sets.size(), 1u);
    const DescriptorBindingMeta* tlasMeta = nullptr;
    for (const auto& binding : meta.Sets[0].Bindings)
    {
        if (binding.Binding == 2)
            tlasMeta = &binding;
    }
    ASSERT_NE(tlasMeta, nullptr);
    EXPECT_EQ(tlasMeta->Type, ShaderMetaBindingType::kAccelerationStructure);
    EXPECT_EQ(tlasMeta->Name, "uTlas");

    const std::vector<DescriptorSetLayoutDesc> builtLayouts = MaterialBuilder::BuildSetLayouts(meta);
    ASSERT_EQ(builtLayouts.size(), 1u);
    const DescriptorBinding* builtTlas = nullptr;
    for (const auto& binding : builtLayouts[0].bindings)
    {
        if (binding.binding == 2)
            builtTlas = &binding;
    }
    ASSERT_NE(builtTlas, nullptr);
    EXPECT_EQ(builtTlas->type, DescriptorType::AccelerationStructure);
#endif

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    DescriptorBinding sampled{};
    sampled.binding = 1;
    sampled.type = DescriptorType::CombinedImageSampler;
    sampled.shaderStages = kShaderStageCompute;
    DescriptorBinding tlas{};
    tlas.binding = 2;
    tlas.type = DescriptorType::AccelerationStructure;
    tlas.shaderStages = kShaderStageCompute;
    layout.bindings = {storage, sampled, tlas};

    // Slot ids come from one monotonic counter shared by every resource class:
    // b0 buffer -> 0, b1 texture+sampler -> 1,2, b2 acceleration structure -> 3.
    const MetalArgumentBufferLayout slots = ComputeMetalArgumentBufferLayout(layout);
    const MetalArgumentSlot* tlasSlot = slots.FindBinding(2);
    ASSERT_NE(tlasSlot, nullptr);
    EXPECT_EQ(tlasSlot->AccelerationStructureId, 3u);
    EXPECT_EQ(tlasSlot->BufferId, MetalArgumentSlot::kUnused);
    EXPECT_EQ(slots.TotalSlotCount, 4u);

    const std::vector<const DescriptorSetLayoutDesc*> layouts{&layout};
    const MetalShaderTranslation translation =
        TranslateSpirvToMsl(spv, MetalShaderStage::Compute, layouts, nullptr);
    ASSERT_TRUE(translation.Success) << "translation error: " << translation.Error;
    EXPECT_NE(translation.Msl.find("raytracing::acceleration_structure"), std::string::npos)
        << translation.Msl;
    EXPECT_NE(translation.Msl.find("uTlas [[id(3)]]"), std::string::npos) << translation.Msl;
}

TEST(MetalSpirvPipeline, TexturedQuadWithDescriptorSet)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kVert = R"(
#version 450
layout(location = 0) out vec2 vUV;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 0, binding = 1) uniform Tint { vec4 tint; } uTint;
void main()
{
    outColor = texture(uTex, vUV) * uTint.tint;
}
)";
    std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "quad.vert");
    std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "quad.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    // 4x4 solid green source texture, uploaded through the transfer queue.
    constexpr uint32_t kTexDim = 4;
    TextureDesc texDesc{};
    texDesc.width = kTexDim;
    texDesc.height = kTexDim;
    texDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    texDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(texDesc);
    ASSERT_TRUE(texture.IsValid());

    std::vector<uint8_t> pixels(kTexDim * kTexDim * 4);
    for (uint32_t i = 0; i < kTexDim * kTexDim; ++i)
    {
        pixels[i * 4 + 0] = 0;   // R
        pixels[i * 4 + 1] = 255; // G
        pixels[i * 4 + 2] = 0;   // B
        pixels[i * 4 + 3] = 255; // A
    }
    IDevice::TextureUploadRequest request{};
    request.Texture = texture;
    request.Pixels = pixels.data();
    request.Width = kTexDim;
    request.Height = kTexDim;
    request.RowPitchBytes = kTexDim * 4;
    const IDevice::GpuSyncToken token = device->SubmitTextureUploads(&request, 1);
    ASSERT_TRUE(device->WaitGpuSyncToken(token, 5'000'000'000ull));

    SamplerHandle sampler = device->CreateSampler(SamplerDesc::PointClamp());
    ASSERT_TRUE(sampler.IsValid());

    // Tint UBO: half intensity on green, so the readback proves the UBO and
    // the texture both flowed through the argument buffer.
    const float tint[4] = {1.0f, 0.5f, 1.0f, 1.0f};
    BufferDesc uboDesc{};
    uboDesc.size = sizeof(tint);
    uboDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
    BufferHandle ubo = device->CreateBuffer(uboDesc);
    ASSERT_TRUE(ubo.IsValid());
    device->UpdateBuffer(ubo, 0, sizeof(tint), tint);

    // Offscreen render target.
    constexpr uint32_t kRtDim = 64;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    DescriptorSetLayoutDesc layout{};
    {
        DescriptorBinding tex{};
        tex.binding = 0;
        tex.type = DescriptorType::CombinedImageSampler;
        tex.shaderStages = kShaderStageFragment;
        layout.bindings.push_back(tex);
        DescriptorBinding tintBinding{};
        tintBinding.binding = 1;
        tintBinding.type = DescriptorType::UniformBuffer;
        tintBinding.shaderStages = kShaderStageFragment;
        layout.bindings.push_back(tintBinding);
    }

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.descriptorSetLayouts = {layout};
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalSpirvQuad";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    setDesc.debugName = "MetalSpirvQuadSet";
    DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    {
        DescriptorSetUpdate update{};
        update.binding = 0;
        update.type = DescriptorType::CombinedImageSampler;
        update.textures = {texture};
        update.samplers = {sampler};
        device->UpdateDescriptorSet(set, update);
    }
    device->UpdateBufferBinding(set, 1, ubo, 0, sizeof(tint));

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f; // red clear (overdrawn by the quad)
    pass.clearColorValue[0][1] = 0.0f;
    pass.clearColorValue[0][2] = 0.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    cl->BindDescriptorSet(0, set, pipeline);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->Draw(3);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    // Center pixel, BGRA: green 255 * tint 0.5 = ~128.
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[0], 0u) << "B";
    EXPECT_NEAR(center[1], 128, 2) << "G (texture * UBO tint)";
    EXPECT_EQ(center[2], 0u) << "R";
    EXPECT_EQ(center[3], 255u) << "A";

    device->DestroyBuffer(readback);
    device->DestroyBuffer(ubo);
    device->DestroyDescriptorSet(set);
    device->DestroySampler(sampler);
    device->DestroyTexture(rt);
    device->DestroyTexture(texture);
    device->Shutdown();
}

namespace
{
// A BC texture samples as the colors its blocks encode. The first and the last
// block of each level of the chain the readback tests upload are drawn through
// texelFetch and read back as texels, so the upload is checked through the
// sampler's BC decode, not only through a copy that a matching readback bug
// could cancel out.
void ExpectBcMipChainSamplesAsItsEncodedColors(TextureFormat format)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    if (!GpuSamplesBlockCompression(*device))
    {
        device->Shutdown();
        GTEST_SKIP() << "This GPU does not sample BC";
    }

    static const char* kVert = R"(
#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
// x: the mip level; y, z: the level's bottom-right texel.
layout(set = 0, binding = 1) uniform Level { ivec4 level; } uLevel;
void main()
{
    // The left half of the 4x4 target fetches the level's first texel, the
    // right half its last.
    ivec2 texel = gl_FragCoord.x < 2.0 ? ivec2(0, 0) : uLevel.level.yz;
    outColor = texelFetch(uTex, texel, uLevel.level.x);
}
)";
    const std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "bc.vert");
    const std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "bc.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    constexpr uint32_t kWidth = 20;
    constexpr uint32_t kHeight = 12;
    constexpr uint32_t kMipLevels = 5; // 20x12, 10x6, 5x3, 2x1, 1x1
    TextureDesc texDesc{};
    texDesc.width = kWidth;
    texDesc.height = kHeight;
    texDesc.mipLevels = kMipLevels;
    texDesc.format = static_cast<uint32_t>(format);
    texDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    TextureHandle texture = device->CreateTexture(texDesc);
    ASSERT_TRUE(texture.IsValid());
    const BcMipChain chain = UploadDistinctBcMipChain(*device, texture, format, kWidth, kHeight, kMipLevels);
    SamplerHandle sampler = device->CreateSampler(SamplerDesc::PointClamp());
    ASSERT_TRUE(sampler.IsValid());

    // std140 rounds the block to 16 bytes.
    constexpr size_t kLevelBlockBytes = 16;
    BufferDesc uboDesc{};
    uboDesc.size = kLevelBlockBytes;
    uboDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
    BufferHandle ubo = device->CreateBuffer(uboDesc);
    ASSERT_TRUE(ubo.IsValid());

    constexpr uint32_t kRtDim = 4;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    DescriptorSetLayoutDesc layout{};
    {
        DescriptorBinding tex{};
        tex.binding = 0;
        tex.type = DescriptorType::CombinedImageSampler;
        tex.shaderStages = kShaderStageFragment;
        layout.bindings.push_back(tex);
        DescriptorBinding level{};
        level.binding = 1;
        level.type = DescriptorType::UniformBuffer;
        level.shaderStages = kShaderStageFragment;
        layout.bindings.push_back(level);
    }
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.descriptorSetLayouts = {layout};
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalBcTexelFetch";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    setDesc.debugName = "MetalBcTexelFetchSet";
    DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    DescriptorSetUpdate update{};
    update.binding = 0;
    update.type = DescriptorType::CombinedImageSampler;
    update.textures = {texture};
    update.samplers = {sampler};
    device->UpdateDescriptorSet(set, update);
    device->UpdateBufferBinding(set, 1, ubo, 0, kLevelBlockBytes);

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    for (uint32_t mip = 0; mip < kMipLevels; ++mip)
    {
        const int32_t lastX = static_cast<int32_t>(std::max(1u, kWidth >> mip)) - 1;
        const int32_t lastY = static_cast<int32_t>(std::max(1u, kHeight >> mip)) - 1;
        const std::array<int32_t, 4> level{static_cast<int32_t>(mip), lastX, lastY, 0};
        device->UpdateBuffer(ubo, 0, sizeof(level), level.data());

        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        RenderPassDesc pass{};
        pass.colorTargets[0] = rt;
        pass.colorTargetCount = 1;
        pass.clearColor[0] = true;
        pass.depthTarget = INVALID_TEXTURE_HANDLE;
        pass.clearDepth = false;
        cl->BeginRenderPass(pass);
        cl->SetPipeline(pipeline);
        cl->BindDescriptorSet(0, set, pipeline);
        cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
        cl->SetScissor(0, 0, kRtDim, kRtDim);
        cl->Draw(3);
        cl->EndRenderPass();
        cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
        cl->End();
        device->ExecuteCommandLists({cl.get()});
        device->WaitForIdle();

        const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
        ASSERT_NE(mapped, nullptr);
        // BC7 mode 6 and BC4 store the endpoint exactly and index 0 weights it
        // fully, so the decoded texel equals the encoded color bit for bit.
        const std::array<uint8_t, 4> expectedFirst = BcShadeColor(format, chain.FirstBlockShades[mip]);
        const std::array<uint8_t, 4> expectedLast = BcShadeColor(format, chain.LastBlockShades[mip]);
        const uint8_t* row = mapped + static_cast<size_t>(kRtDim / 2) * kRtDim * 4;
        const uint8_t* first = row;
        const uint8_t* last = row + static_cast<size_t>(kRtDim - 1) * 4;
        for (size_t channel = 0; channel < expectedFirst.size(); ++channel)
        {
            EXPECT_EQ(first[channel], expectedFirst[channel]) << "mip " << mip << ", first block, channel " << channel;
            EXPECT_EQ(last[channel], expectedLast[channel]) << "mip " << mip << ", last block, channel " << channel;
        }
        device->UnmapBuffer(readback);
    }

    device->DestroyBuffer(readback);
    device->DestroyBuffer(ubo);
    device->DestroyDescriptorSet(set);
    device->DestroyPipeline(pipeline);
    device->DestroySampler(sampler);
    device->DestroyTexture(rt);
    device->DestroyTexture(texture);
    device->Shutdown();
}
} // namespace

// BC7: 16-byte blocks, mode 6.
TEST(MetalSpirvPipeline, Bc7TextureSamplesAsItsEncodedColors)
{
    ExpectBcMipChainSamplesAsItsEncodedColors(TextureFormat::BC7_UNORM);
}

// BC4: 8-byte blocks, sampled as (red, 0, 0, 1).
TEST(MetalSpirvPipeline, Bc4TextureSamplesAsItsEncodedColors)
{
    ExpectBcMipChainSamplesAsItsEncodedColors(TextureFormat::BC4_UNORM);
}

// Mesh-shader end-to-end: a GL_EXT_mesh_shader mesh stage emits a triangle that
// a solid-green fragment shader fills. Exercises the whole path — mesh GLSL ->
// SPIR-V -> MSL [[mesh]] -> MeshRenderPipelineDescriptor -> drawMeshThreadgroups
// -> readback. Guards the two fixes this needed: the LocalSizeId->LocalSize
// workgroup-size resolution (else gl_WorkGroupSize is 0 and the emit loops spin)
// and NOT setting maxTotalThreadsPerObjectThreadgroup on a mesh-only pipeline
// (else Metal waits on a nonexistent object stage). Both manifested as GPU hangs.
TEST(MetalSpirvPipeline, MeshShaderTriangle)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    if (!device->GetCapabilities().supportsMeshShaders)
    {
        GTEST_SKIP() << "mesh shaders unsupported on this GPU";
    }

    static const char* kMesh = R"(
#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles) out;
layout(max_vertices = 3, max_primitives = 1) out;
void main()
{
    SetMeshOutputsEXT(3, 1);
    gl_MeshVerticesEXT[0].gl_Position = vec4(-0.6, -0.6, 0.0, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4( 0.6, -0.6, 0.0, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4( 0.0,  0.6, 0.0, 1.0);
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    std::vector<uint8_t> meshSpv = CompileGlsl(kMesh, shaderc_mesh_shader, "tri.mesh");
    std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "tri.frag");
    ASSERT_FALSE(meshSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    constexpr uint32_t kRtDim = 64;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Mesh;
    pipelineDesc.meshShader = meshSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalMeshTriangle";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f; // red clear — the green triangle must overdraw it
    pass.clearColorValue[0][1] = 0.0f;
    pass.clearColorValue[0][2] = 0.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->DrawMeshTasks(1);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    // Center pixel (NDC 0,0) lies inside the triangle. BGRA: green fill.
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[0], 0u) << "B";
    EXPECT_NEAR(center[1], 255, 2) << "G (mesh-shader triangle fill)";
    EXPECT_EQ(center[2], 0u) << "R (proves the triangle overdrew the red clear)";
    EXPECT_EQ(center[3], 255u) << "A";

    device->DestroyBuffer(readback);
    device->DestroyTexture(rt);
    device->Shutdown();
}

// Mirrors the engine's GE_INSTANCED path: the vertex shader fetches its data
// through a GL_EXT_buffer_reference address delivered in push constants.
TEST(MetalSpirvPipeline, VertexShaderReadsBufferDeviceAddress)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kVert = R"(
#version 450
#extension GL_EXT_buffer_reference : require
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PositionsRef {
    vec4 positions[];
};
layout(push_constant) uniform Push { PositionsRef positionsRef; } uPush;
void main()
{
    gl_Position = uPush.positionsRef.positions[gl_VertexIndex];
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "bda.vert");
    std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "bda.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    // Fullscreen-ish triangle stored in a BDA-addressable buffer.
    const float positions[3][4] = {{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}};
    BufferDesc posDesc{};
    posDesc.size = sizeof(positions);
    posDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::ShaderDeviceAddress);
    BufferHandle posBuffer = device->CreateBuffer(posDesc);
    ASSERT_TRUE(posBuffer.IsValid());
    device->UpdateBuffer(posBuffer, 0, sizeof(positions), positions);
    const uint64_t address = device->GetBufferDeviceAddress(posBuffer);
    ASSERT_NE(address, 0u);

    constexpr uint32_t kRtDim = 16;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.pushConstantSize = 8;
    pipelineDesc.pushConstantStagesMask = kShaderStageVertex;
    pipelineDesc.debugName = "MetalBdaTriangle";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f;
    pass.clearColorValue[0][1] = 0.0f;
    pass.clearColorValue[0][2] = 0.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    cl->SetConstants(0, sizeof(address), &address);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->Draw(3);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[1], 255u) << "BDA-sourced triangle did not rasterize (G channel)";
    EXPECT_EQ(center[2], 0u) << "R channel";

    device->DestroyBuffer(readback);
    device->DestroyBuffer(posBuffer);
    device->DestroyTexture(rt);
    device->Shutdown();
}

// Directional shadows enable depth clamping so casters in front of the
// arbitrary shadow-camera near plane are pancaked onto that plane instead of
// disappearing. Metal owns this state on the render encoder rather than the
// pipeline object, so exercise the full PipelineDesc -> encoder path.
TEST(MetalSpirvPipeline, DepthClampRasterizesGeometryBeyondNearPlane)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kVert = R"(
#version 450
void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    // Metal's clip-space depth range is [0, w]. The whole triangle is in
    // front of the near plane: Clip rejects it, Clamp rasterizes it.
    gl_Position = vec4(positions[gl_VertexIndex], -0.5, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    const std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "depth-clamp.vert");
    const std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "depth-clamp.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    constexpr uint32_t kRtDim = 16;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    const TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.rasterizationState.depthClampEnable = true;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalDepthClampTriangle";
    const PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f;
    pass.clearColorValue[0][3] = 1.0f;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->Draw(3);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[1], 255u) << "near-plane-clamped triangle did not rasterize";
    EXPECT_EQ(center[2], 0u) << "red clear remained; Metal clipped the triangle";

    device->DestroyBuffer(readback);
    device->DestroyTexture(rt);
    device->Shutdown();
}

// The shared skinned depth pipeline of an eight-influence mesh declares the
// Joints1 / Weights1 bindings (6 and 7) while its vertex shader reads neither.
// Vulkan accepts a binding that no attribute reads; Metal's vertex descriptor
// aborts the process when it carries a layout for a buffer no attribute names.
TEST(MetalSpirvPipeline, VertexBindingThatNoAttributeReadsStillDraws)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kVert = R"(
#version 450
layout(location = 0) in vec2 inPosition;
void main() { gl_Position = vec4(inPosition, 0.5, 1.0); }
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    const std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "unread-binding.vert");
    const std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "unread-binding.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    constexpr uint32_t kRtDim = 16;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    const TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());

    // A triangle that covers the whole target, read from binding 0. Bindings 6 and 7 carry the
    // strides of the second joint and weight streams and are bound at draw time, as the depth
    // pass binds them, but no attribute reads them.
    const float positions[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    BufferDesc vbDesc{};
    vbDesc.size = sizeof(positions);
    vbDesc.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::TransferDst);
    const BufferHandle positionBuffer = device->CreateBuffer(vbDesc);
    ASSERT_TRUE(positionBuffer.IsValid());
    device->UpdateBuffer(positionBuffer, 0, sizeof(positions), positions);
    BufferDesc unreadDesc{};
    unreadDesc.size = 3 * 16;
    unreadDesc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    const BufferHandle unreadBuffer = device->CreateBuffer(unreadDesc);
    ASSERT_TRUE(unreadBuffer.IsValid());

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.vertexBindings = {{0, 2 * sizeof(float), 0}, {6, 8, 0}, {7, 16, 0}};
    pipelineDesc.vertexAttributes = {{0, 0, Format::R32G32_FLOAT, 0}};
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalUnreadVertexBinding";
    const PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f;
    pass.clearColorValue[0][3] = 1.0f;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->SetVertexBuffer(positionBuffer, 0);
    cl->SetVertexBuffer(unreadBuffer, 6);
    cl->SetVertexBuffer(unreadBuffer, 7);
    cl->Draw(3);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[1], 255u) << "the triangle read from binding 0 did not rasterize";
    EXPECT_EQ(center[2], 0u) << "red clear remained";

    device->DestroyBuffer(readback);
    device->DestroyBuffer(unreadBuffer);
    device->DestroyBuffer(positionBuffer);
    device->DestroyTexture(rt);
    device->Shutdown();
}

// Mirrors the engine's GPU-driven chain: a compute shader writes an indexed
// indirect draw record and its count; the graphics side draws it through
// DrawIndexedIndirectCount in the same command list.
TEST(MetalSpirvPipeline, ComputeWrittenIndirectDraws)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kComp = R"(
#version 450
layout(local_size_x = 1) in;
struct DrawCmd { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
layout(set = 0, binding = 0, std430) buffer Cmds { DrawCmd cmds[]; } uCmds;
layout(set = 0, binding = 1, std430) buffer Count { uint value; } uCount;
void main()
{
    uCmds.cmds[0].indexCount = 3u;
    uCmds.cmds[0].instanceCount = 1u;
    uCmds.cmds[0].firstIndex = 0u;
    uCmds.cmds[0].vertexOffset = 0;
    uCmds.cmds[0].firstInstance = 0u;
    uCount.value = 1u;
}
)";
    static const char* kVert = R"(
#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    std::vector<uint8_t> compSpv = CompileGlsl(kComp, shaderc_compute_shader, "writecmds.comp");
    std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "tri.vert");
    std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "tri.frag");
    ASSERT_FALSE(compSpv.empty());
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    constexpr uint32_t kMaxDraws = 8;
    constexpr uint32_t kStride = 20; // VkDrawIndexedIndirectCommand layout
    BufferDesc cmdDesc{};
    cmdDesc.size = kMaxDraws * kStride;
    cmdDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst);
    BufferHandle cmdBuffer = device->CreateBuffer(cmdDesc);
    BufferDesc countDesc{};
    countDesc.size = 4;
    countDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect);
    BufferHandle countBuffer = device->CreateBuffer(countDesc);
    ASSERT_TRUE(cmdBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    const uint16_t indices[3] = {0, 1, 2};
    BufferDesc indexDesc{};
    indexDesc.size = sizeof(indices);
    indexDesc.usage = static_cast<uint32_t>(BufferUsage::Index);
    BufferHandle indexBuffer = device->CreateBuffer(indexDesc);
    ASSERT_TRUE(indexBuffer.IsValid());
    device->UpdateBuffer(indexBuffer, 0, sizeof(indices), indices);

    DescriptorSetLayoutDesc layout{};
    for (uint32_t b = 0; b < 2; ++b)
    {
        DescriptorBinding storage{};
        storage.binding = b;
        storage.type = DescriptorType::StorageBuffer;
        storage.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(storage);
    }

    PipelineDesc computeDesc{};
    computeDesc.type = PipelineType::Compute;
    computeDesc.computeShader = compSpv;
    computeDesc.descriptorSetLayouts = {layout};
    computeDesc.debugName = "WriteIndirectCmds";
    PipelineHandle computePipeline = device->CreatePipeline(computeDesc);
    ASSERT_TRUE(computePipeline.IsValid());

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateStorageBufferBinding(set, 0, cmdBuffer, 0, cmdDesc.size);
    device->UpdateStorageBufferBinding(set, 1, countBuffer, 0, countDesc.size);

    constexpr uint32_t kRtDim = 16;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);

    PipelineDesc gfxDesc{};
    gfxDesc.type = PipelineType::Graphics;
    gfxDesc.vertexShader = vertSpv;
    gfxDesc.pixelShader = fragSpv;
    gfxDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    gfxDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    gfxDesc.depthStencilState.depthTestEnable = false;
    gfxDesc.depthStencilState.depthWriteEnable = false;
    gfxDesc.debugName = "IndirectTriangle";
    PipelineHandle gfxPipeline = device->CreatePipeline(gfxDesc);
    ASSERT_TRUE(gfxPipeline.IsValid());

    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(computePipeline);
    cl->BindDescriptorSet(0, set, computePipeline);
    cl->Dispatch(1, 1, 1);

    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f;
    pass.clearColorValue[0][1] = 0.0f;
    pass.clearColorValue[0][2] = 0.0f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(gfxPipeline);
    cl->SetIndexBuffer(indexBuffer, IndexType::Uint16);
    cl->SetViewport(0.0f, 0.0f, static_cast<float>(kRtDim), static_cast<float>(kRtDim));
    cl->SetScissor(0, 0, kRtDim, kRtDim);
    cl->DrawIndexedIndirectCount(cmdBuffer, countBuffer, kMaxDraws, kStride);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    const uint8_t* center = mapped + ((kRtDim / 2) * kRtDim + kRtDim / 2) * 4;
    EXPECT_EQ(center[1], 255u) << "compute-written indirect draw did not rasterize (G)";
    EXPECT_EQ(center[2], 0u) << "R";

    device->DestroyBuffer(readback);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(cmdBuffer);
    device->DestroyDescriptorSet(set);
    device->DestroyTexture(rt);
    device->Shutdown();
}

namespace
{

// The indirect-count tests draw one 4 x 4 pixel quad per draw record into a
// 16 x 16 target. The vertex stage places the quad at cell
// gl_VertexIndex / 4 + 8 * gl_InstanceIndex, so the set of lit cells says
// exactly which records the GPU drew, with which base vertex, first index and
// instance count.
constexpr uint32_t kCellGridDim = 4;
constexpr uint32_t kCellPixels = 4;
constexpr uint32_t kCellTargetDim = kCellGridDim * kCellPixels;
constexpr uint32_t kCellCount = kCellGridDim * kCellGridDim;
constexpr uint32_t kQuadIndexCount = 6;
constexpr uint32_t kQuadVertexCount = 4;

struct DrawRecord
{
    uint32_t IndexCount = 0;
    uint32_t InstanceCount = 0;
    uint32_t FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t FirstInstance = 0;
};
static_assert(sizeof(DrawRecord) == 20, "DrawRecord mirrors VkDrawIndexedIndirectCommand");

// `additive` sums 1.0 per covered draw, so a cell's value counts the draws
// that hit it.
PipelineHandle CreateCellQuadPipeline(IDevice& device, TextureFormat format, bool additive)
{
    static const char* kVert = R"(
#version 450
void main()
{
    uint cell = uint(gl_VertexIndex) / 4u + 8u * uint(gl_InstanceIndex);
    uint corner = uint(gl_VertexIndex) & 3u;
    vec2 cellOrigin = vec2(cell % 4u, cell / 4u);
    vec2 uv = (cellOrigin + vec2(corner & 1u, corner >> 1u)) / 4.0;
    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0, 1.0, 0.0, 1.0); }
)";
    const std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "cell-quad.vert");
    const std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "cell-quad.frag");
    if (vertSpv.empty() || fragSpv.empty())
    {
        return INVALID_PIPELINE_HANDLE;
    }
    PipelineDesc desc{};
    desc.type = PipelineType::Graphics;
    desc.vertexShader = vertSpv;
    desc.pixelShader = fragSpv;
    desc.colorAttachmentFormats = {static_cast<uint32_t>(format)};
    desc.rasterizationState.cullMode = CullModeFlagBits::None;
    desc.depthStencilState.depthTestEnable = false;
    desc.depthStencilState.depthWriteEnable = false;
    if (additive)
    {
        ColorBlendAttachmentState blend{};
        blend.blendEnable = true;
        blend.dstColorBlendFactor = BlendFactor::One;
        blend.dstAlphaBlendFactor = BlendFactor::One;
        desc.colorBlendState.attachments = {blend};
    }
    desc.debugName = "IndirectCountCellQuads";
    return device.CreatePipeline(desc);
}

TextureHandle CreateCellTarget(IDevice& device, TextureFormat format)
{
    TextureDesc desc{};
    desc.width = kCellTargetDim;
    desc.height = kCellTargetDim;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    return device.CreateTexture(desc);
}

BufferHandle CreateCellReadback(IDevice& device, uint32_t bytesPerPixel)
{
    BufferDesc desc{};
    desc.size = static_cast<size_t>(kCellTargetDim) * kCellTargetDim * bytesPerPixel;
    desc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    return device.CreateBuffer(desc);
}

BufferHandle CreateBufferWithData(IDevice& device, uint32_t usage, const void* data, size_t size)
{
    BufferDesc desc{};
    desc.size = size;
    desc.usage = usage;
    const BufferHandle buffer = device.CreateBuffer(desc);
    if (buffer.IsValid())
    {
        device.UpdateBuffer(buffer, 0, size, data);
    }
    return buffer;
}

void BeginCellPass(CommandList& cl, TextureHandle target, PipelineHandle pipeline, BufferHandle indexBuffer,
                   IndexType indexType)
{
    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl.BeginRenderPass(pass);
    cl.SetPipeline(pipeline);
    cl.SetIndexBuffer(indexBuffer, indexType);
    cl.SetViewport(0.0f, 0.0f, static_cast<float>(kCellTargetDim), static_cast<float>(kCellTargetDim));
    cl.SetScissor(0, 0, kCellTargetDim, kCellTargetDim);
}

size_t CellCenterPixel(uint32_t cell)
{
    const uint32_t x = (cell % kCellGridDim) * kCellPixels + kCellPixels / 2;
    const uint32_t y = (cell / kCellGridDim) * kCellPixels + kCellPixels / 2;
    return static_cast<size_t>(y) * kCellTargetDim + x;
}

// Lit cells of a BGRA8 readback of a cell target, in cell order.
std::vector<uint32_t> ReadLitCells(IDevice& device, BufferHandle readback)
{
    std::vector<uint32_t> lit;
    const uint8_t* pixels = static_cast<const uint8_t*>(device.MapBuffer(readback));
    if (pixels == nullptr)
    {
        ADD_FAILURE() << "readback did not map";
        return lit;
    }
    for (uint32_t cell = 0; cell < kCellCount; ++cell)
    {
        if (pixels[CellCenterPixel(cell) * 4 + 1] == 255u)
        {
            lit.push_back(cell);
        }
    }
    device.UnmapBuffer(readback);
    return lit;
}

// Draws per cell from an R16_FLOAT readback of an additive cell target.
std::vector<uint32_t> ReadCellDrawCounts(IDevice& device, BufferHandle readback)
{
    std::vector<uint32_t> counts;
    const uint16_t* pixels = static_cast<const uint16_t*>(device.MapBuffer(readback));
    if (pixels == nullptr)
    {
        ADD_FAILURE() << "readback did not map";
        return counts;
    }
    for (uint32_t cell = 0; cell < kCellCount; ++cell)
    {
        counts.push_back(static_cast<uint32_t>(GameEngine::Mathematics::HalfToFloat(pixels[CellCenterPixel(cell)])));
    }
    device.UnmapBuffer(readback);
    return counts;
}

std::vector<uint32_t> CellRange(uint32_t first, uint32_t count)
{
    std::vector<uint32_t> cells(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        cells[i] = first + i;
    }
    return cells;
}

} // namespace

// The GPU-written count decides how many records a DrawIndexedIndirectCount
// draws: every record in the buffer is a valid, visible draw, so drawing more
// than min(count, maxDrawCount) of them lights extra cells. Counts are written
// by a compute pass in the same command list, the way the culling pass writes
// them, and each count is drawn in its own render pass of that command list.
TEST(MetalCommandList, DrawIndexedIndirectCountDrawsExactlyTheCountedRecords)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kMaxDrawCount = 8;
    constexpr uint32_t kStride = sizeof(DrawRecord);
    // One count per pass: none, one, part, all, and more than maxDrawCount.
    constexpr uint32_t kCounts[] = {0u, 1u, 5u, 8u, 12u};
    constexpr uint32_t kPassCount = static_cast<uint32_t>(std::size(kCounts));

    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * i), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    BufferDesc countDesc{};
    countDesc.size = sizeof(kCounts);
    countDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect);
    const BufferHandle countBuffer = device->CreateBuffer(countDesc);
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    static const char* kWriteCounts = R"(
#version 450
layout(local_size_x = 5) in;
layout(set = 0, binding = 0, std430) buffer Counts { uint values[]; } uCounts;
const uint kCounts[5] = uint[5](0u, 1u, 5u, 8u, 12u);
void main() { uCounts.values[gl_LocalInvocationIndex] = kCounts[gl_LocalInvocationIndex]; }
)";
    const std::vector<uint8_t> compSpv = CompileGlsl(kWriteCounts, shaderc_compute_shader, "write-counts.comp");
    ASSERT_FALSE(compSpv.empty());
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);
    PipelineDesc computeDesc{};
    computeDesc.type = PipelineType::Compute;
    computeDesc.computeShader = compSpv;
    computeDesc.descriptorSetLayouts = {layout};
    computeDesc.debugName = "WriteIndirectCounts";
    const PipelineHandle computePipeline = device->CreatePipeline(computeDesc);
    ASSERT_TRUE(computePipeline.IsValid());
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateStorageBufferBinding(set, 0, countBuffer, 0, countDesc.size);

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, /*additive=*/false);
    ASSERT_TRUE(pipeline.IsValid());
    std::vector<TextureHandle> targets;
    std::vector<BufferHandle> readbacks;
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        targets.push_back(CreateCellTarget(*device, TextureFormat::BGRA8_UNORM));
        readbacks.push_back(CreateCellReadback(*device, 4));
        ASSERT_TRUE(targets.back().IsValid());
        ASSERT_TRUE(readbacks.back().IsValid());
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(computePipeline);
    cl->BindDescriptorSet(0, set, computePipeline);
    cl->Dispatch(1, 1, 1);
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        BeginCellPass(*cl, targets[pass], pipeline, indexBuffer, IndexType::Uint16);
        cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, kStride, 0,
                                     static_cast<size_t>(pass) * sizeof(uint32_t));
        cl->EndRenderPass();
    }
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        cl->CopyTextureToBuffer(targets[pass], readbacks[pass], kCellTargetDim, kCellTargetDim);
    }
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        EXPECT_EQ(ReadLitCells(*device, readbacks[pass]), CellRange(0u, std::min(kCounts[pass], kMaxDrawCount)))
            << "count " << kCounts[pass] << " with maxDrawCount " << kMaxDrawCount;
    }

    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        device->DestroyBuffer(readbacks[pass]);
        device->DestroyTexture(targets[pass]);
    }
    device->DestroyDescriptorSet(set);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// Each counted record draws with its own index count, first index, vertex
// offset, instance count and first instance, read at the call's byte offset
// and stride, from a 32-bit index buffer; two calls in one pass read their own
// counts.
TEST(MetalCommandList, DrawIndexedIndirectCountDrawsEachRecordWithItsOwnArguments)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    // Padded records at a non-zero offset. Record j draws vertices
    // 4j .. 4j + 3 (cell j) through the second quad of the index buffer: an even
    // record as instances 0 and 1 (cells j and j + 8), an odd one as instance 1
    // alone (cell j + 8). The first quad's indices point at cell 15, which only
    // a draw that ignored firstIndex would light.
    constexpr uint32_t kStride = 24;
    constexpr size_t kFirstRecordOffset = 2 * kStride;
    constexpr uint32_t kRecordCount = 8;
    constexpr uint32_t kMaxDrawCount = 4;
    std::vector<uint8_t> recordBytes(kFirstRecordOffset + kRecordCount * kStride, 0xA5);
    for (uint32_t j = 0; j < kRecordCount; ++j)
    {
        const bool odd = (j % 2u) != 0u;
        const DrawRecord record{kQuadIndexCount, odd ? 1u : 2u, kQuadIndexCount,
                                static_cast<int32_t>(kQuadVertexCount * j), odd ? 1u : 0u};
        std::memcpy(recordBytes.data() + kFirstRecordOffset + j * kStride, &record, sizeof(record));
    }
    const uint32_t quadIndices[2 * kQuadIndexCount] = {60, 61, 62, 61, 63, 62, 0, 1, 2, 1, 3, 2};
    const uint32_t counts[2] = {3u, 1u};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), recordBytes.data(),
        recordBytes.size());
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), counts, sizeof(counts));
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, /*additive=*/false);
    ASSERT_TRUE(pipeline.IsValid());
    const TextureHandle target = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const BufferHandle readback = CreateCellReadback(*device, 4);
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    BeginCellPass(*cl, target, pipeline, indexBuffer, IndexType::Uint32);
    // Records 0..3 with count 3 (cells 0, 8; 9; 2, 10), then records 4..7 with
    // count 1 (cells 4, 12).
    cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, kStride, kFirstRecordOffset, 0);
    cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, kStride,
                                 kFirstRecordOffset + kMaxDrawCount * kStride, sizeof(uint32_t));
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(target, readback, kCellTargetDim, kCellTargetDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    EXPECT_EQ(ReadLitCells(*device, readback), (std::vector<uint32_t>{0u, 2u, 4u, 8u, 9u, 10u, 12u}));

    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// A call whose maxDrawCount spans several indirect command pages still draws
// exactly min(count, maxDrawCount) records, each once: record i adds 1.0 to
// cell i % 16, so every cell must hold exactly its share of the count.
TEST(MetalCommandList, DrawIndexedIndirectCountDrawsEachCountedRecordOnceAcrossLargeCalls)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kMaxDrawCount = 5000;
    constexpr uint32_t kCounts[] = {2041u, 4500u, 7000u};
    constexpr uint32_t kPassCount = static_cast<uint32_t>(std::size(kCounts));
    constexpr uint32_t kHalfBytes = 2;

    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * (i % kCellCount)), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), kCounts, sizeof(kCounts));
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::R16_FLOAT, /*additive=*/true);
    ASSERT_TRUE(pipeline.IsValid());
    std::vector<TextureHandle> targets;
    std::vector<BufferHandle> readbacks;
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        targets.push_back(CreateCellTarget(*device, TextureFormat::R16_FLOAT));
        readbacks.push_back(CreateCellReadback(*device, kHalfBytes));
        ASSERT_TRUE(targets.back().IsValid());
        ASSERT_TRUE(readbacks.back().IsValid());
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        BeginCellPass(*cl, targets[pass], pipeline, indexBuffer, IndexType::Uint16);
        cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, sizeof(DrawRecord), 0,
                                     static_cast<size_t>(pass) * sizeof(uint32_t));
        cl->EndRenderPass();
    }
    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        cl->CopyTextureToBuffer(targets[pass], readbacks[pass], kCellTargetDim, kCellTargetDim);
    }
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        const uint32_t drawn = std::min(kCounts[pass], kMaxDrawCount);
        std::vector<uint32_t> expected(kCellCount);
        for (uint32_t cell = 0; cell < kCellCount; ++cell)
        {
            expected[cell] = drawn / kCellCount + (cell < drawn % kCellCount ? 1u : 0u);
        }
        EXPECT_EQ(ReadCellDrawCounts(*device, readbacks[pass]), expected)
            << "count " << kCounts[pass] << " with maxDrawCount " << kMaxDrawCount;
    }

    for (uint32_t pass = 0; pass < kPassCount; ++pass)
    {
        device->DestroyBuffer(readbacks[pass]);
        device->DestroyTexture(targets[pass]);
    }
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// A count written through a buffer device address, which the hazard tracker
// does not see, is still the one the draw uses: the compute pass spins before
// it writes, so a draw that is not ordered behind it reads the stale count.
TEST(MetalCommandList, DrawIndexedIndirectCountReadsACountWrittenThroughADeviceAddress)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kMaxDrawCount = 8;
    constexpr uint32_t kCount = 5;
    constexpr uint32_t kSpinIterations = 2'000'000;

    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * i), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const uint32_t staleCount = 0;
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device,
        static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::ShaderDeviceAddress),
        &staleCount, sizeof(staleCount));
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    // The count store depends on the spin (through a zero mask from the push
    // constants), so neither the loop nor the store's place after it can be
    // optimised away.
    static const char* kWriteCount = R"(
#version 450
#extension GL_EXT_buffer_reference : require
layout(local_size_x = 1) in;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer CountRef { uint value; };
layout(push_constant) uniform Push { CountRef countRef; uint spinIterations; uint count; uint zero; } uPush;
void main()
{
    uint state = 1u;
    for (uint i = 0u; i < uPush.spinIterations; ++i)
    {
        state = state * 1664525u + 1013904223u;
    }
    uPush.countRef.value = uPush.count + (state & uPush.zero);
}
)";
    struct CountWriterConstants
    {
        uint64_t CountAddress = 0;
        uint32_t SpinIterations = 0;
        uint32_t Count = 0;
        uint32_t Zero = 0;
    };
    const std::vector<uint8_t> compSpv = CompileGlsl(kWriteCount, shaderc_compute_shader, "write-count-bda.comp");
    ASSERT_FALSE(compSpv.empty());
    PipelineDesc computeDesc{};
    computeDesc.type = PipelineType::Compute;
    computeDesc.computeShader = compSpv;
    computeDesc.pushConstantSize = sizeof(CountWriterConstants);
    computeDesc.pushConstantStagesMask = kShaderStageCompute;
    computeDesc.debugName = "WriteIndirectCountThroughDeviceAddress";
    const PipelineHandle computePipeline = device->CreatePipeline(computeDesc);
    ASSERT_TRUE(computePipeline.IsValid());
    CountWriterConstants constants{};
    constants.CountAddress = device->GetBufferDeviceAddress(countBuffer);
    constants.SpinIterations = kSpinIterations;
    constants.Count = kCount;
    ASSERT_NE(constants.CountAddress, 0u);

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, /*additive=*/false);
    ASSERT_TRUE(pipeline.IsValid());
    const TextureHandle target = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const BufferHandle readback = CreateCellReadback(*device, 4);
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(computePipeline);
    cl->SetConstants(0, sizeof(constants), &constants);
    cl->Dispatch(1, 1, 1);
    BeginCellPass(*cl, target, pipeline, indexBuffer, IndexType::Uint16);
    cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, sizeof(DrawRecord), 0, 0);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(target, readback, kCellTargetDim, kCellTargetDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    EXPECT_EQ(ReadLitCells(*device, readback), CellRange(0u, kCount));

    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// A count that a blit rewrites between render passes is the count the next
// draw uses. Each blit follows a pass without an indirect-count draw, which
// leaves the translation encoded before that pass open: a draw served by it
// would read the count from before the blit.
TEST(MetalCommandList, DrawIndexedIndirectCountReadsACountABlitRewroteBetweenPasses)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kMaxDrawCount = 8;
    constexpr uint32_t kInitialCount = 2;
    constexpr uint32_t kCopiedCount = 6;
    constexpr uint32_t kFilledCount = 0;
    constexpr uint32_t kTexelCount = 3;
    constexpr uint32_t kBlitCount = 3;

    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * i), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst),
        &kInitialCount, sizeof(kInitialCount));
    const BufferHandle copySource = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::TransferSrc), &kCopiedCount, sizeof(kCopiedCount));
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());
    ASSERT_TRUE(copySource.IsValid());

    // One RGBA8 texel whose bytes read back as the 32-bit count kTexelCount.
    TextureDesc texelDesc{};
    texelDesc.width = 1;
    texelDesc.height = 1;
    texelDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    texelDesc.usage = static_cast<uint32_t>(TextureUsage::TransferSrc | TextureUsage::TransferDst);
    const TextureHandle texel = device->CreateTexture(texelDesc);
    ASSERT_TRUE(texel.IsValid());
    const uint8_t texelBytes[4] = {static_cast<uint8_t>(kTexelCount), 0u, 0u, 0u};
    IDevice::TextureUploadRequest upload{};
    upload.Texture = texel;
    upload.Pixels = texelBytes;
    upload.Width = 1;
    upload.Height = 1;
    upload.RowPitchBytes = sizeof(texelBytes);
    const IDevice::GpuSyncToken uploaded = device->SubmitTextureUploads(&upload, 1);
    ASSERT_TRUE(uploaded.IsValid());
    ASSERT_TRUE(device->WaitGpuSyncToken(uploaded, 5'000'000'000ull));

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, /*additive=*/false);
    ASSERT_TRUE(pipeline.IsValid());
    const TextureHandle idleTarget = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    ASSERT_TRUE(idleTarget.IsValid());
    std::vector<TextureHandle> targets;
    std::vector<BufferHandle> readbacks;
    for (uint32_t blit = 0; blit < kBlitCount; ++blit)
    {
        targets.push_back(CreateCellTarget(*device, TextureFormat::BGRA8_UNORM));
        readbacks.push_back(CreateCellReadback(*device, 4));
        ASSERT_TRUE(targets.back().IsValid());
        ASSERT_TRUE(readbacks.back().IsValid());
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    for (uint32_t blit = 0; blit < kBlitCount; ++blit)
    {
        BeginCellPass(*cl, idleTarget, pipeline, indexBuffer, IndexType::Uint16);
        cl->EndRenderPass();
        switch (blit)
        {
        case 0:
            cl->CopyBuffer(copySource, countBuffer, sizeof(kCopiedCount));
            break;
        case 1:
            cl->FillBuffer(countBuffer, 0, sizeof(kFilledCount), kFilledCount);
            break;
        default:
            cl->CopyTextureToBuffer(texel, countBuffer, 1, 1);
            break;
        }
        BeginCellPass(*cl, targets[blit], pipeline, indexBuffer, IndexType::Uint16);
        cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, sizeof(DrawRecord), 0, 0);
        cl->EndRenderPass();
    }
    for (uint32_t blit = 0; blit < kBlitCount; ++blit)
    {
        cl->CopyTextureToBuffer(targets[blit], readbacks[blit], kCellTargetDim, kCellTargetDim);
    }
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    EXPECT_EQ(ReadLitCells(*device, readbacks[0]), CellRange(0u, kCopiedCount)) << "after CopyBuffer";
    EXPECT_EQ(ReadLitCells(*device, readbacks[1]), CellRange(0u, kFilledCount)) << "after FillBuffer";
    EXPECT_EQ(ReadLitCells(*device, readbacks[2]), CellRange(0u, kTexelCount)) << "after CopyTextureToBuffer";

    for (uint32_t blit = 0; blit < kBlitCount; ++blit)
    {
        device->DestroyBuffer(readbacks[blit]);
        device->DestroyTexture(targets[blit]);
    }
    device->DestroyTexture(idleTarget);
    device->DestroyTexture(texel);
    device->DestroyBuffer(copySource);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// A count that a compute dispatch rewrites between two passes is the count the
// later pass draws with. The pass before the dispatch has no indirect-count
// draw, so the translation encoded before it is still open; compute recorded
// after it must seal it, or the later pass would draw from a translation that
// ran before the dispatch.
TEST(MetalCommandList, DrawIndexedIndirectCountReadsACountAComputePassRewroteBetweenPasses)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    constexpr uint32_t kMaxDrawCount = 8;
    constexpr uint32_t kInitialCount = 2;
    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * i), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), &kInitialCount,
        sizeof(kInitialCount));
    ASSERT_TRUE(recordBuffer.IsValid() && indexBuffer.IsValid() && countBuffer.IsValid());

    static const char* kWriteCount = R"(
#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer Counts { uint values[]; } uCounts;
void main() { uCounts.values[0] = 7u; }
)";
    const std::vector<uint8_t> compSpv = CompileGlsl(kWriteCount, shaderc_compute_shader, "write-count.comp");
    ASSERT_FALSE(compSpv.empty());
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);
    PipelineDesc computeDesc{};
    computeDesc.type = PipelineType::Compute;
    computeDesc.computeShader = compSpv;
    computeDesc.descriptorSetLayouts = {layout};
    const PipelineHandle computePipeline = device->CreatePipeline(computeDesc);
    ASSERT_TRUE(computePipeline.IsValid());
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    device->UpdateStorageBufferBinding(set, 0, countBuffer, 0, sizeof(uint32_t));

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, false);
    const TextureHandle idleTarget = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const TextureHandle target = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const BufferHandle readback = CreateCellReadback(*device, 4);
    ASSERT_TRUE(pipeline.IsValid() && idleTarget.IsValid() && target.IsValid() && readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    BeginCellPass(*cl, idleTarget, pipeline, indexBuffer, IndexType::Uint16);
    cl->EndRenderPass();
    cl->SetPipeline(computePipeline);
    cl->BindDescriptorSet(0, set, computePipeline);
    cl->Dispatch(1, 1, 1);
    BeginCellPass(*cl, target, pipeline, indexBuffer, IndexType::Uint16);
    cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, sizeof(DrawRecord), 0, 0);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(target, readback, kCellTargetDim, kCellTargetDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    EXPECT_EQ(ReadLitCells(*device, readback), CellRange(0u, 7u)) << "after a compute rewrite";

    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->DestroyTexture(idleTarget);
    device->DestroyDescriptorSet(set);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

// A count that a render pass writes through a writable binding is the count a
// later pass draws with. The writing pass has no indirect-count draw of its
// own, so the translation encoded before it is still open: a draw served by
// it would read the count from before the write.
TEST(MetalCommandList, DrawIndexedIndirectCountReadsACountAnEarlierRenderPassWrote)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    constexpr uint32_t kMaxDrawCount = 8;
    constexpr uint32_t kInitialCount = 1;
    constexpr uint32_t kWrittenCount = 5;

    std::vector<DrawRecord> records(kMaxDrawCount);
    for (uint32_t i = 0; i < kMaxDrawCount; ++i)
    {
        records[i] = DrawRecord{kQuadIndexCount, 1u, 0u, static_cast<int32_t>(kQuadVertexCount * i), 0u};
    }
    const uint16_t quadIndices[kQuadIndexCount] = {0, 1, 2, 1, 3, 2};
    const BufferHandle recordBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), records.data(),
        records.size() * sizeof(DrawRecord));
    const BufferHandle indexBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Index), quadIndices, sizeof(quadIndices));
    const BufferHandle countBuffer = CreateBufferWithData(
        *device, static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::Indirect), &kInitialCount,
        sizeof(kInitialCount));
    ASSERT_TRUE(recordBuffer.IsValid());
    ASSERT_TRUE(indexBuffer.IsValid());
    ASSERT_TRUE(countBuffer.IsValid());

    // A full-target triangle whose fragments store kWrittenCount into the
    // count through a storage-buffer binding.
    static const char* kTriangleVert = R"(
#version 450
void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.5, 1.0);
}
)";
    static const char* kWriteCountFrag = R"(
#version 450
layout(set = 0, binding = 0, std430) buffer Counts { uint values[]; } uCounts;
layout(location = 0) out vec4 outColor;
void main()
{
    uCounts.values[0] = 5u;
    outColor = vec4(0.0);
}
)";
    const std::vector<uint8_t> vertSpv = CompileGlsl(kTriangleVert, shaderc_vertex_shader, "write-count.vert");
    const std::vector<uint8_t> fragSpv = CompileGlsl(kWriteCountFrag, shaderc_fragment_shader, "write-count.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageFragment;
    layout.bindings.push_back(storage);
    PipelineDesc writeDesc{};
    writeDesc.type = PipelineType::Graphics;
    writeDesc.vertexShader = vertSpv;
    writeDesc.pixelShader = fragSpv;
    writeDesc.descriptorSetLayouts = {layout};
    writeDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    writeDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    writeDesc.depthStencilState.depthTestEnable = false;
    writeDesc.depthStencilState.depthWriteEnable = false;
    writeDesc.debugName = "WriteIndirectCountFromFragments";
    const PipelineHandle writePipeline = device->CreatePipeline(writeDesc);
    ASSERT_TRUE(writePipeline.IsValid());
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    device->UpdateStorageBufferBinding(set, 0, countBuffer, 0, sizeof(kInitialCount));

    const PipelineHandle pipeline = CreateCellQuadPipeline(*device, TextureFormat::BGRA8_UNORM, /*additive=*/false);
    ASSERT_TRUE(pipeline.IsValid());
    const TextureHandle writeTarget = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const TextureHandle target = CreateCellTarget(*device, TextureFormat::BGRA8_UNORM);
    const BufferHandle readback = CreateCellReadback(*device, 4);
    ASSERT_TRUE(writeTarget.IsValid());
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(readback.IsValid());

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    BeginCellPass(*cl, writeTarget, writePipeline, indexBuffer, IndexType::Uint16);
    cl->BindDescriptorSet(0, set, writePipeline);
    cl->Draw(3, 1);
    cl->EndRenderPass();
    BeginCellPass(*cl, target, pipeline, indexBuffer, IndexType::Uint16);
    cl->DrawIndexedIndirectCount(recordBuffer, countBuffer, kMaxDrawCount, sizeof(DrawRecord), 0, 0);
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(target, readback, kCellTargetDim, kCellTargetDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    EXPECT_EQ(ReadLitCells(*device, readback), CellRange(0u, kWrittenCount));

    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->DestroyTexture(writeTarget);
    device->DestroyDescriptorSet(set);
    device->DestroyBuffer(countBuffer);
    device->DestroyBuffer(indexBuffer);
    device->DestroyBuffer(recordBuffer);
    device->Shutdown();
}

TEST(MetalSpirvPipeline, ComputeWritesStorageBuffer)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kComp = R"(
#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Out { uint values[]; } uOut;
layout(push_constant) uniform Push { uint base; } uPush;
void main()
{
    uOut.values[gl_GlobalInvocationID.x] = uPush.base + gl_GlobalInvocationID.x;
}
)";
    std::vector<uint8_t> compSpv = CompileGlsl(kComp, shaderc_compute_shader, "fill.comp");
    ASSERT_FALSE(compSpv.empty());

    constexpr uint32_t kCount = 64;
    BufferDesc bufDesc{};
    bufDesc.size = kCount * 4;
    bufDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    BufferHandle buffer = device->CreateBuffer(bufDesc);
    ASSERT_TRUE(buffer.IsValid());

    DescriptorSetLayoutDesc layout{};
    {
        DescriptorBinding storage{};
        storage.binding = 0;
        storage.type = DescriptorType::StorageBuffer;
        storage.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(storage);
    }

    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Compute;
    pipelineDesc.computeShader = compSpv;
    pipelineDesc.descriptorSetLayouts = {layout};
    pipelineDesc.pushConstantSize = 4;
    pipelineDesc.pushConstantStagesMask = kShaderStageCompute;
    pipelineDesc.debugName = "MetalSpirvComputeFill";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(set.IsValid());
    device->UpdateStorageBufferBinding(set, 0, buffer, 0, bufDesc.size);

    const uint32_t base = 1000;
    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(pipeline);
    cl->BindDescriptorSet(0, set, pipeline);
    cl->SetConstants(0, sizeof(base), &base);
    cl->Dispatch(kCount / 16, 1, 1);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    const uint32_t* mapped = static_cast<const uint32_t*>(device->MapBuffer(buffer));
    ASSERT_NE(mapped, nullptr);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(mapped[i], base + i) << "element " << i;
    }

    device->DestroyDescriptorSet(set);
    device->DestroyBuffer(buffer);
    device->Shutdown();
}

// The CBT kernel set's shape on Metal: 23 compute pipelines specialised from one
// program, asked for from four threads at once (the device's build workers and a
// synchronous caller). Every thread gets the same pipeline for a kernel, so each
// was created once and none was displaced; MTLDevice creation and the translator
// are safe to run concurrently.
TEST(MetalSpirvPipeline, ConcurrentComputeBuildsCreateEachPipelineOnce)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kComp = R"(
#version 450
layout(constant_id = 0) const uint KERNEL = 0;
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Out { uint values[]; } uOut;
void main()
{
    uOut.values[gl_GlobalInvocationID.x] = KERNEL;
}
)";
    std::vector<uint8_t> compSpv = CompileGlsl(kComp, shaderc_compute_shader, "kernels.comp");
    ASSERT_FALSE(compSpv.empty());
    const auto blob = std::make_shared<const std::vector<uint8_t>>(std::move(compSpv));

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);
    const DescriptorSetLayoutId layoutId = device->InternDescriptorSetLayout(layout);

    constexpr uint32_t kKernels = 23;
    constexpr uint32_t kThreads = 4;
    std::array<ComputePipelineId, kKernels> ids{};
    for (uint32_t i = 0; i < kKernels; ++i)
    {
        ComputePipelineDesc cd{};
        cd.DebugName = "ConcurrentKernel" + std::to_string(i);
        cd.ComputeShader = blob;
        SpecializationConstants spec;
        spec.AddConstant(0u, i, "KERNEL");
        cd.Specialization = std::move(spec);
        cd.DescriptorSetLayouts = {layoutId};
        ids[i] = device->InternComputePipeline(cd);
        ASSERT_TRUE(ids[i].IsValid());
    }

    std::array<std::array<PipelineHandle, kKernels>, kThreads> handles{};
    std::atomic<uint32_t> ready{0};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&, t]
            {
                ready.fetch_add(1);
                while (ready.load() < kThreads)
                {
                }
                // Each thread walks the kernels from a different start, so every
                // kernel is contended at some point.
                for (uint32_t n = 0; n < kKernels; ++n)
                {
                    const uint32_t i = (n + t * 6u) % kKernels;
                    handles[t][i] = device->GetOrCreateComputePipeline(ids[i]);
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    for (uint32_t i = 0; i < kKernels; ++i)
    {
        ASSERT_TRUE(handles[0][i].IsValid()) << "kernel " << i;
        for (uint32_t t = 1; t < kThreads; ++t)
        {
            EXPECT_EQ(handles[t][i], handles[0][i]) << "kernel " << i << " was created twice";
        }
        EXPECT_EQ(device->TryGetWarmComputePipeline(ids[i]), handles[0][i]);
        for (uint32_t j = 0; j < i; ++j)
        {
            EXPECT_NE(handles[0][i], handles[0][j]) << "kernels " << i << " and " << j << " share a pipeline";
        }
    }
    device->Shutdown();
}

namespace
{
// Set 0, binding 0: the one storage buffer the length pipeline writes to.
DescriptorSetLayoutDesc MakeLengthLayout()
{
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);
    return layout;
}

// One invocation writes the element count of the runtime array at set 0,
// binding 0 into that array's element 0. The count comes from the buffer size
// the set's table carries, so element 0 names the table the dispatch read.
PipelineHandle CreateLengthPipeline(IDevice& device, const DescriptorSetLayoutDesc& layout)
{
    static const char* kComp = R"(
#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer Out { uint values[]; } uOut;
void main()
{
    uOut.values[0] = uint(uOut.values.length());
}
)";
    std::vector<uint8_t> compSpv = CompileGlsl(kComp, shaderc_compute_shader, "length.comp");
    if (compSpv.empty())
    {
        return {};
    }
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Compute;
    pipelineDesc.computeShader = compSpv;
    pipelineDesc.descriptorSetLayouts = {layout};
    pipelineDesc.debugName = "MetalDescriptorSetLength";
    return device.CreatePipeline(pipelineDesc);
}

// A storage buffer of `length` uint32 elements, all zero.
BufferHandle CreateZeroedStorageBuffer(IDevice& device, uint32_t length)
{
    BufferDesc bufferDesc{};
    bufferDesc.size = length * sizeof(uint32_t);
    bufferDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    BufferHandle buffer = device.CreateBuffer(bufferDesc);
    if (buffer.IsValid())
    {
        const std::vector<uint32_t> zeros(length, 0u);
        device.UpdateBuffer(buffer, 0, bufferDesc.size, zeros.data());
    }
    return buffer;
}

// Binds two sets of one lifetime one after another in one compute encoder and
// expects each dispatch to reach its own buffer and the size its own table
// carries. The later-created set is bound first: a transient set's table then
// starts the encoder's first bind at a non-zero offset in the arena's page, and
// the second bind only moves the offset.
void ExpectSetsInOneComputeEncoderReadTheirOwnTables(bool transient)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    const DescriptorSetLayoutDesc layout = MakeLengthLayout();
    PipelineHandle pipeline = CreateLengthPipeline(*device, layout);
    ASSERT_TRUE(pipeline.IsValid());

    constexpr uint32_t kLengths[] = {8, 16};
    std::vector<BufferHandle> buffers;
    std::vector<DescriptorSetHandle> sets;
    ASSERT_TRUE(device->BeginFrame());
    for (uint32_t length : kLengths)
    {
        buffers.push_back(CreateZeroedStorageBuffer(*device, length));
        ASSERT_TRUE(buffers.back().IsValid());
        DescriptorSetDesc setDesc{};
        setDesc.layout = layout;
        setDesc.transient = transient;
        sets.push_back(device->CreateDescriptorSet(setDesc));
        ASSERT_TRUE(sets.back().IsValid());
        device->UpdateStorageBufferBinding(sets.back(), 0, buffers.back(), 0, length * sizeof(uint32_t));
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->SetPipeline(pipeline);
    constexpr size_t kBindOrder[] = {1, 0};
    for (size_t i : kBindOrder)
    {
        cl->BindDescriptorSet(0, sets[i], pipeline);
        cl->Dispatch(1, 1, 1);
    }
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->FinalizeFrame();
    device->WaitForIdle();

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        const uint32_t* mapped = static_cast<const uint32_t*>(device->MapBuffer(buffers[i]));
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(mapped[0], kLengths[i]) << (transient ? "transient" : "persistent") << " set, dispatch " << i;
    }

    if (!transient)
    {
        for (DescriptorSetHandle set : sets)
        {
            device->DestroyDescriptorSet(set);
        }
    }
    for (BufferHandle buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }
    device->Shutdown();
}
} // namespace

// The transient sets of one frame share their arena's pages, so binding one
// after another in the same encoder must still hand each dispatch its own
// table, and the buffer sizes each table carries must reach the shader.
TEST(MetalDescriptorSets, TransientSetsInOneComputeEncoderReadTheirOwnTables)
{
    ExpectSetsInOneComputeEncoderReadTheirOwnTables(true);
}

// A persistent set owns its table, and its buffer sizes sit in that table after
// the entries; the size-constants entry must point at them.
TEST(MetalDescriptorSets, PersistentSetsInOneComputeEncoderReadTheirOwnTables)
{
    ExpectSetsInOneComputeEncoderReadTheirOwnTables(false);
}

// The render-pass form of the test above: two draws in one pass, each with its
// own transient set, colour their own half of the target.
TEST(MetalDescriptorSets, TransientSetsInOneRenderPassReadTheirOwnTables)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    static const char* kVert = R"(
#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* kFrag = R"(
#version 450
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform Tint { vec4 color; } uTint;
void main()
{
    outColor = uTint.color;
}
)";
    std::vector<uint8_t> vertSpv = CompileGlsl(kVert, shaderc_vertex_shader, "tint.vert");
    std::vector<uint8_t> fragSpv = CompileGlsl(kFrag, shaderc_fragment_shader, "tint.frag");
    ASSERT_FALSE(vertSpv.empty());
    ASSERT_FALSE(fragSpv.empty());

    DescriptorSetLayoutDesc layout{};
    {
        DescriptorBinding tint{};
        tint.binding = 0;
        tint.type = DescriptorType::UniformBuffer;
        tint.shaderStages = kShaderStageFragment;
        layout.bindings.push_back(tint);
    }
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Graphics;
    pipelineDesc.vertexShader = vertSpv;
    pipelineDesc.pixelShader = fragSpv;
    pipelineDesc.descriptorSetLayouts = {layout};
    pipelineDesc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    pipelineDesc.debugName = "MetalTransientSetTint";
    PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    constexpr uint32_t kRtDim = 64;
    TextureDesc rtDesc{};
    rtDesc.width = kRtDim;
    rtDesc.height = kRtDim;
    rtDesc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
    rtDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    TextureHandle rt = device->CreateTexture(rtDesc);
    ASSERT_TRUE(rt.IsValid());
    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<size_t>(kRtDim) * kRtDim * 4;
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    BufferHandle readback = device->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    // Left half red, right half green.
    const float kColors[2][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
    std::vector<BufferHandle> tints;
    std::vector<DescriptorSetHandle> sets;
    ASSERT_TRUE(device->BeginFrame());
    for (const float* color : kColors)
    {
        BufferDesc tintDesc{};
        tintDesc.size = 4 * sizeof(float);
        tintDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        tints.push_back(device->CreateBuffer(tintDesc));
        ASSERT_TRUE(tints.back().IsValid());
        device->UpdateBuffer(tints.back(), 0, tintDesc.size, color);
        DescriptorSetDesc setDesc{};
        setDesc.layout = layout;
        setDesc.transient = true;
        sets.push_back(device->CreateDescriptorSet(setDesc));
        ASSERT_TRUE(sets.back().IsValid());
        device->UpdateBufferBinding(sets.back(), 0, tints.back(), 0, tintDesc.size);
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true; // the default clear is blue, which neither draw writes
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->SetPipeline(pipeline);
    constexpr uint32_t kHalf = kRtDim / 2;
    // Half 1 first. The first set's table starts the page, so the encoder's
    // first bind, which binds the page anew, is a table at a non-zero offset;
    // the second bind only moves the offset.
    constexpr uint32_t kDrawOrder[] = {1, 0};
    for (uint32_t i : kDrawOrder)
    {
        cl->SetViewport(static_cast<float>(i * kHalf), 0.0f, static_cast<float>(kHalf), static_cast<float>(kRtDim));
        cl->SetScissor(i * kHalf, 0, kHalf, kRtDim);
        cl->BindDescriptorSet(0, sets[i], pipeline);
        cl->Draw(3);
    }
    cl->EndRenderPass();
    cl->CopyTextureToBuffer(rt, readback, kRtDim, kRtDim);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->FinalizeFrame();
    device->WaitForIdle();

    const uint8_t* mapped = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(mapped, nullptr);
    for (uint32_t i = 0; i < sets.size(); ++i)
    {
        // BGRA at the centre of half i.
        const uint8_t* pixel = mapped + ((kRtDim / 2) * kRtDim + i * kHalf + kHalf / 2) * 4;
        EXPECT_EQ(pixel[0], 0u) << "B, half " << i;
        EXPECT_EQ(pixel[1], i == 1 ? 255u : 0u) << "G, half " << i;
        EXPECT_EQ(pixel[2], i == 0 ? 255u : 0u) << "R, half " << i;
    }

    device->DestroyBuffer(readback);
    for (BufferHandle tint : tints)
    {
        device->DestroyBuffer(tint);
    }
    device->DestroyTexture(rt);
    device->Shutdown();
}

// A transient set's table lives in its frame slot's arena, which is rewound
// only once that slot comes round again. A command buffer that binds the set
// and waits on the GPU while every other slot serves a frame of transient sets
// must still read its own table when it runs.
TEST(MetalDescriptorSets, TransientTableSurvivesLaterFramesWhileItsCommandBufferWaits)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    const DescriptorSetLayoutDesc layout = MakeLengthLayout();
    PipelineHandle pipeline = CreateLengthPipeline(*device, layout);
    ASSERT_TRUE(pipeline.IsValid());
    SemaphoreHandle gate = device->CreateTimelineSemaphore(0);
    ASSERT_TRUE(gate.IsValid());
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    setDesc.transient = true;
    std::vector<BufferHandle> buffers;

    // An empty frame first, so the held set is created in a slot other than 0.
    ASSERT_TRUE(device->BeginFrame());
    device->FinalizeFrame();

    ASSERT_TRUE(device->BeginFrame());
    constexpr uint32_t kHeldLength = 8;
    const BufferHandle held = CreateZeroedStorageBuffer(*device, kHeldLength);
    ASSERT_TRUE(held.IsValid());
    buffers.push_back(held);
    const DescriptorSetHandle heldSet = device->CreateDescriptorSet(setDesc);
    ASSERT_TRUE(heldSet.IsValid());
    device->UpdateStorageBufferBinding(heldSet, 0, held, 0, kHeldLength * sizeof(uint32_t));
    auto heldList = device->CreateCommandList(IDevice::QueueType::Compute);
    heldList->Begin();
    heldList->SetPipeline(pipeline);
    heldList->BindDescriptorSet(0, heldSet, pipeline);
    heldList->Dispatch(1, 1, 1);
    heldList->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Compute, {heldList.get()}, {{gate, 1}}, {}));
    device->FinalizeFrame();

    // Every other frame slot serves a frame of transient sets meanwhile.
    constexpr uint32_t kOtherLength = 16;
    constexpr uint32_t kSetsPerFrame = 64;
    for (uint32_t frame = 1; frame < device->GetFramesInFlight(); ++frame)
    {
        ASSERT_TRUE(device->BeginFrame());
        for (uint32_t i = 0; i < kSetsPerFrame; ++i)
        {
            const BufferHandle other = CreateZeroedStorageBuffer(*device, kOtherLength);
            ASSERT_TRUE(other.IsValid());
            buffers.push_back(other);
            const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
            ASSERT_TRUE(set.IsValid());
            device->UpdateStorageBufferBinding(set, 0, other, 0, kOtherLength * sizeof(uint32_t));
        }
        device->FinalizeFrame();
    }

    auto release = device->CreateCommandList(IDevice::QueueType::Graphics);
    release->Begin();
    release->End();
    ASSERT_TRUE(device->QueueSubmit(IDevice::QueueType::Graphics, {release.get()}, {}, {{gate, 1}}));
    device->WaitForIdle();

    const uint32_t* mapped = static_cast<const uint32_t*>(device->MapBuffer(held));
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped[0], kHeldLength) << "the held dispatch did not read its own table";

    for (BufferHandle buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }
    device->DestroySemaphore(gate);
    device->Shutdown();
}

namespace
{
// Per pixel of a kA2cDim x kA2cDim target at 4 samples: bit k set when sample k took the depth the
// alpha-to-coverage pass wrote.
struct A2cCoverage
{
    std::vector<uint8_t> SampleBits;
    bool Ok = false;
    std::string Why;
};

constexpr uint32_t kA2cDim = 16;
constexpr uint32_t kA2cSamples = 4;

// Writes depth 0.5 over the whole target through a pipeline with alpha-to-coverage on, whose fragment
// stage writes an alpha that rises across the columns (and steps by row) at location 0: into a pass with
// a colour target (`withColourTarget`), or into a depth-only pass whose pipeline declares no colour
// attachment, as a depth prepass head does. Then four probe passes, one per sample, read the coverage
// back: each draws depth 0.25 against that depth (Greater, reverse-Z: it passes only where the first pass
// wrote nothing) into a cleared multisampled colour target with gl_SampleMask = 1 << k, and resolves it.
// A resolved texel that stayed black means sample k held the written depth.
A2cCoverage RenderAlphaToCoverageDepth(IDevice& device, bool withColourTarget)
{
    A2cCoverage out{};
    static const char* kWriterVert = R"(
#version 450
void main()
{
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
)";
    static const char* kProbeVert = R"(
#version 450
void main()
{
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.25, 1.0);
}
)";
    // 0 at the left edge, opaque at the right; the row adds a quarter step so neighbouring rows land on
    // different coverage levels.
    static const char* kWriterFrag = R"(
#version 450
layout(location = 0) out vec4 oColor;
void main()
{
    float a = (floor(gl_FragCoord.x) + 0.25 * mod(floor(gl_FragCoord.y), 4.0)) / 15.0;
    oColor = vec4(1.0, 1.0, 1.0, clamp(a, 0.0, 1.0));
}
)";
    const std::vector<uint8_t> writerVs = CompileGlsl(kWriterVert, shaderc_vertex_shader, "a2c-writer.vert");
    const std::vector<uint8_t> probeVs = CompileGlsl(kProbeVert, shaderc_vertex_shader, "a2c-probe.vert");
    const std::vector<uint8_t> writerFs = CompileGlsl(kWriterFrag, shaderc_fragment_shader, "a2c-writer.frag");
    std::vector<uint8_t> probeFs[kA2cSamples];
    for (uint32_t k = 0; k < kA2cSamples; ++k)
    {
        const std::string src = "#version 450\nlayout(location = 0) out vec4 oColor;\nvoid main()\n{\n"
                                "    gl_SampleMask[0] = " + std::to_string(1u << k) + ";\n"
                                "    oColor = vec4(1.0);\n}\n";
        probeFs[k] = CompileGlsl(src.c_str(), shaderc_fragment_shader, "a2c-probe.frag");
    }
    if (writerVs.empty() || probeVs.empty() || writerFs.empty() || probeFs[kA2cSamples - 1].empty())
        return out;

    TextureDesc depthDesc{};
    depthDesc.width = kA2cDim;
    depthDesc.height = kA2cDim;
    depthDesc.sampleCount = kA2cSamples;
    depthDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    TextureDesc msColourDesc = depthDesc;
    msColourDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    msColourDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
    TextureDesc resolveDesc = msColourDesc;
    resolveDesc.sampleCount = 1;
    resolveDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    const TextureHandle depth = device.CreateTexture(depthDesc);
    const TextureHandle writerColour = device.CreateTexture(msColourDesc);
    const TextureHandle probeColour = device.CreateTexture(msColourDesc);
    TextureHandle resolves[kA2cSamples];
    BufferHandle readbacks[kA2cSamples];
    for (uint32_t k = 0; k < kA2cSamples; ++k)
    {
        resolves[k] = device.CreateTexture(resolveDesc);
        readbacks[k] = device.CreateReadbackBuffer(static_cast<size_t>(kA2cDim) * kA2cDim * 4);
    }

    PipelineDesc writer{};
    writer.type = PipelineType::Graphics;
    writer.vertexShader = writerVs;
    writer.pixelShader = writerFs;
    if (withColourTarget)
        writer.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
    writer.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    writer.rasterizationSamples = kA2cSamples;
    writer.rasterizationState.cullMode = CullModeFlagBits::None;
    writer.depthStencilState.depthTestEnable = true;
    writer.depthStencilState.depthWriteEnable = true;
    writer.depthStencilState.depthCompareOp = CompareOp::Always;
    writer.colorBlendState.alphaToCoverageEnable = true;
    writer.debugName = withColourTarget ? "A2cWriterColourAndDepth" : "A2cWriterDepthOnly";
    const PipelineHandle writerPipeline = device.CreatePipeline(writer);

    PipelineHandle probePipelines[kA2cSamples];
    for (uint32_t k = 0; k < kA2cSamples; ++k)
    {
        PipelineDesc probe{};
        probe.type = PipelineType::Graphics;
        probe.vertexShader = probeVs;
        probe.pixelShader = probeFs[k];
        probe.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
        probe.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
        probe.rasterizationSamples = kA2cSamples;
        probe.rasterizationState.cullMode = CullModeFlagBits::None;
        probe.depthStencilState.depthTestEnable = true;
        probe.depthStencilState.depthWriteEnable = false;
        probe.depthStencilState.depthCompareOp = CompareOp::Greater;
        probe.debugName = "A2cProbe";
        probePipelines[k] = device.CreatePipeline(probe);
    }

    bool valid = depth.IsValid() && writerColour.IsValid() && probeColour.IsValid() && writerPipeline.IsValid();
    for (uint32_t k = 0; k < kA2cSamples; ++k)
        valid = valid && resolves[k].IsValid() && readbacks[k].IsValid() && probePipelines[k].IsValid();
    if (!valid)
    {
        out.Why = std::string("not created:") + (depth.IsValid() ? "" : " depth") + (writerColour.IsValid() ? "" : " writerColour") +
                  (probeColour.IsValid() ? "" : " probeColour") + (writerPipeline.IsValid() ? "" : " writerPipeline") +
                  (resolves[0].IsValid() ? "" : " resolve") + (readbacks[0].IsValid() ? "" : " readback") +
                  (probePipelines[0].IsValid() ? "" : " probePipeline");
    }
    if (valid)
    {
        auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        RenderPassDesc writePass{};
        if (withColourTarget)
        {
            writePass.colorTargets[0] = writerColour;
            writePass.colorTargetCount = 1;
            writePass.clearColor[0] = true;
        }
        writePass.depthTarget = depth;
        writePass.clearDepth = true;
        writePass.clearDepthValue = 0.0f;
        writePass.depthLoadOp = RenderPassDesc::LoadOp::Clear;
        writePass.depthStoreOp = RenderPassDesc::StoreOp::Store;
        cl->BeginRenderPass(writePass);
        cl->SetPipeline(writerPipeline);
        cl->SetViewport(0.0f, 0.0f, static_cast<float>(kA2cDim), static_cast<float>(kA2cDim));
        cl->SetScissor(0, 0, kA2cDim, kA2cDim);
        cl->Draw(3);
        cl->EndRenderPass();
        for (uint32_t k = 0; k < kA2cSamples; ++k)
        {
            RenderPassDesc probePass{};
            probePass.colorTargets[0] = probeColour;
            probePass.resolveColorTargets[0] = resolves[k];
            probePass.colorTargetCount = 1;
            probePass.clearColor[0] = true;
            probePass.clearColorValue[0][0] = probePass.clearColorValue[0][1] = probePass.clearColorValue[0][2] = 0.0f;
            probePass.clearColorValue[0][3] = 0.0f;
            probePass.depthTarget = depth;
            probePass.clearDepth = false;
            probePass.depthLoadOp = RenderPassDesc::LoadOp::Load;
            probePass.depthStoreOp = RenderPassDesc::StoreOp::Store;
            probePass.depthReadOnly = true;
            cl->BeginRenderPass(probePass);
            cl->SetPipeline(probePipelines[k]);
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(kA2cDim), static_cast<float>(kA2cDim));
            cl->SetScissor(0, 0, kA2cDim, kA2cDim);
            cl->Draw(3);
            cl->EndRenderPass();
            cl->CopyTextureToBuffer(resolves[k], readbacks[k], kA2cDim, kA2cDim);
        }
        cl->End();
        device.ExecuteCommandLists({cl.get()});
        device.WaitForIdle();

        out.SampleBits.assign(static_cast<size_t>(kA2cDim) * kA2cDim, 0u);
        out.Ok = true;
        for (uint32_t k = 0; k < kA2cSamples; ++k)
        {
            const uint8_t* mapped = static_cast<const uint8_t*>(device.MapBuffer(readbacks[k]));
            if (!mapped)
            {
                out.Ok = false;
                out.Why = "readback not mapped";
                break;
            }
            for (size_t i = 0; i < out.SampleBits.size(); ++i)
                out.SampleBits[i] |= mapped[i * 4] == 0u ? static_cast<uint8_t>(1u << k) : 0u;
        }
    }

    for (uint32_t k = 0; k < kA2cSamples; ++k)
    {
        if (probePipelines[k].IsValid())
            device.DestroyPipeline(probePipelines[k]);
        if (readbacks[k].IsValid())
            device.DestroyBuffer(readbacks[k]);
        if (resolves[k].IsValid())
            device.DestroyTexture(resolves[k]);
    }
    if (writerPipeline.IsValid())
        device.DestroyPipeline(writerPipeline);
    device.DestroyTexture(probeColour);
    device.DestroyTexture(writerColour);
    device.DestroyTexture(depth);
    return out;
}

void ExpectDepthOnlyAlphaToCoverageMatchesColour(IDevice& device, const char* backend)
{
    const A2cCoverage colour = RenderAlphaToCoverageDepth(device, /*withColourTarget=*/true);
    const A2cCoverage depthOnly = RenderAlphaToCoverageDepth(device, /*withColourTarget=*/false);
    ASSERT_TRUE(colour.Ok) << backend << ": " << colour.Why;
    ASSERT_TRUE(depthOnly.Ok) << backend << ": " << depthOnly.Why;
    size_t partial = 0;
    size_t differing = 0;
    for (size_t i = 0; i < colour.SampleBits.size(); ++i)
    {
        partial += colour.SampleBits[i] != 0u && colour.SampleBits[i] != 0xFu ? 1u : 0u;
        differing += colour.SampleBits[i] != depthOnly.SampleBits[i] ? 1u : 0u;
    }
    EXPECT_GT(partial, colour.SampleBits.size() / 4)
        << backend << ": with a colour target, alpha-to-coverage left too few pixels partially covered to compare";
    EXPECT_EQ(differing, 0u) << backend << ": of " << colour.SampleBits.size()
                             << " pixels, these keep different samples with no colour target";
}
} // namespace

// Terrain grass's alpha-to-coverage blades draw a depth-only head in the camera prepass (#2433): its
// fragment stage writes the colour draw's alpha at location 0 into a pass with no colour target, and its
// pipeline enables alpha-to-coverage. That holds only if the device turns the alpha of output 0 into the
// sample mask whether or not a colour attachment is bound. Every pixel keeps the same samples either way,
// and a quarter or more of them are partially covered (an alpha the device ignored would cover them all).
// On Metal here; on Vulkan in the next test, so a pass says which backends ran.
TEST(MetalSpirvPipeline, DepthOnlyAlphaToCoverageKeepsTheSamplesAColourTargetKeeps)
{
    auto metal = CreateHeadlessMetalDevice();
    if (!metal)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    ExpectDepthOnlyAlphaToCoverageMatchesColour(*metal, "Metal");
    metal->Shutdown();
}

// The same on Vulkan (MoltenVK on a Mac). Skips when no Vulkan device is created, rather than passing.
TEST(MetalSpirvPipeline, DepthOnlyAlphaToCoverageKeepsTheSamplesAColourTargetKeepsOnVulkan)
{
    DeviceDesc vulkanDesc{};
    vulkanDesc.preferredAPI = GraphicsAPI::Vulkan;
    vulkanDesc.enableSwapchain = false;
    vulkanDesc.enableDynamicRendering = true;
    auto vulkan = DeviceFactory::CreateDevice(vulkanDesc);
    if (!vulkan || !vulkan->Initialize(vulkanDesc) || vulkan->GetAPI() != GraphicsAPI::Vulkan)
    {
        GTEST_SKIP() << "no Vulkan device on this machine";
    }
    ExpectDepthOnlyAlphaToCoverageMatchesColour(*vulkan, "Vulkan");
    vulkan->Shutdown();
}

#endif // RENDERING_HAS_SHADERC

// The renderer creates a transient descriptor set for every material bind in
// every frame, so creating one must cost no Metal allocation once every frame
// slot has served a frame of that size.
TEST(MetalDescriptorSets, TransientSetsAllocateNoMetalMemoryOnceWarm)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    MTL::Device* metalDevice = static_cast<MetalDevice*>(device.get())->GetMTLDevice();

    BufferDesc uniformDesc{};
    uniformDesc.size = 256;
    uniformDesc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
    BufferHandle uniform = device->CreateBuffer(uniformDesc);
    ASSERT_TRUE(uniform.IsValid());
    BufferDesc storageDesc{};
    storageDesc.size = 1024;
    storageDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    BufferHandle storage = device->CreateBuffer(storageDesc);
    ASSERT_TRUE(storage.IsValid());

    DescriptorSetDesc setDesc{};
    setDesc.transient = true;
    {
        DescriptorBinding uniformBinding{};
        uniformBinding.binding = 0;
        uniformBinding.type = DescriptorType::UniformBuffer;
        uniformBinding.shaderStages = kShaderStageFragment;
        setDesc.layout.bindings.push_back(uniformBinding);
        DescriptorBinding storageBinding{};
        storageBinding.binding = 1;
        storageBinding.type = DescriptorType::StorageBuffer;
        storageBinding.shaderStages = kShaderStageFragment;
        setDesc.layout.bindings.push_back(storageBinding);
    }

    // More tables than one arena page holds, so the warm state spans pages.
    constexpr uint32_t kSetsPerFrame = 1500;
    constexpr uint32_t kMeasuredFrames = 3;
    const uint32_t warmUpFrames = device->GetFramesInFlight();
    for (uint32_t frame = 0; frame < warmUpFrames + kMeasuredFrames; ++frame)
    {
        ASSERT_TRUE(device->BeginFrame());
        const size_t allocatedBefore = metalDevice->currentAllocatedSize();
        for (uint32_t i = 0; i < kSetsPerFrame; ++i)
        {
            DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
            ASSERT_TRUE(set.IsValid());
            device->UpdateBufferBinding(set, 0, uniform, 0, uniformDesc.size);
            device->UpdateStorageBufferBinding(set, 1, storage, 0, storageDesc.size);
        }
        const size_t allocatedAfter = metalDevice->currentAllocatedSize();
        device->FinalizeFrame();
        if (frame >= warmUpFrames)
        {
            EXPECT_LE(allocatedAfter, allocatedBefore)
                << "frame " << frame << ": " << kSetsPerFrame << " transient sets allocated "
                << allocatedAfter - allocatedBefore << " bytes of Metal memory";
        }
    }

    device->WaitForIdle();
    device->DestroyBuffer(storage);
    device->DestroyBuffer(uniform);
    device->Shutdown();
}

// --- GPU timestamp spans (MetalQueryPool + MetalCommandList) ---------------
//
// Apple GPUs sample counters only at encoder stage boundaries, so a timing
// span can only ever name the encoder(s) that carry the bracketed work. These
// pin that mapping: a span must sample ITS OWN encoder's boundaries (a span
// whose begin lands on the PREVIOUS encoder's end reports the gap since that
// encoder instead of its own work), and two spans that share one encoder must
// be reported as one measurement rather than two.

namespace
{
struct SpanResult
{
    uint32_t BeginQuery = ~0u;
    uint32_t EndQuery = ~0u;
    uint32_t BeginUnit = IQueryPool::kInvalidTimestampUnit;
    uint32_t EndUnit = IQueryPool::kInvalidTimestampUnit;
    uint64_t BeginTicks = 0;
    uint64_t EndTicks = 0;
    bool Resolved = false;
};
} // namespace

TEST(MetalTimestampSpans, SpanSamplesItsOwnEncoderBoundaries)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    IQueryPool* pool = device->GetQueryPool();
    if (pool == nullptr || !pool->IsValid())
    {
        GTEST_SKIP() << "device has no timestamp query pool";
    }
    EXPECT_EQ(pool->GetTimestampSemantics(), TimestampSemantics::EncoderSpan)
        << "stage-boundary sampling can only time whole encoders";

    constexpr size_t kSize = 256;
    BufferDesc bd{};
    bd.size = kSize;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle src = device->CreateBuffer(bd);
    BufferHandle dst = device->CreateBuffer(bd);
    ASSERT_TRUE(src.IsValid());
    ASSERT_TRUE(dst.IsValid());

    TextureDesc td{};
    td.width = 32;
    td.height = 32;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle rt = device->CreateTexture(td);
    ASSERT_TRUE(rt.IsValid());

    const uint32_t slot = device->GetFrameIndex();
    pool->BeginFrame(slot);

    SpanResult copySpan;
    SpanResult drawSpan;
    SpanResult sharedA;
    SpanResult sharedB;

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();

    // Span 1: one blit encoder of its own.
    copySpan.BeginQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    copySpan.EndQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);

    // Span 2: a render pass, which forces a different (render) encoder.
    drawSpan.BeginQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->EndRenderPass();
    drawSpan.EndQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);

    // Spans 3 and 4: two copies with nothing between them, so Metal keeps one
    // blit encoder open across both — one measurement, two spans.
    sharedA.BeginQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    sharedA.EndQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    sharedB.BeginQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    sharedB.EndQuery = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);

    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();

    // The slot's work is complete, so collecting it resolves without stalling.
    pool->BeginFrame(slot);

    auto resolve = [&](SpanResult& s)
    {
        QueryResult b{}, e{};
        s.Resolved = pool->GetTimestampResult(s.BeginQuery, b) && b.Available &&
                     pool->GetTimestampResult(s.EndQuery, e) && e.Available;
        s.BeginTicks = b.Value;
        s.EndTicks = e.Value;
        s.BeginUnit = pool->GetTimestampUnitId(s.BeginQuery);
        s.EndUnit = pool->GetTimestampUnitId(s.EndQuery);
    };
    resolve(copySpan);
    resolve(drawSpan);
    resolve(sharedA);
    resolve(sharedB);

    ASSERT_TRUE(copySpan.Resolved) << "timestamps were not written back";
    ASSERT_TRUE(drawSpan.Resolved);
    ASSERT_TRUE(sharedA.Resolved);
    ASSERT_TRUE(sharedB.Resolved);

    // Each span's two ends must name the SAME encoder: its own.
    EXPECT_NE(copySpan.BeginUnit, IQueryPool::kInvalidTimestampUnit);
    EXPECT_EQ(copySpan.BeginUnit, copySpan.EndUnit);
    EXPECT_NE(drawSpan.BeginUnit, IQueryPool::kInvalidTimestampUnit);
    EXPECT_EQ(drawSpan.BeginUnit, drawSpan.EndUnit)
        << "a span's begin must sample its own encoder's start, not the previous encoder's end";
    // ... and the render pass is a different encoder from the copy.
    EXPECT_NE(copySpan.BeginUnit, drawSpan.BeginUnit);

    // Spans that shared one encoder are one measurement, reported identically.
    EXPECT_EQ(sharedA.BeginUnit, sharedA.EndUnit);
    EXPECT_EQ(sharedA.BeginUnit, sharedB.BeginUnit);
    EXPECT_EQ(sharedA.EndUnit, sharedB.EndUnit);
    EXPECT_EQ(sharedA.EndTicks - sharedA.BeginTicks, sharedB.EndTicks - sharedB.BeginTicks);
    // The span whose copy joined the encoder another span opened is charged
    // that encoder, never zero: its begin binds to the encoder's start, not to
    // the end boundary its end also reads.
    EXPECT_GT(sharedB.EndTicks - sharedB.BeginTicks, 0u);

    // Monotonic within a span, and none of them spans the whole command buffer.
    EXPECT_GE(copySpan.EndTicks, copySpan.BeginTicks);
    EXPECT_GE(drawSpan.EndTicks, drawSpan.BeginTicks);
    const double commandBufferMs =
        pool->TimestampToMs(std::max(drawSpan.EndTicks, sharedB.EndTicks) - copySpan.BeginTicks);
    EXPECT_LE(pool->TimestampToMs(drawSpan.EndTicks - drawSpan.BeginTicks), commandBufferMs);

    device->DestroyTexture(rt);
    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Shutdown();
}

// A span over several encoders must be charged their work, not the wall extent
// from the first one's start to the last one's end — the GPU can idle between
// them, and a render encoder's vertex and fragment stages are scheduled apart.
TEST(MetalTimestampSpans, MultiEncoderSpanExcludesTheGapsBetweenEncoders)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    IQueryPool* pool = device->GetQueryPool();
    if (pool == nullptr || !pool->IsValid())
    {
        GTEST_SKIP() << "device has no timestamp query pool";
    }

    constexpr size_t kSize = 4096;
    BufferDesc bd{};
    bd.size = kSize;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle src = device->CreateBuffer(bd);
    BufferHandle dst = device->CreateBuffer(bd);
    ASSERT_TRUE(src.IsValid());
    ASSERT_TRUE(dst.IsValid());
    TextureDesc td{};
    td.width = 256;
    td.height = 256;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    TextureHandle rt = device->CreateTexture(td);
    ASSERT_TRUE(rt.IsValid());

    const uint32_t slot = device->GetFrameIndex();
    pool->BeginFrame(slot);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    // A span with work FIRST, so the span under test is not the command list's
    // first: a begin that binds to the previous encoder's end (the defect this
    // pins) is only distinguishable once an encoder has already closed.
    const uint32_t leadBegin = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    const uint32_t leadEnd = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    // One span, three encoders: blit, render, blit.
    const uint32_t spanBegin = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    RenderPassDesc pass{};
    pass.colorTargets[0] = rt;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    cl->BeginRenderPass(pass);
    cl->EndRenderPass();
    cl->CopyBuffer(src, dst, kSize);
    const uint32_t spanEnd = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();
    pool->BeginFrame(slot);

    QueryResult b{}, e{};
    ASSERT_TRUE(pool->GetTimestampResult(spanBegin, b) && b.Available);
    ASSERT_TRUE(pool->GetTimestampResult(spanEnd, e) && e.Available);

    uint64_t workTicks = 0;
    ASSERT_TRUE(pool->GetTimestampSpanTicks(spanBegin, spanEnd, workTicks))
        << "encoder-span backends must account per work unit";
    // NOT compared against the begin..end extent: the GPU may run the encoders
    // of one command buffer out of submission order (observed on M4 Max), so
    // the last encoder's end boundary can precede the first one's start and the
    // extent is neither an upper nor a lower bound on their summed work.
    EXPECT_GT(workTicks, 0u) << "three encoders of real work cannot sum to nothing";
    // The span really did cover more than one encoder.
    EXPECT_NE(pool->GetTimestampUnitId(spanBegin), pool->GetTimestampUnitId(spanEnd));

    // The span starts at the FIRST encoder created inside it — the one after
    // the lead span's — and not at the lead encoder it followed, whose work it
    // must not absorb.
    const uint32_t leadUnit = pool->GetTimestampUnitId(leadEnd);
    ASSERT_NE(leadUnit, IQueryPool::kInvalidTimestampUnit);
    EXPECT_EQ(pool->GetTimestampUnitId(spanBegin), leadUnit + 1)
        << "a span's begin must name its own first encoder, not the previous span's last";
    uint64_t leadTicks = 0;
    ASSERT_TRUE(pool->GetTimestampSpanTicks(leadBegin, leadEnd, leadTicks));
    uint64_t bothTicks = 0;
    ASSERT_TRUE(pool->GetTimestampSpanTicks(leadBegin, spanEnd, bothTicks));
    EXPECT_EQ(bothTicks, leadTicks + workTicks)
        << "the two spans' work must partition, with no encoder counted in both";

    device->DestroyTexture(rt);
    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Shutdown();
}

// A span that records no GPU work at all has no encoder to name; it must report
// zero, not the interval up to whatever encoder happens to come next.
TEST(MetalTimestampSpans, SpanWithoutGpuWorkReportsZero)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    IQueryPool* pool = device->GetQueryPool();
    if (pool == nullptr || !pool->IsValid())
    {
        GTEST_SKIP() << "device has no timestamp query pool";
    }

    constexpr size_t kSize = 256;
    BufferDesc bd{};
    bd.size = kSize;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle src = device->CreateBuffer(bd);
    BufferHandle dst = device->CreateBuffer(bd);
    ASSERT_TRUE(src.IsValid());
    ASSERT_TRUE(dst.IsValid());

    const uint32_t slot = device->GetFrameIndex();
    pool->BeginFrame(slot);

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    // A first span with real work, so an encoder exists to attribute against.
    const uint32_t workBegin = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    const uint32_t workEnd = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    // An empty span: markers record no GPU work of their own.
    const uint32_t emptyBegin = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->SetMarker("EmptyPass");
    const uint32_t emptyEnd = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    // A second span with work, which the empty span must NOT reach into.
    const uint32_t tailBegin = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanBegin);
    cl->CopyBuffer(src, dst, kSize);
    const uint32_t tailEnd = pool->WriteTimestamp(cl.get(), TimestampPoint::SpanEnd);
    cl->End();
    device->ExecuteCommandLists({cl.get()});
    device->WaitForIdle();
    pool->BeginFrame(slot);

    QueryResult wb{}, we{}, eb{}, ee{}, tb{}, te{};
    ASSERT_TRUE(pool->GetTimestampResult(workBegin, wb) && wb.Available);
    ASSERT_TRUE(pool->GetTimestampResult(workEnd, we) && we.Available);
    ASSERT_TRUE(pool->GetTimestampResult(emptyBegin, eb) && eb.Available);
    ASSERT_TRUE(pool->GetTimestampResult(emptyEnd, ee) && ee.Available);
    ASSERT_TRUE(pool->GetTimestampResult(tailBegin, tb) && tb.Available);
    ASSERT_TRUE(pool->GetTimestampResult(tailEnd, te) && te.Available);

    EXPECT_EQ(ee.Value, eb.Value) << "a span with no encoder of its own must measure zero";
    uint64_t emptyTicks = 1;
    ASSERT_TRUE(pool->GetTimestampSpanTicks(emptyBegin, emptyEnd, emptyTicks));
    EXPECT_EQ(emptyTicks, 0u);
    EXPECT_GE(te.Value, tb.Value);
    // All three spans sit in one blit encoder (a marker opens none), so the two
    // that recorded a copy must both report that encoder — equally. A begin
    // left bound to the previous span's END boundary instead would collapse the
    // tail span to zero while the lead span kept the encoder's full span.
    const uint32_t tailBeginUnit = pool->GetTimestampUnitId(tailBegin);
    ASSERT_NE(tailBeginUnit, IQueryPool::kInvalidTimestampUnit);
    EXPECT_EQ(tailBeginUnit, pool->GetTimestampUnitId(tailEnd))
        << "the span following a zero-work span must not straddle encoders";
    uint64_t leadTicks = 0;
    uint64_t tailTicks = 0;
    ASSERT_TRUE(pool->GetTimestampSpanTicks(workBegin, workEnd, leadTicks));
    ASSERT_TRUE(pool->GetTimestampSpanTicks(tailBegin, tailEnd, tailTicks));
    EXPECT_EQ(tailTicks, leadTicks)
        << "both spans recorded into the same encoder and must read it identically";
    EXPECT_GT(tailTicks, 0u) << "a span that recorded a copy into an open encoder reads that encoder, not zero";

    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Shutdown();
}

// The render graph folds a span shared by several passes into one measurement.
// Metal is the backend that produces such spans (Vulkan's per-command
// timestamps never share), so the fold has to be exercised here: two passes
// whose work lands in one blit encoder must report one distinct measurement,
// not two.
TEST(MetalTimestampSpans, RenderGraphFoldsOneEncoderSharedByTwoPassesIntoOneMeasurement)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    IQueryPool* pool = device->GetQueryPool();
    if (pool == nullptr || !pool->IsValid())
    {
        GTEST_SKIP() << "device has no timestamp query pool";
    }

    constexpr size_t kSize = 4096;
    BufferDesc bd{};
    bd.size = kSize;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::TransferDst);
    BufferHandle src = device->CreateBuffer(bd);
    BufferHandle dst = device->CreateBuffer(bd);
    ASSERT_TRUE(src.IsValid());
    ASSERT_TRUE(dst.IsValid());

    BufferDesc storage{};
    storage.size = 256;
    storage.usage = static_cast<uint32_t>(BufferUsage::Storage);

    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transient(device.get());
    RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
    RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
    frame.SetProfilingEnabled(true);

    // Two frames: per-pass results resolve one slot-collection later.
    for (uint64_t n = 0; n < 2; ++n)
    {
        frame.BeginFrame(n);
        // Independent writes, so the graph orders nothing between the passes and
        // both copies land in one blit encoder (Metal barriers are no-ops, and a
        // first-touch barrier is hoisted to the submission front regardless).
        RenderGraph::RGBuffer a = frame.CreateBuffer("A", storage);
        RenderGraph::RGBuffer b = frame.CreateBuffer("B", storage);
        frame.AddPass("CopyA", 0, [&](RenderGraph::RGPassBuilder& p) { p.Write(a); },
                      [=](RenderGraph::RGContext& ctx) { ctx.Cmd->CopyBuffer(src, dst, kSize); });
        frame.AddPass("CopyB", 0, [&](RenderGraph::RGPassBuilder& p) { p.Write(b); },
                      [=](RenderGraph::RGContext& ctx) { ctx.Cmd->CopyBuffer(src, dst, kSize); });
        frame.MarkOutput(a);
        frame.MarkOutput(b);
        frame.Execute();
        device->WaitForIdle();
        pool->BeginFrame(device->GetFrameIndex());
    }

    const auto timings = frame.LastFrameTimings();
    const auto stats = frame.LastResolveStats();
    ASSERT_EQ(frame.TimingSemantics(), TimestampSemantics::EncoderSpan);
    ASSERT_EQ(timings.size(), 2u);
    ASSERT_EQ(stats.ResolvedPasses, 2u);
    ASSERT_EQ(stats.NonMonotonic, 0u);

    // One encoder carried both passes, so both read the same span...
    EXPECT_EQ(timings[0].BeginUnit, timings[1].BeginUnit);
    EXPECT_EQ(timings[0].EndUnit, timings[1].EndUnit);
    EXPECT_DOUBLE_EQ(timings[0].GpuSpanMs, timings[1].GpuSpanMs);
    // The pass that joined the encoder the other pass opened reads that
    // encoder's span, not 0 ms.
    EXPECT_GT(timings[1].GpuSpanMs, 0.0);
    EXPECT_TRUE(timings[0].SpanShared);
    EXPECT_TRUE(timings[1].SpanShared);
    // ... and the frame total counts it once, through exactly one pass.
    EXPECT_EQ(stats.SharedSpanPasses, 1u);
    EXPECT_NE(timings[0].SpanCounted, timings[1].SpanCounted);
    EXPECT_DOUBLE_EQ(stats.DistinctSpanGpuMs, timings[0].GpuSpanMs);

    device->DestroyBuffer(src);
    device->DestroyBuffer(dst);
    device->Shutdown();
}

// ClearColorImageSubresource is a transfer clear: the engine contract (and
// vkCmdClearColorImage) needs only TransferDst usage. Metal's render-pass
// clear needs MTLTextureUsageRenderTarget, which a texture created
// ShaderResource | TransferDst does not carry, so those textures clear by a
// blit. The MetalDebugLayer ctest entry runs these with MTL_DEBUG_LAYER=1,
// where a render-pass clear of such a texture aborts the process.
namespace
{

TextureHandle CreateClearTexture(IDevice& device, TextureFormat format, uint32_t width, uint32_t height,
                                 bool renderTarget, const char* name, uint32_t mips = 1, uint32_t layers = 1,
                                 uint32_t depth = 1)
{
    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.mipLevels = mips;
    desc.arrayLayers = layers;
    desc.format = static_cast<uint32_t>(format);
    TextureUsage usage = TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst;
    if (renderTarget)
    {
        usage = usage | TextureUsage::RenderTarget;
    }
    desc.usage = static_cast<uint32_t>(usage);
    desc.debugName = name;
    return device.CreateTexture(desc);
}

// The tightly packed bytes of one subresource (every slice of a volume).
std::vector<uint8_t> ReadBackSubresource(IDevice& device, TextureHandle texture, uint32_t mip, uint32_t layer,
                                         uint32_t width, uint32_t height, uint32_t texelBytes, uint32_t depth = 1)
{
    const size_t bytes = static_cast<size_t>(width) * height * depth * texelBytes;
    const BufferHandle readback = device.CreateReadbackBuffer(bytes, "ClearReadback");
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyTextureSubresourceToBuffer(texture, mip, layer, readback, width, height, 0, 0, 0, 0, depth, 0);
    cl->End();
    device.ExecuteCommandLists({cl.get()});
    device.WaitForIdle();
    std::vector<uint8_t> out(bytes);
    const auto* mapped = static_cast<const uint8_t*>(device.MapBuffer(readback));
    if (mapped != nullptr)
    {
        std::memcpy(out.data(), mapped, bytes);
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return out;
}

void ClearSubresource(IDevice& device, TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4])
{
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->ClearColorImageSubresource(texture, mip, layer, rgba);
    cl->End();
    device.ExecuteCommandLists({cl.get()});
    device.WaitForIdle();
}

size_t CountFloatsNotEqualTo(const std::vector<uint8_t>& bytes, float expected)
{
    size_t mismatches = 0;
    for (size_t offset = 0; offset + sizeof(float) <= bytes.size(); offset += sizeof(float))
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        mismatches += value != expected ? 1u : 0u;
    }
    return mismatches;
}

} // namespace

// Every texel of exactly the named subresource: a mip and layer of an array
// texture large enough to take more than one staging band, and every slice of
// a volume (a volume's layer-0 subresource is its whole mip, as in Vulkan).
TEST(MetalClear, ClearsATextureWithoutRenderTargetUsage)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    // 1024 x 300 R32_FLOAT is 1.2 MiB per layer: two bands of rows.
    constexpr uint32_t kWidth = 1024;
    constexpr uint32_t kHeight = 300;
    const TextureHandle array = CreateClearTexture(*device, TextureFormat::R32_FLOAT, kWidth, kHeight,
                                                   /*renderTarget=*/false, "ClearArray", /*mips=*/2, /*layers=*/2);
    ASSERT_TRUE(array.IsValid());
    const float layer0[4] = {0.75f, 0.0f, 0.0f, 0.0f};
    const float layer1[4] = {0.25f, 0.0f, 0.0f, 0.0f};
    const float layer1Mip1[4] = {-2.0f, 0.0f, 0.0f, 0.0f};
    ClearSubresource(*device, array, 0, 0, layer0);
    ClearSubresource(*device, array, 0, 1, layer1);
    ClearSubresource(*device, array, 1, 1, layer1Mip1);
    EXPECT_EQ(CountFloatsNotEqualTo(ReadBackSubresource(*device, array, 0, 0, kWidth, kHeight, 4), 0.75f), 0u);
    EXPECT_EQ(CountFloatsNotEqualTo(ReadBackSubresource(*device, array, 0, 1, kWidth, kHeight, 4), 0.25f), 0u);
    EXPECT_EQ(CountFloatsNotEqualTo(ReadBackSubresource(*device, array, 1, 1, kWidth / 2, kHeight / 2, 4), -2.0f),
              0u);

    constexpr uint32_t kVolumeDim = 4;
    constexpr uint32_t kSlices = 3;
    const TextureHandle volume = CreateClearTexture(*device, TextureFormat::R32_FLOAT, kVolumeDim, kVolumeDim,
                                                    /*renderTarget=*/false, "ClearVolume", 1, 1, kSlices);
    ASSERT_TRUE(volume.IsValid());
    const float half[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    ClearSubresource(*device, volume, 0, 0, half);
    EXPECT_EQ(CountFloatsNotEqualTo(
                  ReadBackSubresource(*device, volume, 0, 0, kVolumeDim, kVolumeDim, 4, kSlices), 0.5f),
              0u);

    device->DestroyTexture(volume);
    device->DestroyTexture(array);
    device->Shutdown();
}

// The blit stores the bytes a render-pass clear to the same color stores, in
// every color format either path can clear: two textures per format, one with
// render-target usage (render-pass clear) and one without (blit), are cleared
// alike and read back. UNORM and sRGB channels may differ by one step (the
// GPU's own rounding); the rest match exactly.
TEST(MetalClear, TransferClearStoresWhatARenderPassClearStores)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }

    enum class Compare
    {
        Exact,
        UnormStep8,
        Packed1010102,
        Packed111110,
    };
    struct Case
    {
        TextureFormat Format;
        float Color[4];
        Compare Mode;
    };
    const Case kCases[] = {
        {TextureFormat::RGBA8_UNORM, {0.2f, 0.4f, 0.6f, 0.8f}, Compare::UnormStep8},
        {TextureFormat::BGRA8_UNORM, {0.2f, 0.4f, 0.6f, 0.8f}, Compare::UnormStep8},
        {TextureFormat::RGBA8_SRGB, {0.2f, 0.4f, 0.6f, 0.8f}, Compare::UnormStep8},
        {TextureFormat::BGRA8_SRGB, {0.05f, 0.4f, 0.9f, 0.3f}, Compare::UnormStep8},
        {TextureFormat::R8_UNORM, {0.3f, 0.0f, 0.0f, 0.0f}, Compare::UnormStep8},
        {TextureFormat::R8G8_UNORM, {1.5f, -0.5f, 0.0f, 0.0f}, Compare::UnormStep8},
        {TextureFormat::R8_UINT, {7.0f, 0.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::RGBA8_SINT, {-3.0f, 5.0f, -100.0f, 100.0f}, Compare::Exact},
        {TextureFormat::R16G16_UINT, {300.0f, 65535.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::RGBA16_SINT, {-2.0f, 2.0f, -30000.0f, 30000.0f}, Compare::Exact},
        {TextureFormat::R32_UINT, {123456.0f, 0.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::RGBA32_SINT, {-7.0f, 8.0f, -9.0f, 10.0f}, Compare::Exact},
        {TextureFormat::R16_FLOAT, {0.1f, 0.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::R16G16_FLOAT, {-1.25f, 1000.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::R16G16B16A16_FLOAT, {0.1f, 0.2f, 0.3f, 1.0f}, Compare::Exact},
        {TextureFormat::R16G16B16A16_UNORM, {0.2f, 0.4f, 0.6f, 1.0f}, Compare::Exact},
        {TextureFormat::R32_FLOAT, {0.1f, 0.0f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::R32G32_FLOAT, {-3.5f, 1e6f, 0.0f, 0.0f}, Compare::Exact},
        {TextureFormat::R32G32B32A32_FLOAT, {0.1f, -0.2f, 0.3f, 4.0f}, Compare::Exact},
        {TextureFormat::RGB10A2_UNORM, {0.2f, 0.4f, 0.6f, 0.7f}, Compare::Packed1010102},
        {TextureFormat::R11G11B10_FLOAT, {0.1f, 2.5f, 0.3f, 0.0f}, Compare::Packed111110},
    };

    constexpr uint32_t kDim = 4;
    for (const Case& c : kCases)
    {
        const uint32_t texelBytes = BytesPerPixel(c.Format);
        SCOPED_TRACE(::testing::Message() << "format " << static_cast<uint32_t>(c.Format));
        const TextureHandle viaRenderPass =
            CreateClearTexture(*device, c.Format, kDim, kDim, /*renderTarget=*/true, "ClearByRenderPass");
        const TextureHandle viaBlit =
            CreateClearTexture(*device, c.Format, kDim, kDim, /*renderTarget=*/false, "ClearByBlit");
        ASSERT_TRUE(viaRenderPass.IsValid());
        ASSERT_TRUE(viaBlit.IsValid());
        ClearSubresource(*device, viaRenderPass, 0, 0, c.Color);
        ClearSubresource(*device, viaBlit, 0, 0, c.Color);
        const std::vector<uint8_t> expected = ReadBackSubresource(*device, viaRenderPass, 0, 0, kDim, kDim, texelBytes);
        const std::vector<uint8_t> actual = ReadBackSubresource(*device, viaBlit, 0, 0, kDim, kDim, texelBytes);
        ASSERT_EQ(expected.size(), actual.size());

        for (size_t texel = 0; texel < expected.size() / texelBytes; ++texel)
        {
            const uint8_t* e = expected.data() + texel * texelBytes;
            const uint8_t* a = actual.data() + texel * texelBytes;
            if (c.Mode == Compare::UnormStep8)
            {
                for (uint32_t i = 0; i < texelBytes; ++i)
                {
                    EXPECT_LE(std::abs(int(e[i]) - int(a[i])), 1) << "texel " << texel << " byte " << i;
                }
                continue;
            }
            if (c.Mode == Compare::Exact)
            {
                EXPECT_EQ(std::memcmp(e, a, texelBytes), 0) << "texel " << texel;
                continue;
            }
            uint32_t ev = 0;
            uint32_t av = 0;
            std::memcpy(&ev, e, 4);
            std::memcpy(&av, a, 4);
            const std::array<std::pair<uint32_t, uint32_t>, 4> fields =
                c.Mode == Compare::Packed1010102
                    ? std::array<std::pair<uint32_t, uint32_t>, 4>{{{0, 10}, {10, 10}, {20, 10}, {30, 2}}}
                    : std::array<std::pair<uint32_t, uint32_t>, 4>{{{0, 11}, {11, 11}, {22, 10}, {0, 0}}};
            for (const auto& [shift, bits] : fields)
            {
                if (bits == 0)
                {
                    continue;
                }
                const int mask = (1 << bits) - 1;
                EXPECT_LE(std::abs(int((ev >> shift) & mask) - int((av >> shift) & mask)), 1)
                    << "texel " << texel << " field at bit " << shift;
            }
        }
        device->DestroyTexture(viaBlit);
        device->DestroyTexture(viaRenderPass);
    }
    device->Shutdown();
}

// The MetalDebugLayer ctest entry is only evidence while the layer is really
// loaded: Metal wraps the device in its debug class exactly when
// MTL_DEBUG_LAYER is set before the device is created.
TEST(MetalDebugLayer, WrapsTheDeviceExactlyWhenRequested)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    const char* requested = std::getenv("MTL_DEBUG_LAYER");
    const bool layerRequested = requested != nullptr && requested[0] != '0';
    const std::string deviceClass = object_getClassName(reinterpret_cast<id>(static_cast<MetalDevice&>(*device).GetMTLDevice()));
    EXPECT_EQ(deviceClass.find("Debug") != std::string::npos, layerRequested) << "device class " << deviceClass;
    device->Shutdown();
}

#if RENDERING_HAS_SHADERC && RENDERING_ENABLE_SPIRV_REFLECTION

// PushBlockMslLayout::MslSize is what ShaderReflectionMetadata's whole-tree
// push-block check relies on; here Metal itself reports the size of the
// struct it binds for each block shape the trap covers.
TEST(MetalSpirvPipeline, MslPushStructIsTheBlockRoundedUpToItsLargestAlignment)
{
    auto device = CreateHeadlessMetalDevice();
    if (!device)
    {
        GTEST_SKIP() << "Metal backend unavailable";
    }
    struct Shape
    {
        const char* Members; // GLSL member declarations
        const char* Sum;     // an expression reading every member through `pc`
        uint32_t SpirvEnd;
        uint32_t MslSize;
    };
    const Shape kShapes[] = {
        {"ivec2 a; int b; int c; int d;", "float(pc.a.x + pc.b + pc.c + pc.d)", 20, 24},
        {"ivec2 a; int b; int c; int d; int pad;", "float(pc.a.x + pc.b + pc.c + pc.d + pc.pad)", 24, 24},
        {"vec4 a; vec2 b; float c; float d; float e; float f;", "pc.a.x + pc.b.x + pc.c + pc.d + pc.e + pc.f", 40, 48},
        {"mat4 a; mat4 b; uint c; uint d;", "pc.a[0].x + pc.b[0].x + float(pc.c + pc.d)", 136, 144},
        {"float a; vec3 b;", "pc.a + pc.b.x", 28, 32},
        {"float a; float b; vec2 c; uint d;", "pc.a + pc.b + pc.c.x + float(pc.d)", 20, 24},
        {"uint a; uint b; uint c;", "float(pc.a + pc.b + pc.c)", 12, 12},
    };

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(storage);
    const std::vector<const DescriptorSetLayoutDesc*> layouts{&layout};
    MTL::Device* mtl = static_cast<MetalDevice&>(*device).GetMTLDevice();

    for (const Shape& shape : kShapes)
    {
        SCOPED_TRACE(shape.Members);
        const std::string glsl = std::string("#version 450\nlayout(local_size_x = 1) in;\n"
                                             "layout(set = 0, binding = 0, std430) buffer Out { float v; } uOut;\n"
                                             "layout(push_constant) uniform Push { ") +
                                 shape.Members + " } pc;\nvoid main() { uOut.v = " + shape.Sum + "; }\n";
        const std::vector<uint8_t> spv = CompileGlsl(glsl.c_str(), shaderc_compute_shader, "push_shape.comp");
        ASSERT_FALSE(spv.empty()) << glsl;

        ReflectionOptions opts{};
        StageReflectionResult reflected{};
        std::string reflectError;
        ASSERT_TRUE(ReflectSpirv(ShaderStageKind::Compute, reinterpret_cast<const uint32_t*>(spv.data()),
                                 spv.size() / 4, opts, reflected, &reflectError))
            << reflectError;
        ASSERT_EQ(reflected.Pcr.size(), 1u);
        EXPECT_EQ(PushBlockMslLayout::SpirvEnd(reflected.Pcr[0].Block), shape.SpirvEnd);
        EXPECT_EQ(PushBlockMslLayout::MslSize(reflected.Pcr[0].Block), shape.MslSize);

        const MetalShaderTranslation translation = TranslateSpirvToMsl(spv, MetalShaderStage::Compute, layouts, nullptr);
        ASSERT_TRUE(translation.Success) << translation.Error;
        NS::Error* error = nullptr;
        MTL::Library* library =
            mtl->newLibrary(NS::String::string(translation.Msl.c_str(), NS::UTF8StringEncoding), nullptr, &error);
        ASSERT_NE(library, nullptr) << translation.Msl;
        MTL::Function* function =
            library->newFunction(NS::String::string(translation.EntryPoint.c_str(), NS::UTF8StringEncoding));
        ASSERT_NE(function, nullptr);
        MTL::ComputePipelineReflection* reflection = nullptr;
        MTL::ComputePipelineState* pipeline = mtl->newComputePipelineState(
            function, MTL::PipelineOptionBindingInfo | MTL::PipelineOptionBufferTypeInfo, &reflection, &error);
        ASSERT_NE(pipeline, nullptr);
        ASSERT_NE(reflection, nullptr);
        uint32_t metalBytes = 0;
        NS::Array* bindings = reflection->bindings();
        for (NS::UInteger i = 0; i < bindings->count(); ++i)
        {
            auto* binding = bindings->object<MTL::Binding>(i);
            if (binding->type() == MTL::BindingTypeBuffer && binding->index() == kMetalPushConstantBufferIndex)
                metalBytes = static_cast<uint32_t>(static_cast<MTL::BufferBinding*>(binding)->bufferDataSize());
        }
        EXPECT_EQ(metalBytes, shape.MslSize) << translation.Msl;
        pipeline->release();
        function->release();
        library->release();
    }
    device->Shutdown();
}

#endif // RENDERING_HAS_SHADERC && RENDERING_ENABLE_SPIRV_REFLECTION
