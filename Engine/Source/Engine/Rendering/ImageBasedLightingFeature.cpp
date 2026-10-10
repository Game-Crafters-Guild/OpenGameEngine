#include "Engine/Rendering/ImageBasedLightingFeature.h"

#include "Engine/Rendering/IEnvironmentSource.h" // complete type for the owned m_Source unique_ptr
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace GameEngine::Rendering;

namespace
{
// IEEE-754 half-float bit patterns for the BRDF-LUT fallback values.
constexpr uint16_t kHalfOne = 0x3C00;  // 1.0
constexpr uint16_t kHalfZero = 0x0000; // 0.0

// Minimal f32 -> f16 (IEEE half) for seeding RGBA16F textures with constants.
// Round-to-zero, flushes denormals/underflow to 0 — adequate for seed values.
uint16_t FloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return static_cast<uint16_t>(sign); // underflow -> +/-0
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u); // overflow -> inf
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// Create a CubeCompatible RGBA16F cubemap (`mips` x 6 faces) seeded with a
// constant HDR color across every mip+face, so it is a valid ShaderResource
// before a bake overwrites it (gap-free). `extraUsage` adds e.g. UnorderedAccess
// for the compute convolve target. Mirrors CreateFallbackCube's staging pattern.
bool CreateSeededCube(IDevice& device, const char* name, uint32_t size, uint32_t mips,
                      uint32_t extraUsage, const float color[4],
                      TextureHandle& texOut, TextureViewHandle& viewOut)
{
    // Idempotent: reuse on a partial-init retry so handles aren't overwritten + leaked.
    if (texOut.IsValid() || viewOut.IsValid())
        return true;

    TextureDesc td{};
    td.width = size;
    td.height = size;
    td.mipLevels = mips;
    td.arrayLayers = 6;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst) | extraUsage;
    td.flags = TextureCreateFlags::CubeCompatible;
    td.persistent = true;
    td.debugName = name;
    texOut = device.CreateTexture(td);
    if (!texOut.IsValid())
        return false;

    const uint16_t h[4] = {FloatToHalf(color[0]), FloatToHalf(color[1]),
                           FloatToHalf(color[2]), FloatToHalf(color[3])};

    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        texOut, ResourceState::Undefined, ResourceState::CopyDest, 0, mips, 0, 6));

    // Row pitch padded to the device's copy alignment (WebGPU requires 256 for
    // multi-row copies); the padding texels are never sampled.
    const uint32_t pitchAlign = std::max<uint32_t>(1u, device.GetCapabilities().textureCopyRowPitchAlignment);

    std::vector<BufferHandle> stagings;
    for (uint32_t mip = 0; mip < mips; ++mip)
    {
        const uint32_t ms = (size >> mip) > 0 ? (size >> mip) : 1;
        const uint32_t tightPitch = ms * 8; // RGBA16F = 8 bytes/texel
        const uint32_t rowPitch = ((tightPitch + pitchAlign - 1) / pitchAlign) * pitchAlign;
        const size_t faceBytes = static_cast<size_t>(rowPitch) * ms;
        std::vector<uint16_t> px(faceBytes / sizeof(uint16_t), 0);
        const size_t rowHalfs = rowPitch / sizeof(uint16_t);
        for (uint32_t y = 0; y < ms; ++y)
        {
            for (uint32_t x = 0; x < ms; ++x)
            {
                uint16_t* t = px.data() + y * rowHalfs + static_cast<size_t>(x) * 4;
                t[0] = h[0];
                t[1] = h[1];
                t[2] = h[2];
                t[3] = h[3];
            }
        }

        BufferDesc sb{};
        sb.size = faceBytes;
        sb.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        sb.memoryUsage = BufferMemoryUsage::Upload;
        sb.debugName = "IBL_SeedStaging";
        BufferHandle staging = device.CreateBuffer(sb);
        if (!staging.IsValid())
            return false;
        device.UpdateBuffer(staging, 0, faceBytes, px.data());
        for (uint32_t face = 0; face < 6; ++face)
            cl->CopyBufferToTextureSubresource(staging, texOut, mip, face, ms, ms, 0, rowPitch);
        stagings.push_back(staging);
    }

    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        texOut, ResourceState::CopyDest, ResourceState::ShaderResource, 0, mips, 0, 6));
    cl->End();
    device.ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    for (BufferHandle s : stagings)
        device.DestroyBuffer(s);

    TextureViewDesc vd{};
    vd.aspect = TextureAspect::Color;
    vd.viewType = TextureViewType::ViewCube;
    vd.baseMip = 0;
    vd.levelCount = 0; // all mips
    vd.baseLayer = 0;
    vd.layerCount = 6;
    vd.formatOverride = 0;
    vd.debugName = name;
    viewOut = device.CreateTextureView(texOut, vd);
    return viewOut != INVALID_TEXTURE_VIEW_HANDLE;
}

