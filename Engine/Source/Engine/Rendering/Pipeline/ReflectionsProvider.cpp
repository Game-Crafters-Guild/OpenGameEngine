#include "Engine/Rendering/Pipeline/ReflectionsProvider.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Logger/Logger.h"

namespace GameEngine::Engine::Renderer::Pipeline
{

namespace
{
// Forward MRT locations the SSSR adapter variant writes (0 is the world color).
constexpr uint32_t kNormalRoughnessAttachment = 1;
constexpr uint32_t kSpecularWeightAttachment = 2;
constexpr uint32_t kSpecularRadianceAttachment = 3;
// The slices below are created single-sample, so this is the only world-colour
// sample count they can attach alongside.
constexpr uint32_t kSliceSampleCount = 1;
} // namespace

Rendering::MaterialKeyword ReflectionsProvider::WorldPassKeyword(ViewDeclare& d)
{
    if (!d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId).IsSSSRActive())
        return Rendering::MaterialKeyword::None;

    const auto colorTarget = d.ViewColor.IsValid() ? d.ViewColor : d.ViewResolve;
    if (!colorTarget.IsValid())
        return Rendering::MaterialKeyword::None;

    // Bailing here withholds the keyword from every contributor AND, in
    // ContributeWorldPassTargets, the attachments and the *Written publishes —
    // the SSR node consumes the G-buffer only through those names, so this
    // positively disables it for the view.
    const uint32_t colorSamples = d.Frame.Graph().ResourceDesc(colorTarget.Id).SampleCount;
    if (colorSamples != kSliceSampleCount)
    {
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            warnedOnce = true;
            LOG_WARNING("SSSR: view {} renders the world color at {}x MSAA but the SSR G-buffer "
                        "is single-sample — SSSR is disabled for this view. Disable MSAA or "
                        "disable SSSR to resolve.",
                        static_cast<uint32_t>(d.View.id), colorSamples);
        }
        return Rendering::MaterialKeyword::None;
    }
    return Rendering::MaterialKeyword::SSSRNormalRoughness;
}

