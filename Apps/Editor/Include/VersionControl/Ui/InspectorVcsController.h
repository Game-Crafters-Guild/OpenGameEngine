#pragma once

#include "UI/UiCoalescedPost.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/SceneDiff.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace GameEngine
{
struct EditorContext;
class InspectorPanel;
class INativeContextMenu;

namespace Editor
{
// Owns Inspector scene-diff state, VCS notifications, decoration, and property
// actions. InspectorPanel delegates to this controller and remains concerned
// with building and refreshing the underlying Inspector UI.
class InspectorVcsController final
{
  public:
    explicit InspectorVcsController(InspectorPanel& panel);
    ~InspectorVcsController();

    void SetContext(const EditorContext* context);
    void SetSceneDiffProvider(std::function<std::vector<SceneObjectDiff>()> provider);
    void ApplyDecorations();

  private:
    void ShowPropertyContextMenu(const ScenePropertyDiff& property, float x, float y);
    void RevertProperty(const ScenePropertyDiff& property);
    void DetachService();

    InspectorPanel& m_Panel;
    EditorVersionControlService::Subscription m_VcsSubscription;
    std::function<std::vector<SceneObjectDiff>()> m_SceneDiffProvider;
    // Requested from the provider's status thread; cancelled before this controller dies so
    // neither a mid-flight notification nor an already-queued refresh can reach it.
    UI::UiCoalescedPost m_StatusRefresh;
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
};

} // namespace Editor
} // namespace GameEngine