// Real-size split-sum BRDF integration LUT (UnorderedAccess so the one-shot
// brdf_lut.comp bake in IBLGenNode can write it). Seeded with the identity
// (scale=1, bias=0) so specular reconstructs as prefiltered * F0 and the
// binding is valid before the bake lands. `size` is the square edge length.
//
// Two channels are all the split-sum term needs, but rg16float is not a
// storage-capable format in WebGPU and the whole texture is refused at
// creation there — taking image-based lighting down with it, since a metal
// surface with no LUT and no environment resolves to black. Backends that
// cannot store to rg16float get rgba16float, which is on every storage list;
// the extra two channels are written and never read.
TextureHandle CreateBrdfLut(IDevice& device, uint32_t size)
{
    const bool rg16Storage = device.GetCapabilities().supportsRG16FloatStorage;
    const TextureFormat lutFormat =
        rg16Storage ? TextureFormat::R16G16_FLOAT : TextureFormat::R16G16B16A16_FLOAT;
    const uint32_t lutChannels = rg16Storage ? 2u : 4u;

    TextureDesc td{};
    td.width = size;
    td.height = size;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(lutFormat);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst) |
               static_cast<uint32_t>(TextureUsage::UnorderedAccess);
    td.persistent = true;
    td.debugName = "IBL_BrdfLut";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;

    // Row pitch padded to the device's copy alignment (WebGPU requires 256 for
    // multi-row copies); the padding texels are never sampled.
    const uint32_t lutPitchAlign =
        std::max<uint32_t>(1u, device.GetCapabilities().textureCopyRowPitchAlignment);
    const uint32_t lutTightPitch = size * lutChannels * 2u; // half per channel
    const uint32_t lutRowPitch = ((lutTightPitch + lutPitchAlign - 1) / lutPitchAlign) * lutPitchAlign;
    const size_t bytes = static_cast<size_t>(lutRowPitch) * size;
    std::vector<uint16_t> px(bytes / sizeof(uint16_t), 0);
    const size_t lutRowHalfs = lutRowPitch / sizeof(uint16_t);
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            uint16_t* t = px.data() + y * lutRowHalfs + static_cast<size_t>(x) * lutChannels;
            t[0] = kHalfOne;  // R = scale = 1
            t[1] = kHalfZero; // G = bias = 0
            if (lutChannels == 4u)
            {
                t[2] = kHalfZero;
                t[3] = kHalfOne;
            }
        }
    }

    BufferDesc sb{};
    sb.size = bytes;
    sb.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    sb.memoryUsage = BufferMemoryUsage::Upload;
    sb.debugName = "IBL_LutStaging";
    BufferHandle staging = device.CreateBuffer(sb);
    if (!staging.IsValid())
    {
        device.DestroyTexture(tex); // don't leak the texture we just created
        return TextureHandle{};
    }
    device.UpdateBuffer(staging, 0, bytes, px.data());

    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::Undefined, ResourceState::CopyDest, 0, 1, 0, 1));
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, size, size, 0, lutRowPitch);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource, 0, 1, 0, 1));
    cl->End();
    device.ExecuteCommandLists(std::vector<CommandList*>{cl.get()});
    device.DestroyBuffer(staging);
    return tex;
}

uint32_t FullMipCount(uint32_t size)
{
    uint32_t count = 1;
    while (size > 1)
    {
        size >>= 1u;
        ++count;
    }
    return count;
}

uint32_t PrefilterMipCountForSize(uint32_t size)
{
    const uint32_t fullChain = FullMipCount(size);
    return std::clamp(fullChain > 4u ? fullChain - 4u : 1u, 1u,
                      ImageBasedLightingFeature::kMaxPrefilterMipCount);
}

