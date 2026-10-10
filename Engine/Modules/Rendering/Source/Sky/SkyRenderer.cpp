#include "Rendering/Sky/SkyRenderer.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"

#include "Logger/Logger.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#if defined(GE_HAVE_STB)
#include <stb_image.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

namespace
{
// Must match SkyRenderNode / sky_render.frag: starSizeRange.w < 0 selects procedural sky LUT sampling.
constexpr float kSkyHdriProceduralSentinel = -1.0f;

TextureHandle LoadSkyTexture2D(IDevice* device, const std::filesystem::path& path, TextureFormat format, const char* debugName)
{
    if (!device)
        return {};

#if defined(GE_HAVE_STB)
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(false);
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0)
    {
        if (pixels)
            stbi_image_free(pixels);
        return {};
    }

    TextureDesc td{};
    td.width = static_cast<uint32_t>(width);
    td.height = static_cast<uint32_t>(height);
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;
    TextureHandle texture = device->CreateTexture(td);
    if (texture.IsValid())
        UploadTexture2D(device, texture, pixels, td.width, td.height, td.width * 4u, debugName);
    stbi_image_free(pixels);
    return texture;
#else
    (void)path;
    (void)format;
    (void)debugName;
    return {};
#endif
}

// Resolve a set0 binding index from reflected shader meta by name. Only the
// index is trusted from reflection (the reflected type enum is unreliable for
// images); a miss falls back to the GLSL literal with a warning so the C++
// binds keep working while the divergence stays diagnosable.
uint32_t ResolveSet0Binding(const ShaderMeta& meta, const char* name, uint32_t fallback,
                            const char* pkgName)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set != 0)
            continue;
        for (const auto& b : s.Bindings)
            if (b.Name == name)
                return b.Binding;
        break;
    }
    Logger::Log::Warning("SkyRenderer: failed to resolve set0 binding '{}' in {} from reflection; "
                         "falling back to literal {}",
                         name, pkgName, fallback);
    return fallback;
}

TextureHandle CreateSolidSkyTexture2D(IDevice* device, uint32_t rgba, TextureFormat format, const char* debugName)
{
    if (!device)
        return {};

    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.format = static_cast<uint32_t>(format);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
             | static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = debugName;

    TextureHandle texture = device->CreateTexture(td);
    if (texture.IsValid())
        UploadTexture2D(device, texture, &rgba, td.width, td.height, sizeof(rgba), debugName);
    return texture;
}
} // namespace

SkyRenderer::SkyRenderer() = default;

SkyRenderer::~SkyRenderer()
{
    DestroyResources();
}

void SkyRenderer::DestroyResources()
{
    if (!m_Device)
        return;

    if (m_TransmittanceLutTex.IsValid())
        m_Device->DestroyTexture(m_TransmittanceLutTex);
    if (m_SkyViewLutTex.IsValid())
        m_Device->DestroyTexture(m_SkyViewLutTex);
    if (m_MultiscatterLutTex.IsValid())
        m_Device->DestroyTexture(m_MultiscatterLutTex);
    if (m_LinearSampler.IsValid())
        m_Device->DestroySampler(m_LinearSampler);
    if (m_SkyViewSampler.IsValid())
        m_Device->DestroySampler(m_SkyViewSampler);
    if (m_StarSSBO.IsValid())
        m_Device->DestroyBuffer(m_StarSSBO);
    if (m_MoonFullTexture.IsValid())
        m_Device->DestroyTexture(m_MoonFullTexture);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        if (m_SkyViewLutSkyUBO[i].IsValid())   m_Device->DestroyBuffer(m_SkyViewLutSkyUBO[i]);
        if (m_SkyViewLutAtmoUBO[i].IsValid())   m_Device->DestroyBuffer(m_SkyViewLutAtmoUBO[i]);
        if (m_MultiscatterAtmoUBO[i].IsValid()) m_Device->DestroyBuffer(m_MultiscatterAtmoUBO[i]);
        if (m_SkyRenderUBO[i].IsValid())         m_Device->DestroyBuffer(m_SkyRenderUBO[i]);
        if (m_TransmittanceAtmoUBO[i].IsValid()) m_Device->DestroyBuffer(m_TransmittanceAtmoUBO[i]);
        if (m_RenderAtmoUBO[i].IsValid())        m_Device->DestroyBuffer(m_RenderAtmoUBO[i]);
    }

    m_TransmittanceLutTex = {};
    m_SkyViewLutTex = {};
    m_MultiscatterLutTex = {};
    m_StarSSBO = {};
    m_MoonFullTexture = {};
    m_StarCount = 0;
    m_LinearSampler = {};
    m_SkyViewSampler = {};
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        m_SkyViewLutSkyUBO[i] = {};
        m_SkyViewLutAtmoUBO[i] = {};
        m_MultiscatterAtmoUBO[i] = {};
        m_SkyRenderUBO[i] = {};
        m_TransmittanceAtmoUBO[i] = {};
        m_RenderAtmoUBO[i] = {};
    }
}

