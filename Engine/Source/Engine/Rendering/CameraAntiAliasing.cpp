#include "Engine/Rendering/CameraAntiAliasing.h"

#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"

namespace GameEngine::Engine::Renderer
{

ResolvedAntiAliasing ApplyCameraAntiAliasing(RenderServices& rs,
                                             ::GameEngine::Rendering::ViewId viewId,
                                             const Camera& camera,
                                             const CameraAspectResolution& aspect)
{
    const ResolvedAntiAliasing aa =
        rs.ResolveAntiAliasing(camera.params.AntiAliasing, camera.params.MSAASamples);

    // MSAA resolves to a multisampled target instead of per-view AA state, so
    // a sample count above 1 rules the state out on its own.
    const bool registerViewState = UsesPerViewAntiAliasingState(aa.Mode) &&
                                   aa.SampleCount == 1u && !aspect.letterbox.active;
    rs.Views().SetViewAntiAliasing(viewId, registerViewState, aa.Mode, rs.GetTaaSequenceLength());
    return aa;
}

} // namespace GameEngine::Engine::Renderer