// std140 layout for the EnvData UBO (set 0, binding 21). Kept in lockstep with
// Shaders/Includes/ibl.glsl.
struct EnvDataGpu
{
    float iblIntensity = 1.0f;
    float prefilterMaxMip = 0.0f;
    float localBoxProjection = 0.0f;
    float lowerHemisphereDarkness = 0.0f;
    float localProbePositionWS[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float localBoxCenterWS[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float localBoxHalfExtentsWS[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float localBoxAxisXWS[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float localBoxAxisYWS[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float localBoxAxisZWS[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    // Ambient gradient tint (scene-linear RGB in .rgb, .a unused). White = identity;
    // multiplied into the diffuse irradiance only (see ibl.glsl GE_AmbientGradientTint).
    float ambientTintSky[4]     = {1.0f, 1.0f, 1.0f, 0.0f};
    float ambientTintEquator[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float ambientTintGround[4]  = {1.0f, 1.0f, 1.0f, 0.0f};
    // Additive authored ambient irradiance floor (AmbientLight component), scene-linear RGB in .rgb,
    // premultiplied by intensity/203. ambientFloorParams.x = mode (0 off / 1 flat / 2 gradient),
    // .y = affectSpecular. Default (mode 0, colors 0) => GE_AmbientFloor returns 0 => byte-identical.
    float ambientFloorSky[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
    float ambientFloorEquator[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ambientFloorGround[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
    float ambientFloorParams[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(EnvDataGpu) == ImageBasedLightingFeature::kEnvDataBytes,
              "EnvData UBO must match ibl.glsl std140 layout (and the pre-init fallback size)");
} // namespace

ImageBasedLightingFeature::ImageBasedLightingFeature() = default;

ImageBasedLightingFeature::~ImageBasedLightingFeature()
{
    if (!m_Device)
        return;
    DestroyCaptureResources();
    if (m_Set.diffuseEnvView != INVALID_TEXTURE_VIEW_HANDLE)
        m_Device->DestroyTextureView(m_Set.diffuseEnvView);
    if (m_Set.diffuseEnvTex.IsValid())
        m_Device->DestroyTexture(m_Set.diffuseEnvTex);
    if (m_Set.brdfLutTex.IsValid())
        m_Device->DestroyTexture(m_Set.brdfLutTex);
    if (m_CubeSampler.IsValid())
        m_Device->DestroySampler(m_CubeSampler);
    if (m_LutSampler.IsValid())
        m_Device->DestroySampler(m_LutSampler);
    for (auto& b : m_EnvDataBuffer)
        if (b.IsValid())
            m_Device->DestroyBuffer(b);

    if (m_IrradianceStoreView.IsValid())
        m_Device->DestroyTextureView(m_IrradianceStoreView);
}

const DescriptorSetLayoutDesc& ImageBasedLightingFeature::ConvolveSet0()
{
    static const DescriptorSetLayoutDesc d = []() {
        DescriptorSetLayoutDesc x{};
        x.bindings = {
            {0, DescriptorType::CombinedImageSampler, 1, kShaderStageCompute},
            {1, DescriptorType::StorageImage, 1, kShaderStageCompute},
        };
        // The shaders declare samplerCube (b0) and rgba16f image2DArray (b1).
        // WebGPU bakes view dimension and storage format into the layout and
        // rejects a cube view against the 2D default; Vulkan reads both off
        // the view and ignores these.
        x.bindings[0].imageDim = 4;                  // SPIR-V dim: cube
        x.bindings[0].imageFilterableFloat = true;   // linear-samples rgba16f
        x.bindings[1].imageDim = 2;
        x.bindings[1].imageArrayed = true;           // image2DArray
        x.bindings[1].storageTexelFormat =
            static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        x.debugName = "IBL_Convolve_Set0";
        return x;
    }();
    return d;
}

uint32_t ImageBasedLightingFeature::NormalizeCaptureResolution(uint32_t requested)
{
    if (requested == 0)
        requested = kDefaultEnvCaptureSize;
    requested = std::clamp(requested, kMinEnvCaptureSize, kMaxEnvCaptureSize);

    uint32_t lower = kMinEnvCaptureSize;
    while (lower <= (std::numeric_limits<uint32_t>::max() >> 1u) &&
           (lower << 1u) <= requested)
    {
        lower <<= 1u;
    }
    const uint32_t upper = std::min(kMaxEnvCaptureSize, lower << 1u);
    return (requested - lower <= upper - requested) ? lower : upper;
}

void ImageBasedLightingFeature::DestroyCaptureResources()
{
    if (!m_Device)
        return;

    if (m_EnvCaptureCubeView.IsValid())
    {
        m_Device->DestroyTextureView(m_EnvCaptureCubeView);
        m_EnvCaptureCubeView = {};
    }
    for (auto& v : m_EnvCaptureStoreViews)
    {
        if (v.IsValid())
            m_Device->DestroyTextureView(v);
        v = {};
    }
    for (auto& v : m_EnvCaptureMipSampleViews)
    {
        if (v.IsValid())
            m_Device->DestroyTextureView(v);
        v = {};
    }
    if (m_EnvCaptureTex.IsValid())
    {
        m_Device->DestroyTexture(m_EnvCaptureTex);
        m_EnvCaptureTex = {};
    }

    if (m_Set.specularEnvView != INVALID_TEXTURE_VIEW_HANDLE)
    {
        m_Device->DestroyTextureView(m_Set.specularEnvView);
        m_Set.specularEnvView = {};
    }
    for (auto& v : m_PrefilterStoreViews)
    {
        if (v.IsValid())
            m_Device->DestroyTextureView(v);
        v = {};
    }
    if (m_Set.specularEnvTex.IsValid())
    {
        m_Device->DestroyTexture(m_Set.specularEnvTex);
        m_Set.specularEnvTex = {};
    }
}

bool ImageBasedLightingFeature::CreateCaptureResources(uint32_t resolution)
{
    if (!m_Device)
        return false;

    const uint32_t size = NormalizeCaptureResolution(resolution);
    const uint32_t envMipCount = std::min(FullMipCount(size), kMaxEnvCaptureMipCount);
    const uint32_t prefilterMipCount = PrefilterMipCountForSize(size);

    const float kSeedAmbient[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    const uint32_t kStorageUsage = static_cast<uint32_t>(TextureUsage::UnorderedAccess);
    if (!CreateSeededCube(*m_Device, "IBL_Prefilter", size, prefilterMipCount,
                          kStorageUsage, kSeedAmbient, m_Set.specularEnvTex,
                          m_Set.specularEnvView))
    {
        Logger::Log::Error("ImageBasedLightingFeature: failed to create prefilter cube");
        return false;
    }

    const float kEnvSeed[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!CreateSeededCube(*m_Device, "IBL_EnvCapture", size, envMipCount,
                          static_cast<uint32_t>(TextureUsage::RenderTarget) |
                              static_cast<uint32_t>(TextureUsage::UnorderedAccess),
                          kEnvSeed, m_EnvCaptureTex, m_EnvCaptureCubeView))
    {
        Logger::Log::Error("ImageBasedLightingFeature: failed to create env capture cube");
        return false;
    }

    auto makeView = [&](TextureHandle tex, TextureViewType type,
                        uint32_t baseMip, const char* name) -> TextureViewHandle {
        TextureViewDesc vd{};
        vd.aspect = TextureAspect::Color;
        vd.viewType = type;
        vd.baseMip = baseMip;
        vd.levelCount = 1;
        vd.baseLayer = 0;
        vd.layerCount = 6;
        vd.formatOverride = 0;
        vd.debugName = name;
        return m_Device->CreateTextureView(tex, vd);
    };

    for (uint32_t mip = 0; mip < prefilterMipCount; ++mip)
    {
        m_PrefilterStoreViews[mip] =
            makeView(m_Set.specularEnvTex, TextureViewType::View2DArray, mip,
                     "IBL_PrefilterStore");
        if (!m_PrefilterStoreViews[mip].IsValid())
            return false;
    }
    for (uint32_t mip = prefilterMipCount; mip < kMaxPrefilterMipCount; ++mip)
        m_PrefilterStoreViews[mip] = {};

    for (uint32_t mip = 1; mip < envMipCount; ++mip)
    {
        m_EnvCaptureStoreViews[mip] =
            makeView(m_EnvCaptureTex, TextureViewType::View2DArray, mip,
                     "IBL_EnvCaptureStore");
        if (!m_EnvCaptureStoreViews[mip].IsValid())
            return false;
    }
    for (uint32_t mip = envMipCount; mip < kMaxEnvCaptureMipCount; ++mip)
        m_EnvCaptureStoreViews[mip] = {};

    // Per-mip sampled source views for the box-downsample (mip N-1 feeds mip N;
    // the last mip is never a source). See GetEnvCaptureMipSampleView for why
    // the downsample must not sample the whole-chain view.
    for (uint32_t mip = 0; mip + 1 < envMipCount; ++mip)
    {
        m_EnvCaptureMipSampleViews[mip] =
            makeView(m_EnvCaptureTex, TextureViewType::ViewCube, mip,
                     "IBL_EnvCaptureMipSample");
        if (!m_EnvCaptureMipSampleViews[mip].IsValid())
            return false;
    }
    for (uint32_t mip = envMipCount > 0 ? envMipCount - 1 : 0; mip < kMaxEnvCaptureMipCount; ++mip)
        m_EnvCaptureMipSampleViews[mip] = {};

    m_EnvCaptureSize = size;
    m_EnvCaptureMipCount = envMipCount;
    m_PrefilterSize = size;
    m_PrefilterMipCount = prefilterMipCount;
    return true;
}

bool ImageBasedLightingFeature::EnsureCaptureResolution(uint32_t requested)
{
    const uint32_t resolution = NormalizeCaptureResolution(requested);
    if (m_EnvCaptureTex.IsValid() &&
        m_Set.specularEnvTex.IsValid() &&
        m_EnvCaptureSize == resolution &&
        m_PrefilterSize == resolution)
    {
        return true;
    }

    DestroyCaptureResources();
    if (!CreateCaptureResources(resolution))
        return false;

    m_LastBakedDigest = 0;
    UploadEnvData(m_Device);
    return true;
}

bool ImageBasedLightingFeature::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
    {
        Logger::Log::Error("ImageBasedLightingFeature::Initialize: null device");
        return false;
    }
    m_Device = device;

    // A modest neutral ambient so an unbaked IBL scene reads like a mild ambient
    // probe rather than black. Stage 2 replaces these with real sky-derived cubes.
    // Real RGBA16F environment cubes, seeded with a neutral ambient so they read
    // as a mild ambient probe until a source bakes real sky data into them. Created
    // with UnorderedAccess so the Stage 2 convolve compute can write them in place.
    const float kSeedAmbient[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    const uint32_t kStorageUsage = static_cast<uint32_t>(TextureUsage::UnorderedAccess);
    if (!CreateSeededCube(*device, "IBL_Irradiance", 64, 1, kStorageUsage, kSeedAmbient,
                          m_Set.diffuseEnvTex, m_Set.diffuseEnvView))
    {
        Logger::Log::Error("ImageBasedLightingFeature: failed to create irradiance cube");
        return false;
    }

    // Bake resources: the sky capture cube (render-target written by the 6 face
    // passes, sampled by the convolves) + the storage views the convolve / per-mip
    // prefilter compute write into. A cube view can't be a storage image, so the
    // write targets are View2DArray (gl_GlobalInvocationID.z indexes the face).
    //
    // Seed it like the irradiance/prefilter cubes (CreateSeededCube also builds the
    // sampled ViewCube over ALL mips + adds TransferDst). It is `persistent`, so a raw
    // create would leave undefined VRAM in any mip/face a steady-sky one-shot bake
    // hasn't written yet; the convolves sample the whole cube, so seed every mip to
    // black to keep it always valid. Full mip chain: the capture renders mip 0, a
    // box-downsample compute fills the rest, and the convolve/prefilter read the
    // pre-blurred mips via textureLod. UnorderedAccess so the downsample can write
    // mips 1..N-1 in place; RenderTarget so the capture pass can attach mip 0.
    auto makeView = [&](TextureHandle tex, TextureViewType type,
                        uint32_t baseMip, const char* name) -> TextureViewHandle {
        TextureViewDesc vd{};
        vd.aspect = TextureAspect::Color;
        vd.viewType = type;
        vd.baseMip = baseMip;
        vd.levelCount = 1;
        vd.baseLayer = 0;
        vd.layerCount = 6;
        vd.formatOverride = 0;
        vd.debugName = name;
        return device->CreateTextureView(tex, vd);
    };

    if (!m_IrradianceStoreView.IsValid())
        m_IrradianceStoreView = makeView(m_Set.diffuseEnvTex, TextureViewType::View2DArray, 0, "IBL_IrradianceStore");
    if (!EnsureCaptureResolution(kDefaultEnvCaptureSize))
        return false;

    // The remaining creates are each guarded on existing validity so a retry
    // after a partial-init failure reuses what already exists instead of leaking.
    if (!m_Set.brdfLutTex.IsValid())
        m_Set.brdfLutTex = CreateBrdfLut(*device, kBrdfLutSize);
    if (!m_Set.brdfLutTex.IsValid())
    {
        Logger::Log::Error("ImageBasedLightingFeature: failed to create BRDF LUT");
        return false;
    }

    if (!m_CubeSampler.IsValid())
        m_CubeSampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("IBL_CubeSampler"));

    if (!m_LutSampler.IsValid())
    {
        SamplerDesc ls{};
        ls.minFilter = 1;     // Linear
        ls.magFilter = 1;     // Linear
        ls.mipFilter = 0;     // no trilinear (LUT has no mips)
        ls.addressModeU = 2;  // ClampToEdge
        ls.addressModeV = 2;
        ls.addressModeW = 2;
        ls.maxLod = 0.0f;
        ls.debugName = "IBL_LutSampler";
        m_LutSampler = device->CreateSampler(ls);
    }

    if (!m_CubeSampler.IsValid() || !m_LutSampler.IsValid())
    {
        Logger::Log::Error("ImageBasedLightingFeature: failed to create samplers");
        return false;
    }

    // EnvData UBO, ringed one element per device frame so the dynamic iblIntensity write
    // on frame N never lands on a buffer an in-flight earlier frame is still reading.
    for (auto& buf : m_EnvDataBuffer)
    {
        if (buf.IsValid())
            continue;
        BufferDesc bd{};
        bd.size = sizeof(EnvDataGpu);
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "IBL_EnvData";
        buf = device->CreateBuffer(bd);
        if (!buf.IsValid())
        {
            Logger::Log::Error("ImageBasedLightingFeature: failed to create EnvData UBO");
            return false;
        }
    }

    m_Initialized = true;
    UploadEnvData(device);
    Logger::Log::Info("ImageBasedLightingFeature initialized (seeded RGBA16F env cubes + BRDF LUT)");
    return true;
}

void ImageBasedLightingFeature::OnScheduleCulling(const FeatureCullingContext& ctx)
{
    if (m_Source)
        m_Source->ScheduleCulling(ctx);
}

void ImageBasedLightingFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!device)
        return;

    // Every IBL GPU resource was freed by the rebuild teardown, but the cached
    // handles still read IsValid(). Zero them WITHOUT destroy (that would
    // double-free a recycled slot), then re-run Initialize — each create is
    // IsValid-guarded, so this re-seeds the ambient-fallback cubes, BRDF LUT,
    // samplers, capture cube + storage views, and EnvData UBOs. The fallback keeps
    // the world-pass IBL binding valid on the first resumed frame (the "never black"
    // invariant); the real sky/probe cubes re-convolve afterward via the re-armed
    // bake below.
    m_Device = device;
    m_Set = IBLSet{};
    m_CubeSampler = {};
    m_LutSampler = {};
    for (auto& b : m_EnvDataBuffer)
        b = {};
    m_EnvCaptureTex = {};
    m_EnvCaptureCubeView = {};
    for (auto& v : m_EnvCaptureStoreViews)
        v = {};
    for (auto& v : m_EnvCaptureMipSampleViews)
        v = {};
    m_IrradianceStoreView = {};
    for (auto& v : m_PrefilterStoreViews)
        v = {};

    m_Initialized = false;
    Initialize(device);

    // Re-arm the content bake: force the IBL gen node to re-convolve the real cubes
    // (the source's InputDigest can no longer match the never-baked sentinel), rebake
    // the fresh BRDF LUT, and skip the first-rebake throttle.
    m_LastBakedDigest = 0;
    m_BrdfLutBaked    = false;
    m_FramesSinceBake = 0xFFFFFFFFu;
}

BufferHandle ImageBasedLightingFeature::UploadEnvData(IDevice* device)
{
    if (!device)
        return {};

    // The device's frame index is a CHANGE TOKEN here, never the element: it reports a
    // slot in [0, GetFramesInFlight()), a domain too narrow to name this ring's last
    // element, so reducing its VALUE would collapse the rotation onto the device's
    // pacing and strand that element unwritten and unbound. DeviceFrameCounter consumes
    // the token and steps once per device frame; it is idempotent within a frame, so the
    // gen node, each view's world pass, the ocean and the grass contributor all land on
    // the same element instead of rotating several times inside one frame.
    //
    // ORDERING. The write happens during graph declare/record, after BeginFrame waited
    // the fence for frame F - framesInFlight. Rotating one element per frame over R
    // elements, the write at frame F is the next touch of an element last read at F - R,
    // so the margin over the fenced frame is R - framesInFlight:
    //   R == framesInFlight (what reducing the wrapped index gives): margin ZERO — the
    //     write lands on the element the JUST-retired frame used, correct only while the
    //     fence-before-write ordering is exact and no consumer reads an earlier frame's
    //     element.
    //   R == framesInFlight + 1 (this ring on Vulkan: 4 against 3): the element's last
    //     reader retired a full frame before the write.
    const uint32_t slot = m_EnvDataFrameCounter.Tick(device->GetFrameIndex()) % kCaptureFrameSlots;
    BufferHandle buf = m_EnvDataBuffer[slot];
    if (!buf.IsValid())
        return {};
    EnvDataGpu d{};
    d.iblIntensity = m_IblIntensity;
    d.prefilterMaxMip = static_cast<float>(GetPrefilterMaxMip());

    // Ambient floor is independent of the environment source (it works with no sky at all), so it is
    // written unconditionally from the feature's own state. Default (off) keeps ambientFloorParams.x
    // at 0 -> GE_AmbientFloor returns exactly vec3(0) -> byte-identical output.
    d.ambientFloorParams[0] = static_cast<float>(m_AmbientFloor.Mode);
    d.ambientFloorParams[1] = static_cast<float>(m_AmbientFloor.AffectSpecular);
    for (int i = 0; i < 3; ++i)
    {
        d.ambientFloorSky[i]     = m_AmbientFloor.Sky[i];
        d.ambientFloorEquator[i] = m_AmbientFloor.Equator[i];
        d.ambientFloorGround[i]  = m_AmbientFloor.Ground[i];
    }

    if (m_Source && m_Source->HasActiveContent())
    {
        d.lowerHemisphereDarkness =
            std::clamp(m_Source->LowerHemisphereDarkness(), 0.0f, 1.0f);
        float tintSky[3], tintEquator[3], tintGround[3];
        m_Source->AmbientGradientTint(tintSky, tintEquator, tintGround);
        for (int i = 0; i < 3; ++i)
        {
            d.ambientTintSky[i]     = tintSky[i];
            d.ambientTintEquator[i] = tintEquator[i];
            d.ambientTintGround[i]  = tintGround[i];
        }
        const EnvironmentLocalReflectionData local = m_Source->LocalReflectionData();
        if (local.BoxProjection)
        {
            d.localBoxProjection = 1.0f;
            std::memcpy(d.localProbePositionWS, local.ProbePositionWS, sizeof(local.ProbePositionWS));
            std::memcpy(d.localBoxCenterWS, local.BoxCenterWS, sizeof(local.BoxCenterWS));
            std::memcpy(d.localBoxHalfExtentsWS, local.BoxHalfExtentsWS, sizeof(local.BoxHalfExtentsWS));
            std::memcpy(d.localBoxAxisXWS, local.BoxAxisXWS, sizeof(local.BoxAxisXWS));
            std::memcpy(d.localBoxAxisYWS, local.BoxAxisYWS, sizeof(local.BoxAxisYWS));
            std::memcpy(d.localBoxAxisZWS, local.BoxAxisZWS, sizeof(local.BoxAxisZWS));
        }
    }
    device->UpdateBuffer(buf, 0, sizeof(d), &d);
    return buf;
}

} // namespace Engine::Renderer
} // namespace GameEngine