void SkyRenderer::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;
    IDevice* device = m_Device;
    // Moon art is lazily loaded (EnsureMoonFullTexture guards on IsValid); zero the
    // dead handle so it reloads.
    m_MoonFullTexture = {};
    // Initialize resets m_TransmittanceComputed but not the other two one-shot LUT
    // flags — reset them so all three LUTs recompute into the fresh textures.
    m_SkyViewLutComputedOnce   = false;
    m_MultiscatterComputedOnce = false;
    // Recreates every LUT/sampler/UBO/star handle over the dead ones (unconditional
    // overwrite, no destroy -> no double-free) and re-interns the pipelines.
    Initialize(device, m_Config);
}

TextureHandle SkyRenderer::EnsureMoonFullTexture()
{
    if (m_MoonFullTexture.IsValid() || !m_Device)
        return m_MoonFullTexture;

    // With no root there is nothing to resolve against. Deliberately NOT falling back to
    // the bare relative path: that resolves against the process working directory, which is
    // the defect this replaced — it finds the repo on a developer's machine and the wrong
    // file (or nothing) anywhere else.
    // make_preferred so the path a human reads in the warning below is spelled in one
    // separator, not the '\' of the root joined to the '/' of the relative constant.
    std::filesystem::path path;
    if (!m_Config.installAssetsRoot.empty())
        path = (m_Config.installAssetsRoot / kSkyMoonFullTextureRelativePath).make_preferred();

    if (!path.empty())
        m_MoonFullTexture = LoadSkyTexture2D(m_Device, path, TextureFormat::RGBA8_SRGB, "SkyMoonFull");

    if (!m_MoonFullTexture.IsValid())
    {
        // The substitute keeps the sky render descriptor complete, and a transparent moon is
        // indistinguishable from a working night sky — so say what is missing, and name the
        // root too, so an unset root reads differently from a missing file. Runs once per
        // device: the fallback handle is itself valid, so the guard above stops the repeat.
        Logger::Log::Warning("SkyRenderer: moon texture not loaded (resolved '{}', install "
                             "assets root '{}') — the moon will not render. The build stages "
                             "it next to the executable; check the staged Assets tree.",
                             path.string(), m_Config.installAssetsRoot.string());
        m_MoonFullTexture = CreateSolidSkyTexture2D(m_Device, 0x00000000u, TextureFormat::RGBA8_SRGB, "SkyMoonFallbackTransparent");
    }
    return m_MoonFullTexture;
}

