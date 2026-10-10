#include "UI/BuiltInViewOverlays.h"

#include "Editor/Assets/PendingAssetLoadsOverlay.h"
#include "Editor/Materials/ShaderCompileProgressOverlay.h"
#include "Editor/RenderPipeline/RenderPipelineStandInNotice.h"
#include "Scene/SceneLoadProgressOverlay.h"
#include "SceneView/SceneViewNoticeOverlay.h"
#include "Terrain/TerrainPageCookOverlay.h"
#include "UI/ViewOverlayHost.h"

#include <memory>

namespace GameEngine::Editor
{

void RegisterBuiltInViewOverlays(const EditorContext& context)
{
    ViewOverlayHost& host = ViewOverlayHost::Get();
    host.Register(std::make_unique<SceneLoadProgressOverlay>(context));
    host.Register(std::make_unique<PendingAssetLoadsOverlay>());
    host.Register(std::make_unique<TerrainPageCookOverlay>());
    host.Register(std::make_unique<ShaderCompileProgressOverlay>(context));
    host.Register(std::make_unique<RenderPipelineStandInNotice>(context));
    host.Register(std::make_unique<SceneViewNoticeOverlay>());
}

} // namespace GameEngine::Editor
