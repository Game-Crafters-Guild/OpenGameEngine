#include "SceneView/SceneViewEntityLinks.h"

#include "SceneViewController.h"
#include "UI/Controls/EntityLink.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{

void SelectFromLink(const std::function<SceneViewController*()>& sceneView, ECS::EntityHandle entity, bool frame)
{
    SceneViewController* view = sceneView();
    if (!view)
        return;
    view->OnEntityPicked(entity, false, false, true);
    if (frame)
        view->FrameOrigin();
}

void HoverFromLink(const std::function<SceneViewController*()>& sceneView, ECS::EntityHandle entity)
{
    if (SceneViewController* view = sceneView())
        view->SetHoverEntity(entity, false, SceneViewController::HoverEntitySource::Panel);
}

} // namespace

void InstallSceneViewEntityLinks(std::function<SceneViewController*()> sceneView)
{
    EditorUI::EntityLinkActions actions;
    actions.Select = [sceneView](ECS::EntityHandle entity, bool frame) { SelectFromLink(sceneView, entity, frame); };
    actions.Hover = [sceneView](ECS::EntityHandle entity) { HoverFromLink(sceneView, entity); };
    EditorUI::SetEditorEntityLinkActions(std::move(actions));
}

void UninstallSceneViewEntityLinks()
{
    EditorUI::SetEditorEntityLinkActions({});
}

} // namespace GameEngine::Editor