bool SkyRenderer::Initialize(IDevice* device, const Config& config)
{
    m_Device = device;
    m_Config = config;
    m_TransmittanceComputed = false;

    {
        TextureDesc d{};
        d.width = m_Config.transmittanceLutWidth;
        d.height = m_Config.transmittanceLutHeight;
        d.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_Transmittance_LUT";
        m_TransmittanceLutTex = m_Device->CreateTexture(d);
    }
    {
        TextureDesc d{};
        d.width = m_Config.skyViewLutWidth;
        d.height = m_Config.skyViewLutHeight;
        d.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_View_LUT";
        m_SkyViewLutTex = m_Device->CreateTexture(d);
    }
    {
        TextureDesc d{};
        d.width = m_Config.multiscatterLutSize;
        d.height = m_Config.multiscatterLutSize;
        d.format = (uint32_t)TextureFormat::R16G16B16A16_FLOAT;
        d.usage = (uint32_t)(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.debugName = "Sky_Multiscatter_LUT";
        m_MultiscatterLutTex = m_Device->CreateTexture(d);
    }

    {
        // Fully-clamped sampler for the transmittance / multiscatter / moon reads,
        // whose UVs are parametric (no wrap at the edges).
        SamplerDesc sd{};
        sd.minFilter = 1;    // Linear
        sd.magFilter = 1;    // Linear
        sd.addressModeU = 2; // ClampToEdge
        sd.addressModeV = 2; // ClampToEdge
        m_LinearSampler = m_Device->CreateSampler(sd);
    }
    {
        // Sky-view LUT reads only: u is the full-circle azimuth, so it must Repeat
        // to wrap seamlessly across u=0/1; v is the elevation, clamped at the poles.
        SamplerDesc sd{};
        sd.minFilter = 1;    // Linear
        sd.magFilter = 1;    // Linear
        sd.addressModeU = 0; // Repeat (lat-long azimuth wrap)
        sd.addressModeV = 2; // ClampToEdge (elevation poles)
        m_SkyViewSampler = m_Device->CreateSampler(sd);
    }

    m_FramesInFlight = m_Device->GetFramesInFlight();
    if (m_FramesInFlight > kMaxFramesInFlight)
        m_FramesInFlight = kMaxFramesInFlight;

    for (uint32_t i = 0; i < m_FramesInFlight; ++i)
    {
        auto makeUBO = [&](size_t size, const char* name) -> BufferHandle {
            BufferDesc bd{};
            bd.size = size;
            bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
            bd.memoryUsage = BufferMemoryUsage::Upload;
            // FrameSlotted: one element of a ring exactly as deep as the device paces.
            // SkyRenderNode fills it from its pass execute lambdas, which run behind
            // BeginFrame; a fill from anywhere earlier would race a frame in flight.
            bd.flags = BufferCreateFlags::PersistentlyMapped | BufferCreateFlags::FrameSlotted;
            bd.debugName = name;
            return m_Device->CreateBuffer(bd);
        };

        std::string suffix = ".Frame" + std::to_string(i);
        m_SkyViewLutSkyUBO[i]   = makeUBO(sizeof(SkyUBOGPU), ("Sky_LUT_SkyUBO" + suffix).c_str());
        m_SkyViewLutAtmoUBO[i]  = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_LUT_AtmoUBO" + suffix).c_str());
        m_MultiscatterAtmoUBO[i] = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_MS_AtmoUBO" + suffix).c_str());
        m_SkyRenderUBO[i]       = makeUBO(sizeof(SkyUBOGPU), ("Sky_Render_SkyUBO" + suffix).c_str());
        m_TransmittanceAtmoUBO[i] = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_Trans_AtmoUBO" + suffix).c_str());
        m_RenderAtmoUBO[i]      = makeUBO(sizeof(AtmosphereParametersGPU), ("Sky_Render_AtmoUBO" + suffix).c_str());
    }

    GenerateStarCatalog();

    if (!CreatePipelines())
    {
        DestroyResources();
        return false;
    }

    return true;
}

bool SkyRenderer::CreatePipelines()
{
    if (!m_Device)
        return false;
    const ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();

    // Transmittance Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_transmittance_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.Transmittance.TransLUT =
            ResolveSet0Binding(pkg.meta, "uTransLUT", 0, "sky_transmittance_lut");
        m_Bindings.Transmittance.Atmos =
            ResolveSet0Binding(pkg.meta, "uAtmos", 1, "sky_transmittance_lut");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.Transmittance.TransLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.Transmittance.Atmos, DescriptorType::UniformBuffer, 1, 0x20}
        // The LUT is rgba16f and the two LUT samplers filter rgba16f from
        // compute; WebGPU bakes both facts into the bind group layout and
        // rejects the pipeline if the defaults (RGBA8, unfilterable) stand.
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_TransmittancePipe.type = PipelineType::Compute;
        m_TransmittancePipe.computeShader = itCs->second;
        m_TransmittancePipe.descriptorSetLayouts.push_back(sl);
        m_TransmittancePipe.debugName = "Sky_Transmittance_Pipe";
    }

    // Multiscatter LUT Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_multiscatter_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.Multiscatter.MultiscatterLUT =
            ResolveSet0Binding(pkg.meta, "oMultiscatterLUT", 0, "sky_multiscatter_lut");
        m_Bindings.Multiscatter.TransmittanceLUT =
            ResolveSet0Binding(pkg.meta, "uTransmittanceLUT", 1, "sky_multiscatter_lut");
        m_Bindings.Multiscatter.Atmos =
            ResolveSet0Binding(pkg.meta, "uAtmos", 2, "sky_multiscatter_lut");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.Multiscatter.MultiscatterLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.Multiscatter.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, 0x20},
            {m_Bindings.Multiscatter.Atmos, DescriptorType::UniformBuffer, 1, 0x20}
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_MultiscatterPipe.type = PipelineType::Compute;
        m_MultiscatterPipe.computeShader = itCs->second;
        m_MultiscatterPipe.descriptorSetLayouts.push_back(sl);
        m_MultiscatterPipe.debugName = "Sky_Multiscatter_Pipe";
    }

    // Sky View LUT Compute
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_view_lut.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
            return false;

        m_Bindings.SkyViewLut.SkyViewLUT =
            ResolveSet0Binding(pkg.meta, "oSkyViewLUT", 0, "sky_view_lut");
        m_Bindings.SkyViewLut.TransmittanceLUT =
            ResolveSet0Binding(pkg.meta, "uTransmittanceLUT", 1, "sky_view_lut");
        m_Bindings.SkyViewLut.Atmos = ResolveSet0Binding(pkg.meta, "uAtmos", 2, "sky_view_lut");
        m_Bindings.SkyViewLut.Sky = ResolveSet0Binding(pkg.meta, "uSky", 3, "sky_view_lut");
        m_Bindings.SkyViewLut.MultiscatterLUT =
            ResolveSet0Binding(pkg.meta, "uMultiscatterLUT", 4, "sky_view_lut");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.SkyViewLut.SkyViewLUT, DescriptorType::StorageImage, 1, 0x20},
            {m_Bindings.SkyViewLut.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, 0x20},
            {m_Bindings.SkyViewLut.Atmos, DescriptorType::UniformBuffer, 1, 0x20},
            {m_Bindings.SkyViewLut.Sky, DescriptorType::UniformBuffer, 1, 0x20},
            {m_Bindings.SkyViewLut.MultiscatterLUT, DescriptorType::CombinedImageSampler, 1, 0x20}
        };
        for (auto& b : sl.bindings)
        {
            if (b.type == DescriptorType::StorageImage)
                b.storageTexelFormat = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            if (b.type == DescriptorType::CombinedImageSampler)
                b.imageFilterableFloat = true;
        }
        m_SkyViewLutPipe.type = PipelineType::Compute;
        m_SkyViewLutPipe.computeShader = itCs->second;
        m_SkyViewLutPipe.descriptorSetLayouts.push_back(sl);
        m_SkyViewLutPipe.debugName = "Sky_View_LUT_Pipe";
    }

    // Sky Render Graphics (Fullscreen Triangle)
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_render.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itVs = pkg.stageBytes.find("vs");
        auto itFs = pkg.stageBytes.find("fs");
        if (itVs == pkg.stageBytes.end() || itVs->second.empty() || itFs == pkg.stageBytes.end() || itFs->second.empty())
            return false;

        m_Bindings.SkyRender.Sky = ResolveSet0Binding(pkg.meta, "uSky", 0, "sky_render");
        m_Bindings.SkyRender.SkyViewLUT = ResolveSet0Binding(pkg.meta, "uSkyViewLUT", 1, "sky_render");
        m_Bindings.SkyRender.Atmos = ResolveSet0Binding(pkg.meta, "uAtmos", 2, "sky_render");
        m_Bindings.SkyRender.TransLUT = ResolveSet0Binding(pkg.meta, "uTransLUT", 3, "sky_render");
        m_Bindings.SkyRender.MoonFull =
            ResolveSet0Binding(pkg.meta, "uMoonFullTexture", 4, "sky_render");

        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.SkyRender.Sky, DescriptorType::UniformBuffer, 1, 0x10},
            {m_Bindings.SkyRender.SkyViewLUT, DescriptorType::CombinedImageSampler, 1, 0x10},
            {m_Bindings.SkyRender.Atmos, DescriptorType::UniformBuffer, 1, 0x10},
            {m_Bindings.SkyRender.TransLUT, DescriptorType::CombinedImageSampler, 1, 0x10},
            {m_Bindings.SkyRender.MoonFull, DescriptorType::CombinedImageSampler, 1, 0x10},
        };
        m_SkyRenderPipe.type = PipelineType::Graphics;
        m_SkyRenderPipe.vertexShader = itVs->second;
        m_SkyRenderPipe.pixelShader = itFs->second;
        m_SkyRenderPipe.descriptorSetLayouts.push_back(sl);
        m_SkyRenderPipe.debugName = "Sky_Render_Pipe";

        m_SkyRenderPipe.rasterizationSamples = 1;
        m_SkyRenderPipe.EnableDepthTest(false);
        m_SkyRenderPipe.SetCullingMode(CullModeFlagBits::None);
        m_SkyRenderPipe.AddDynamicState(DynamicState::Viewport);
        m_SkyRenderPipe.AddDynamicState(DynamicState::Scissor);
    }

    // Star Billboard Graphics
    {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg("Shaders/sky_stars_billboard.shaderpkg", sourceKind, pkg, &err))
            return false;
        auto itVs = pkg.stageBytes.find("vs");
        auto itFs = pkg.stageBytes.find("fs");
        if (itVs == pkg.stageBytes.end() || itVs->second.empty() ||
            itFs == pkg.stageBytes.end() || itFs->second.empty())
            return false;

        m_Bindings.StarBillboard.Stars = ResolveSet0Binding(pkg.meta, "sb", 0, "sky_stars_billboard");
        m_Bindings.StarBillboard.Sky = ResolveSet0Binding(pkg.meta, "uSky", 1, "sky_stars_billboard");

        constexpr uint32_t kVertFrag = kShaderStageVertex | kShaderStageFragment;
        DescriptorSetLayoutDesc sl{};
        sl.bindings = {
            {m_Bindings.StarBillboard.Stars, DescriptorType::StorageBuffer, 1, kVertFrag},
            {m_Bindings.StarBillboard.Sky, DescriptorType::UniformBuffer, 1, kVertFrag}
        };
        m_StarBillboardPipe.type = PipelineType::Graphics;
        m_StarBillboardPipe.vertexShader = itVs->second;
        m_StarBillboardPipe.pixelShader = itFs->second;
        m_StarBillboardPipe.descriptorSetLayouts.push_back(sl);
        m_StarBillboardPipe.debugName = "Sky_StarBillboard_Pipe";

        m_StarBillboardPipe.rasterizationSamples = 1;
        m_StarBillboardPipe.EnableDepthTest(false);
        m_StarBillboardPipe.SetCullingMode(CullModeFlagBits::None);
        m_StarBillboardPipe.EnableBlending(true, BlendFactor::One, BlendFactor::One);
        m_StarBillboardPipe.AddDynamicState(DynamicState::Viewport);
        m_StarBillboardPipe.AddDynamicState(DynamicState::Scissor);
    }

    return true;
}

