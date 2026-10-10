#include "Engine/Rendering/SunGlareRenderFeature.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <string>

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kVertexStage = kShaderStageVertex;
constexpr uint32_t kFragmentStage = kShaderStageFragment;
} // namespace

bool SunGlareRenderFeature::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device || m_InitializeFailed)
        return false;
    m_Device = device;

    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg("Shaders/sun_glare.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("SunGlareRenderFeature: failed to load shaderpkg: {}", loadErr);
        m_InitializeFailed = true;
        return false;
    }
    auto vs = pkg.stageBytes.find("vs");
    auto fsIt = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fsIt == pkg.stageBytes.end() || vs->second.empty() ||
        fsIt->second.empty())
    {
        Logger::Log::Warning("SunGlareRenderFeature: shaderpkg missing vs/fs stage bytes.");
        m_InitializeFailed = true;
        return false;
    }

    // The multisampled variant: the same fragment stage with a sampler2DMS depth probe in the
    // vertex stage. Its absence is a warning rather than a failure — the single-sample
    // pipeline covers every MSAA-off view, and the node leaves the pass undeclared on an MSAA
    // view rather than binding a multisampled image to a sampler2D, which is undefined.
    ShaderPackage pkgMS{};
    std::string loadErrMS;
    const std::vector<uint8_t>* vsMS = nullptr;
    if (LoadShaderPkg("Shaders/sun_glare_ms.shaderpkg", device->PreferredShaderSource(), pkgMS, &loadErrMS))
    {
        const auto it = pkgMS.stageBytes.find("vs");
        if (it != pkgMS.stageBytes.end() && !it->second.empty())
            vsMS = &it->second;
    }
    if (!vsMS)
    {
        Logger::Log::Warning("SunGlareRenderFeature: multisampled variant unavailable ({}); the "
                             "glare will be skipped on MSAA views.",
                             loadErrMS.empty() ? "missing vs stage bytes" : loadErrMS);
    }

    // Resolve each set0 binding index from the shader's reflected meta by name so a layout
    // edit in sun_glare.vert/frag stays in lockstep with the C++ binds. Only the INDEX is
    // taken from reflection; the descriptor type stays explicit. The UBO reflects under its
    // INSTANCE name, "ViewParams", not the block name.
    auto resolveBinding = [&pkg](const char* name, uint32_t fallback) -> uint32_t {
        for (const auto& setMeta : pkg.meta.Sets)
        {
            if (setMeta.Set != 0)
                continue;
            for (const auto& b : setMeta.Bindings)
                if (b.Name == name)
                    return b.Binding;
            break;
        }
        Logger::Log::Warning("SunGlareRenderFeature: failed to resolve set0 binding '{}' from "
                             "reflection; falling back to literal {}",
                             name, fallback);
        return fallback;
    };
    m_TransmittanceBinding = resolveBinding("uTransLUT", 0u);
    m_DepthBinding = resolveBinding("uSceneDepth", 1u);
    m_ViewParamsBinding = resolveBinding("ViewParams", 2u);
    m_ShadowArrayBinding = resolveBinding("uShadowMapArray", 3u);
    m_ShadowDataBinding = resolveBinding("ShadowData", 4u);

    // The layouts come from each package's reflection so the image shapes
    // (depth array, multisampled probe, filtered LUT) match what the shader
    // declares; WebGPU bakes those into the bind group layout.
    const auto layoutFromMeta = [](const ShaderPackage& package, const char* name) {
        DescriptorSetLayoutDesc layout{};
        for (const auto& setMeta : package.meta.Sets)
        {
            if (setMeta.Set != 0)
                continue;
            layout = MaterialBuilder::BuildSetLayout(setMeta);
            break;
        }
        // The meta strings die with the package; the layout outlives it.
        for (DescriptorBinding& b : layout.bindings)
            b.debugName = nullptr;
        layout.debugName = name;
        return layout;
    };
    m_Layout = layoutFromMeta(pkg, "SunGlare.Set0");

    m_Pipeline = {};
    m_Pipeline.type = PipelineType::Graphics;
    m_Pipeline.vertexShader = vs->second;
    m_Pipeline.pixelShader = fsIt->second;
    m_Pipeline.rasterizationSamples = 1;
    m_Pipeline.EnableDepthTest(false);
    m_Pipeline.SetCullingMode(CullModeFlagBits::None);
    m_Pipeline.pushConstantSize = sizeof(SunGlarePushConstants);
    m_Pipeline.pushConstantStagesMask = kVertexStage | kFragmentStage;
    // Veiling glare is light ADDED to the image by the optics, so the target's own contents
    // survive underneath it. Alpha is left alone (source alpha is 0).
    m_Pipeline.EnableBlending(true, BlendFactor::One, BlendFactor::One);
    auto& blend = m_Pipeline.colorBlendState.attachments[0];
    blend.srcAlphaBlendFactor = BlendFactor::Zero;
    blend.dstAlphaBlendFactor = BlendFactor::One;
    m_Pipeline.AddDynamicState(DynamicState::Viewport);
    m_Pipeline.AddDynamicState(DynamicState::Scissor);
    m_Pipeline.descriptorSetLayouts.push_back(m_Layout);
    m_Pipeline.debugName = "SunGlare";

    m_PipelineId = PipelineDescTranslator::InternGraphics(*device, m_Pipeline);
    if (!m_PipelineId.IsValid())
    {
        Logger::Log::Warning("SunGlareRenderFeature: failed to intern pipeline.");
        m_InitializeFailed = true;
        return false;
    }

    if (vsMS)
    {
        m_LayoutMS = layoutFromMeta(pkgMS, "SunGlare.Set0.MS");
        PipelineDesc msDesc = m_Pipeline;
        msDesc.vertexShader = *vsMS;
        msDesc.descriptorSetLayouts = {m_LayoutMS};
        msDesc.debugName = "SunGlare.MS";
        m_PipelineIdMS = PipelineDescTranslator::InternGraphics(*device, msDesc);
        if (!m_PipelineIdMS.IsValid())
            Logger::Log::Warning("SunGlareRenderFeature: failed to intern the multisampled "
                                 "pipeline; the glare will be skipped on MSAA views.");
    }

    // Linear CLAMP, and the clamp is the load-bearing half: the transmittance LUT's u axis
    // is the ray's zenith cosine, so a repeating address mode would wrap a sun near the
    // horizon onto the opposite horizon's optical depth. The depth probe ignores the filter
    // entirely (texelFetch).
    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("SunGlare_Sampler"));
    if (!m_Sampler.IsValid())
    {
        m_InitializeFailed = true;
        return false;
    }

    // The cascades are sampled through a COMPARISON sampler: sampler2DArrayShadow returns the
    // depth-test result, and the hardware's 2x2 bilinear blend of four binary compares is what
    // makes the visibility continuous rather than a per-texel step.
    m_ShadowSampler = device->CreateSampler(SamplerDesc::ShadowComparePCF("SunGlare_ShadowPCF"));
    if (!m_ShadowSampler.IsValid())
    {
        m_InitializeFailed = true;
        return false;
    }

    m_Initialized = true;
    return true;
}

void SunGlareRenderFeature::SetSkylines(const ViewSkyline* entries, size_t count)
{
    m_Skyline.assign(entries, entries + count);
}

float SunGlareRenderFeature::GetSkylineTangent(uint32_t viewId) const
{
    for (const ViewSkyline& e : m_Skyline)
        if (e.ViewId == viewId)
            return e.Tangent;
    return kNoSkyline;
}

void SunGlareRenderFeature::OnDeviceRebuilt(IDevice* device)
{
    if (!device || !m_Initialized)
        return;
    m_Device = device;
    // The sampler is created eagerly in Initialize, whose m_Initialized guard means it will
    // not recreate it; do so here. The interned pipeline self-heals via warm recompile.
    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("SunGlare_Sampler"));
    m_ShadowSampler = device->CreateSampler(SamplerDesc::ShadowComparePCF("SunGlare_ShadowPCF"));
}

} // namespace GameEngine::Engine::Renderer
