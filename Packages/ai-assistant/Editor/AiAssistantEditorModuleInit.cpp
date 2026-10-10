#include "AgentReplyActions.h"
#include "AiAssistantSettings.h"
#include "AssistantGate.h"
#include "CopyReplyAction.h"
#include "PromptPanel.h"

#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Editor/Registries/EditorPanelRegistry.h"

#include <memory>
#include <utility>

namespace
{

struct AiAssistantEditorModuleRegistrar
{
    AiAssistantEditorModuleRegistrar()
    {
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
            {"ai-assistant", "Editor/UI/PromptPanelChrome.css"});

        GameEngine::Editor::EditorPanelDescriptor panel;
        panel.PanelId = "AIAssistant";
        panel.Title = "AI Assistant";
        panel.TabIconClass = "prompt-panel-tab-icon";
        panel.AssetSourceAlias = "ai-assistant";
        panel.LayoutAssetPath = "Editor/UI/panels/PromptPanel.uxml";
        panel.StyleAssetPath = "Editor/UI/panels/PromptPanel.css";
        panel.DefaultDockAnchorPanelId = "SceneView";
        panel.Factory = []() -> std::unique_ptr<GameEngine::UIElement> {
            return std::make_unique<GameEngine::PromptPanel>();
        };
        GameEngine::Editor::EditorPanelRegistry::Get().RegisterPanel(std::move(panel));

        GameEngine::AgentReplyActions::Register({"copy", "Copy", "Copy the reply text", GameEngine::CopyReply});

        GameEngine::AiAssistantSettings::Register();

        GameEngine::Editor::DebugRequestGateRegistry::Get().Register(
            GameEngine::AssistantGate::Get().AsDebugRequestGate());
    }
};

AiAssistantEditorModuleRegistrar s_AiAssistantEditorModuleRegistrar;

} // namespace