void SkyRenderer::GenerateStarCatalog()
{
    const uint32_t maxStars = m_Config.maxStars;
    std::vector<StarInstanceGPU> stars;
    stars.reserve(maxStars);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kTwoPi = 2.0f * kPi;

    for (uint32_t i = 0; i < maxStars; ++i)
    {
        float u = dist01(rng);
        float v = dist01(rng);
        float theta = kTwoPi * u;
        float phi = std::acos(2.0f * v - 1.0f);

        float sinPhi = std::sin(phi);
        float x = sinPhi * std::cos(theta);
        float y = std::cos(phi);
        float z = sinPhi * std::sin(theta);

        float brightness = 0.3f + 0.7f * dist01(rng);
        float sizeScale = 0.6f + 1.2f * dist01(rng);
        float rotAngle = kTwoPi * dist01(rng);
        float twinkleSeed = dist01(rng);
        float colorTemp = dist01(rng);

        StarInstanceGPU star{};
        star.dirAndBrightness[0] = x;
        star.dirAndBrightness[1] = y;
        star.dirAndBrightness[2] = z;
        star.dirAndBrightness[3] = brightness;
        star.properties[0] = sizeScale;
        star.properties[1] = rotAngle;
        star.properties[2] = twinkleSeed;
        star.properties[3] = colorTemp;
        stars.push_back(star);
    }

    m_StarCount = static_cast<uint32_t>(stars.size());

    BufferDesc bd{};
    bd.size = m_StarCount * sizeof(StarInstanceGPU);
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    bd.memoryUsage = BufferMemoryUsage::Upload;
    bd.flags = BufferCreateFlags::PersistentlyMapped;
    bd.debugName = "Sky_StarCatalog_SSBO";
    m_StarSSBO = m_Device->CreateBuffer(bd);

    void* mapped = m_Device->MapBuffer(m_StarSSBO);
    if (mapped)
    {
        std::memcpy(mapped, stars.data(), m_StarCount * sizeof(StarInstanceGPU));
        m_Device->UnmapBuffer(m_StarSSBO);
    }
}

