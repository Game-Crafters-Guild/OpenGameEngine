#include "SceneView/GizmoSettingsWiring.h"

#include "Panels/SettingsPanel.h"
#include "SceneView/TransformTool.h"
#include "SceneViewController.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine
{
namespace Editor
{
namespace SceneTools
{

void GizmoSettingsWiring::WireGizmoSettings(::GameEngine::SettingsPanel* settingsPanel,
                                            ::GameEngine::SceneViewController* sceneController)
{
    if (!settingsPanel || !sceneController)
    {
        return;
    }

    // Thickness callbacks (for non-constant screen size mode)
    settingsPanel->SetOnTranslateGizmoThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetTranslateGizmoThickness(thickness);
    });

    settingsPanel->SetOnRotateGizmoThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetRotateGizmoThickness(thickness);
    });

    settingsPanel->SetOnScaleGizmoThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetScaleGizmoThickness(thickness);
    });

    // Constant screen size toggle
    settingsPanel->SetOnGizmoConstantSizeChanged([sceneController](bool enabled)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetConstantScreenSize(enabled);
    });

    // Enhanced rotate gizmo toggle (culling + screen-space ring + edge-on map).
    settingsPanel->SetOnRotateGizmoEnhancedChanged([sceneController](bool enabled)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetRotateGizmoEnhanced(enabled);
    });

    // Gizmo scale callbacks (for constant screen size mode)
    settingsPanel->SetOnTranslateGizmoScaleChanged([sceneController](float scale)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetTranslateGizmoScale(scale);
    });

    settingsPanel->SetOnRotateGizmoScaleChanged([sceneController](float scale)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetRotateGizmoScale(scale);
    });

    settingsPanel->SetOnScaleGizmoScaleChanged([sceneController](float scale)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetScaleGizmoScale(scale);
    });

    // Constant size thickness callbacks
    settingsPanel->SetOnTranslateConstantThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetTranslateConstantThickness(thickness);
    });

    settingsPanel->SetOnRotateConstantThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetRotateConstantThickness(thickness);
    });

    settingsPanel->SetOnScaleConstantThicknessChanged([sceneController](float thickness)
    {
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetScaleConstantThickness(thickness);
    });

    // Grid settings
    settingsPanel->SetOnGridOpacityChanged([sceneController](float opacity)
    {
        sceneController->SetGridOpacity(opacity);
    });

    settingsPanel->SetOnGridSnapSizeChanged([sceneController](float size)
    {
        sceneController->SetGridSnapSize(size);
    });

    // Apply saved grid + gizmo preferences at startup.
    {
        auto prefs = ::GameEngine::Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double opacity = 0.3;
        double snapSize = 0.5;
        prefs.TryGetDouble("grid.opacity", opacity);
        prefs.TryGetDouble("grid.snapSize", snapSize);
        sceneController->SetGridOpacity(static_cast<float>(opacity));
        sceneController->SetGridSnapSize(static_cast<float>(snapSize));

        bool rotateEnhanced = false;
        prefs.TryGetBool("gizmo.rotateEnhanced", rotateEnhanced);
        if (auto* tool = sceneController->GetTransformTool())
            tool->SetRotateGizmoEnhanced(rotateEnhanced);
    }
}

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