Rendering::MaterialKeyword ReflectionsProvider::ContributeWorldPassTargets(
    ViewDeclare& d, RenderServices::WorldPassTargetsRG& targets,
    Rendering::RenderGraph::RGTexture normalTarget)
{
    // Resolve before the inactive/MSAA bails so a blueprint-declared G-buffer
    // still materializes as a never-written texture. The node consumes only
    // *.Written; the raw names existing is what makes that signal necessary.
    const auto blueprintNormalRoughness = d.ResolveTexture(Names::View::NormalRoughness);
    auto specularWeight = d.ResolveTexture(Names::View::SSRSpecularWeight);
    auto specularRadiance = d.ResolveTexture(Names::View::SSRSpecularRadiance);

    const auto settings =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    if (!settings.IsSSSRActive() && !normalTarget.IsValid())
        return Rendering::MaterialKeyword::None;

    // A consumer-owned normal target wins over the blueprint's: the SSR node,
    // when active, reads whichever texture the *Written publish names, so both
    // consumers see the same write.
    auto normalRoughness = normalTarget.IsValid() ? normalTarget : blueprintNormalRoughness;

    // The SSR G-buffer slices are engine internals: create them here when the
    // blueprint doesn't declare them (older project rendergraphs), so projects
    // never break when the engine grows a target. Published so the SSR node
    // and later frames resolve the same textures. The specular slices always
    // ride along: the adapter's variant writes locations 1, 2 AND 3.
    if (d.RenderWidth > 0 && d.RenderHeight > 0)
    {
        Rendering::TextureDesc gb{};
        gb.width = d.RenderWidth;
        gb.height = d.RenderHeight;
        gb.mipLevels = 1;
        gb.arrayLayers = 1;
        gb.sampleCount = 1;
        gb.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget |
                                         Rendering::TextureUsage::ShaderResource);
        if (!normalRoughness.IsValid())
        {
            gb.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            gb.debugName = "SSSR.NormalRoughness";
            normalRoughness =
                d.Frame.CreateTexture(d.PassName("SSSR.NormalRoughness").c_str(), gb);
            d.PublishTexture(Names::View::NormalRoughness, normalRoughness);
        }
        if (!specularWeight.IsValid())
        {
            gb.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            gb.debugName = "SSSR.SpecularWeight";
            specularWeight = d.Frame.CreateTexture(d.PassName("SSSR.SpecularWeight").c_str(), gb);
            d.PublishTexture(Names::View::SSRSpecularWeight, specularWeight);
        }
        if (!specularRadiance.IsValid())
        {
            gb.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
            gb.debugName = "SSSR.SpecularRadiance";
            specularRadiance = d.Frame.CreateTexture(d.PassName("SSSR.SpecularRadiance").c_str(), gb);
            d.PublishTexture(Names::View::SSRSpecularRadiance, specularRadiance);
        }
    }
    if (!normalRoughness.IsValid() || !specularWeight.IsValid() || !specularRadiance.IsValid())
        return Rendering::MaterialKeyword::None;

    const auto colorTarget = targets.Color.IsValid() ? targets.Color : targets.Resolve;
    if (!colorTarget.IsValid())
        return Rendering::MaterialKeyword::None;

    // The slices are single-sample. A multisampled world pass can still feed a
    // consumer-owned normal target: the pass renders into a transient slice at
    // its own sample count and resolves it into the target (Store DontCare —
    // the multisampled data never leaves tile memory). SSSR keeps its
    // single-sample contract: with MSAA the *.Written publishes are withheld
    // and the SSR node stays disabled for the view, as before.
    const uint32_t colorSamples = d.Frame.Graph().ResourceDesc(colorTarget.Id).SampleCount;
    const uint32_t sliceSamples = d.Frame.Graph().ResourceDesc(normalRoughness.Id).SampleCount;
    Rendering::RenderGraph::RGTexture normalResolve{};
    if (colorSamples != sliceSamples)
    {
        if (!normalTarget.IsValid() || d.RenderWidth == 0 || d.RenderHeight == 0)
        {
            static bool warnedOnce = false;
            if (!warnedOnce)
            {
                warnedOnce = true;
                LOG_WARNING("SSSR: view {} renders the world color at {}x MSAA but the SSR "
                            "G-buffer is {}x — SSSR is disabled for this view. Disable MSAA or "
                            "disable SSSR to resolve.",
                            static_cast<uint32_t>(d.View.id), colorSamples, sliceSamples);
            }
            return Rendering::MaterialKeyword::None;
        }
        Rendering::TextureDesc ms{};
        ms.width = d.RenderWidth;
        ms.height = d.RenderHeight;
        ms.mipLevels = 1;
        ms.arrayLayers = 1;
        ms.sampleCount = colorSamples;
        ms.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        ms.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        ms.debugName = "SSSR.NormalRoughness.MS";
        normalResolve = normalTarget;
        normalRoughness = d.Frame.CreateTexture(d.PassName("SSSR.NormalRoughness.MS").c_str(), ms);
        ms.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        ms.debugName = "SSSR.SpecularWeight.MS";
        specularWeight = d.Frame.CreateTexture(d.PassName("SSSR.SpecularWeight.MS").c_str(), ms);
        ms.debugName = "SSSR.SpecularRadiance.MS";
        specularRadiance = d.Frame.CreateTexture(d.PassName("SSSR.SpecularRadiance.MS").c_str(), ms);
        if (!normalRoughness.IsValid() || !specularWeight.IsValid() || !specularRadiance.IsValid())
            return Rendering::MaterialKeyword::None;
    }
    const bool multisampled = normalResolve.IsValid();

    Rendering::RenderGraph::RGAttachmentOps nr{};
    nr.Load = Rendering::RenderGraph::RGLoadOp::Clear;
    nr.Store = multisampled ? Rendering::RenderGraph::RGStoreOp::DontCare
                            : Rendering::RenderGraph::RGStoreOp::Store;
    // .rg = oct(+Z view normal) = (0.5,0.5); .b = roughness 1 (SSR classify
    // skips it); .a = metallic 0. Sky/untouched pixels stay out of SSR by
    // roughness alone.
    nr.Clear.Color[0] = 0.5f;
    nr.Clear.Color[1] = 0.5f;
    nr.Clear.Color[2] = 1.0f;
    nr.Clear.Color[3] = 0.0f;
    targets.AddExtraColor(kNormalRoughnessAttachment, normalRoughness, nr, normalResolve);

    // Specular MRTs ride the NormalRoughness gate. Their stores are needed
    // only while SSR runs; a DDGI-only normal consumer discards them.
    Rendering::RenderGraph::RGAttachmentOps al{};
    al.Load = Rendering::RenderGraph::RGLoadOp::Clear;
    al.Store = (settings.IsSSSRActive() && !multisampled)
                   ? Rendering::RenderGraph::RGStoreOp::Store
                   : Rendering::RenderGraph::RGStoreOp::DontCare;
    al.Clear.Color[3] = 0.0f;
    targets.AddExtraColor(kSpecularWeightAttachment, specularWeight, al);
    targets.AddExtraColor(kSpecularRadianceAttachment, specularRadiance, al);

    // Positive signal for the SSR node: published exactly when the
    // single-sample slices are attached (cleared + written) this frame.
    // Mirrors the attach conditions above so the signal can never outrun the
    // data.
    if (!multisampled)
    {
        d.PublishTexture(Names::View::NormalRoughnessWritten, normalRoughness);
        d.PublishTexture(Names::View::SSRSpecularWeightWritten, specularWeight);
        d.PublishTexture(Names::View::SSRSpecularRadianceWritten, specularRadiance);
    }
    return Rendering::MaterialKeyword::SSSRNormalRoughness;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