uint32_t SkyRenderer::GetStarInstanceCount(float density) const
{
    float t = std::clamp(density, 0.0f, 1.0f);
    float mapped = t * t;
    uint32_t count = static_cast<uint32_t>(mapped * static_cast<float>(m_StarCount));
    return std::clamp(count, 1u, m_StarCount);
}

void SkyRenderer::Update(const SkySettings& settings, const SkySystemState& state)
{
    m_Settings = settings;
    m_State = state;
}

void SkyRenderer::SetCamera(const CameraState& camera)
{
    m_Camera = camera;
}

void SkyRenderer::InvalidateTransmittance()
{
    m_TransmittanceComputed = false;
}

void SkyRenderer::FillDefaultAtmosphere(AtmosphereParametersGPU& atmo)
{
    atmo.planetRadius = 6360.0e3f;
    atmo.atmosphereRadius = 6460.0e3f;
    atmo.rayleighScaleHeight = 8000.0f;
    atmo.mieScaleHeight = 1200.0f;
    atmo.betaRayleigh[0] = 5.802e-6f;
    atmo.betaRayleigh[1] = 13.558e-6f;
    atmo.betaRayleigh[2] = 33.100e-6f;
    atmo.betaMie[0] = 2.0e-6f;
    atmo.betaMie[1] = 2.0e-6f;
    atmo.betaMie[2] = 2.0e-6f;
    atmo.mieG = 0.8f;
    atmo.groundAlbedo[0] = 0.3f;
    atmo.groundAlbedo[1] = 0.3f;
    atmo.groundAlbedo[2] = 0.3f;
    atmo.groundBrightness = 0.8f;
    atmo.groundNightColor[0] = 0.025f;
    atmo.groundNightColor[1] = 0.025f;
    atmo.groundNightColor[2] = 0.035f;
    atmo.groundNightColorStd140Pad = 0.0f;
    atmo.groundHorizonColor[0] = 0.04f;
    atmo.groundHorizonColor[1] = 0.05f;
    atmo.groundHorizonColor[2] = 0.07f;
    atmo.groundHorizonColorStd140Pad = 0.0f;
    // sRGB #020305 (scene-linear).
    atmo.groundHorizonNightColor[0] = 0.000607054f;
    atmo.groundHorizonNightColor[1] = 0.000910581f;
    atmo.groundHorizonNightColor[2] = 0.001517635f;
    atmo.nightSkyHorizonColor[0] = 0.12f;
    atmo.nightSkyHorizonColor[1] = 0.035f;
    atmo.nightSkyHorizonColor[2] = 0.19f;
    atmo.nightSkyHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonDayCosWidth = 0.035f;
    atmo.groundHorizonNightCosWidth = 0.035f;
    atmo.belowHorizonBlendSharpness = 2.0f;
    atmo.belowHorizonDarkness = 1.0f;
    atmo.belowHorizonDarkPadding[0] = 0.0f;
    atmo.belowHorizonDarkPadding[1] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[0] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[1] = 0.0f;
    atmo.belowHorizonDarkColorStd140PrePad[2] = 0.0f;
    atmo.belowHorizonDarkColor[0] = 0.02f;
    atmo.belowHorizonDarkColor[1] = 0.02f;
    atmo.belowHorizonDarkColor[2] = 0.025f;
    atmo.belowHorizonMode = static_cast<uint32_t>(SkyBelowHorizonMode::ContinueHorizon);
    atmo.groundHazeStrength = 1.0f;
}

