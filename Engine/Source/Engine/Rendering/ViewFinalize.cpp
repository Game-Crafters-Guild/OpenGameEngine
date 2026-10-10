#include "Engine/Rendering/ViewFinalize.h"

#include "Core/Engine.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TemporalDither.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
namespace
{

// RGGraph arena-copies resource and pass names at declaration, but the
// TextureDesc::debugName below is NOT copied: the frame's pending-transient
// list carries the desc until Execute realizes the texture, and the transient
// pool retains it beyond the frame — so the name must outlive the declaring
// scope. The set here is the host's fixed view names, so interning is bounded,
// and map nodes never move, so the returned pointer stays valid for the
// process.
const char* FinalizedNameFor(const char* viewName)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> interned;
    std::lock_guard<std::mutex> lock(mutex);
    return interned.try_emplace(viewName, std::string(viewName) + ".Finalized").first->second.c_str();
}

} // namespace

bool ViewFinalizeEligible(::GameEngine::Rendering::IDevice* device)
{
    return device != nullptr &&
           !::GameEngine::Rendering::IsHdrOutputModeActive(device->GetActiveHdrOutputMode());
}

float ResolveViewDebandThresholdLsb(RenderServices* services)
{
    if (!services)
        return ::GameEngine::Rendering::Passes::kOutputDebandBaselineThresholdLsb;
    uint64_t worldId = 0;
    if (::GameEngine::ECS::World* world = ::GameEngine::EngineCore::GetInstance().GetPrimaryWorld())
        worldId = world->GetWorldId();
    return services->GetWorldPostProcessSettings(worldId).DebandThresholdLsb;
}

ViewFinalizeResult DeclareViewFinalize(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                                       ::GameEngine::Rendering::RenderGraph::RGTexture viewColor,
                                       ::GameEngine::UI::UITextureSpace pipelineSpace,
                                       const char* viewName, float volumeDebandThresholdLsb,
                                       ::GameEngine::Rendering::TextureFormat presentedFormat,
                                       bool hostRefusal)
{
    const ViewFinalizeResult declined{viewColor, pipelineSpace};
    if (hostRefusal || !viewColor.IsValid() || !ViewFinalizeEligible(frame.Device()))
        return declined;

    const auto& srcDesc = frame.Graph().ResourceDesc(viewColor.Id);
    if (srcDesc.Width == 0 || srcDesc.Height == 0)
        return declined;

    // Same format as the view's own colour. F16 carries encoded, dithered [0,1]
    // values with a worst-case error of 2^-12 — a quarter of the 1/1023 step they
    // were dithered for — so the presented store rounds them to the same code it
    // would have reached directly, and the finalize needs no format change. That
    // also means a monitor move never has to recreate a view's target.
    ::GameEngine::Rendering::TextureDesc td{};
    td.width = srcDesc.Width;
    td.height = srcDesc.Height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = srcDesc.Format;
    // TransferSrc: the editor's ViewPresentationSnapshot copies the finalized
    // image so its panel can sample a stable frame while the next one renders.
    td.usage = static_cast<uint32_t>(::GameEngine::Rendering::TextureUsage::RenderTarget) |
               static_cast<uint32_t>(::GameEngine::Rendering::TextureUsage::ShaderResource) |
               static_cast<uint32_t>(::GameEngine::Rendering::TextureUsage::TransferSrc);
    const char* name = FinalizedNameFor(viewName);
    td.debugName = name;

    const ::GameEngine::Rendering::RenderGraph::RGTexture finalized = frame.CreateTexture(name, td);
    if (!finalized.IsValid())
        return declined;

    // Presented, not Destination: this writes an F16 intermediate the UI still
    // composites onto, so its filters are sized to the swapchain step the bytes
    // will actually land on (1/255 at 8 bits, 1/1023 at 10) rather than to a
    // float target's absent one. The step's format is the caller's, resolved
    // from its own window-target handle, and it is always forwarded ENGAGED —
    // the pass's device fallback is unreachable from this policy, so the step
    // can never ride whichever window target happens to be active.
    //
    // Named once and both handed to the pass and returned: a gate that asserts
    // what this policy decided must read the same value the pass was given, or
    // it is comparing a literal against a copy of itself.
    const ViewFinalizeStep step{::GameEngine::Rendering::Passes::FinalizeInputSpace::Linear,
                                ::GameEngine::Rendering::Passes::FinalizeQuantizer::Presented,
                                presentedFormat};
    const ::GameEngine::Rendering::RenderGraph::RGPass pass =
        ::GameEngine::Rendering::Passes::AddSRGBEncodePassRG(
            frame, viewColor, finalized,
            {.InputSpace = step.InputSpace,
             .Quantizer = step.Quantizer,
             .VolumeDebandThresholdLsb = volumeDebandThresholdLsb,
             .PresentedFormat = step.PresentedFormat,
             // The view's own dither is the one the viewer looks at, so the
             // experimental screen phase belongs here and not only at the
             // terminal — on an SDR frame the terminal filters nothing. 0
             // unless the toggle is on, which is every shipped path.
             .DitherPhase =
                 ::GameEngine::Rendering::Passes::ScreenDitherPhase(frame.FrameIndex())},
            name);
    if (!pass.IsValid())
    {
        // Unfinalized and linear — the UI's encoded target applies the OETF at
        // sample time, exactly as it did before this pass existed, so the view is
        // correct and merely unfiltered. Retries next frame.
        static bool s_WarnedUndeclared = false;
        if (!s_WarnedUndeclared)
        {
            s_WarnedUndeclared = true;
            Logger::Log::Warning("View finalize '{}': encode pass not declared (shaders staged?) — "
                                 "the view composites unfinalized until it loads",
                                 viewName);
        }
        return declined;
    }

    return {finalized, ::GameEngine::UI::UITextureSpace::SdrFinalized(), step};
}

} // namespace GameEngine::Engine::Renderer