uint32_t SkyRenderer::GetFrameSlot(IDevice* device) const
{
    return device->GetFrameIndex() % m_FramesInFlight;
}

ComputePipelineId SkyRenderer::EnsureTransmittanceId(IDevice& device)
{
    if (!m_TransmittanceId.IsValid())
    {
        m_TransmittancePipe.debugName = "Sky_Transmittance_Pipe";
        m_TransmittanceId = PipelineDescTranslator::InternCompute(device, m_TransmittancePipe);
    }
    return m_TransmittanceId;
}

ComputePipelineId SkyRenderer::EnsureMultiscatterId(IDevice& device)
{
    if (!m_MultiscatterId.IsValid())
    {
        m_MultiscatterPipe.debugName = "Sky_Multiscatter_Pipe";
        m_MultiscatterId = PipelineDescTranslator::InternCompute(device, m_MultiscatterPipe);
    }
    return m_MultiscatterId;
}

ComputePipelineId SkyRenderer::EnsureSkyViewLutId(IDevice& device)
{
    if (!m_SkyViewLutId.IsValid())
    {
        m_SkyViewLutPipe.debugName = "Sky_View_LUT_Pipe";
        m_SkyViewLutId = PipelineDescTranslator::InternCompute(device, m_SkyViewLutPipe);
    }
    return m_SkyViewLutId;
}

GraphicsPipelineId SkyRenderer::EnsureStarBillboardId(IDevice& device)
{
    if (!m_StarBillboardId.IsValid())
    {
        m_StarBillboardPipe.debugName = "Sky_StarBillboard_Pipe";
        m_StarBillboardId = PipelineDescTranslator::InternGraphics(device, m_StarBillboardPipe);
    }
    return m_StarBillboardId;
}

GraphicsPipelineId SkyRenderer::EnsureSkyRenderId(IDevice& device)
{
    if (!m_SkyRenderId.IsValid())
    {
        m_SkyRenderPipe.debugName = "Sky_Render_Pipe";
        m_SkyRenderId = PipelineDescTranslator::InternGraphics(device, m_SkyRenderPipe);
    }
    return m_SkyRenderId;
}

} // namespace Rendering
} // namespace GameEngine
