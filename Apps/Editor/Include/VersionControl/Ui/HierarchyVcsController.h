#pragma once

#include "ECS/ECS.h"
#include "UI/Controls/TreeView.h"
#include "UI/UiCoalescedPost.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/SceneDiff.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
struct EditorContext;
class HierarchyPanel;
class INativeContextMenu;
class UIElement;

namespace Editor
{

// A ghost row stands in for an entity that is in the version control baseline
// but no longer in the working scene, so a deletion is visible and revertable
// where it happened rather than only in a file diff.
//
// The low 32 bits are saturated on purpose: HierarchyDataProvider::Decode reads
// them as an entity index and version, and 0xFFFFFFFF is neither a live index
// nor a live version. Every seam that decodes a row id already rejects handles
// the world does not know, so they all decline ghost rows without needing to
// learn what one is. The identifying index lives above bit 32.
// Mirrors HierarchyDataProvider's root sentinel; a static_assert in the panel
// keeps the two from drifting.
inline constexpr TreeId kHierarchyRootTreeId = ~TreeId{0};

inline constexpr TreeId kGhostTreeIdFlag = TreeId{1} << 62;
inline constexpr TreeId kGhostTreeIdEntityBits = 0xFFFFFFFFull;

// kHierarchyRootTreeId (all bits set) also matches this flag; every consumer
// checks the root sentinel before the ghost test, and the root is never bound
// to a row, so the alias is unreachable — keep that ordering when adding
// consumers.
constexpr bool IsGhostTreeId(TreeId id)
{
    return (id & kGhostTreeIdFlag) != 0;
}

constexpr TreeId MakeGhostTreeId(uint32_t ghostIndex)
{
    return kGhostTreeIdFlag | (static_cast<TreeId>(ghostIndex) << 32) |
           kGhostTreeIdEntityBits;
}

constexpr uint32_t GhostTreeIdIndex(TreeId id)
{
    return static_cast<uint32_t>((id >> 32) & 0x3FFFFFFFull);
}

// Owns provider-neutral scene diff and VCS lock presentation for HierarchyPanel.
// The panel delegates row decoration and lock interactions here so its core
// hierarchy/selection implementation does not depend on VCS provider state.
class HierarchyVcsController final
{
  public:
    explicit HierarchyVcsController(HierarchyPanel& panel);
    ~HierarchyVcsController();

    // Subscribes to provider status changes so a commit, checkout or external
    // edit refreshes the markers. Without it the Hierarchy only reloads on
    // scene save/load and keeps showing pre-commit state indefinitely.
    void SetContext(const EditorContext* context);
    void SetSceneDiffProvider(std::function<std::vector<SceneObjectDiff>()> provider);
    void SetScenePathProvider(
        std::function<std::optional<std::filesystem::path>()> provider);
    void Refresh();
    void RefreshLiveState();
    void Update();

    void DecorateRow(ECS::EntityHandle entity, UIElement& row);
    // Renders a baseline-only entity: dimmed label, Removed marker, no entity
    // affordances (it has no entity to act on until it is restored).
    void DecorateGhostRow(TreeId id, UIElement& row);
    bool ShowGhostContextMenu(TreeId id, float x, float y);
    std::string GhostLabel(TreeId id) const;
    bool IsSceneLocked() const;
    std::string LockTooltip(bool locallyLocked) const;
    void ShowLockContextMenu(TreeId id, float x, float y);
    // True when the point lands on a row's lock control or scene-diff marker.
    // Only those two elements route to the lock menu; everything else on the
    // row keeps the hierarchy's own context menu.
    bool IsVcsRowControlPoint(float x, float y) const;

    // Flips the shared Hierarchy/Inspector scene-diff indicator preference.
    // The host supplies the action because the preference reaches further than
    // this panel: the Inspector paints from the same flag and has to repaint
    // with it. Without a host, the Hierarchy does not offer the toggle.
    void SetIndicatorVisibilityToggle(std::function<void()> toggle);
    bool CanToggleIndicatorVisibility() const { return static_cast<bool>(m_IndicatorVisibilityToggle); }
    void ToggleIndicatorVisibility();

  private:
    void DetachService();
    bool IsEditMode() const;
    void ReloadDiffCache(bool isRetry);
    void ReloadLockState(bool force = false);
    void RebuildDescendantDiffCounts();
    const SceneObjectDiff* FindEntityDiff(ECS::EntityHandle entity) const;
    bool IsLiveAddedEntity(ECS::EntityHandle entity) const;
    SceneObjectDiff MakeLiveAddedDiff(ECS::EntityHandle entity) const;
    void RevertEntityToBaseline(ECS::EntityHandle entity,
                                const SceneObjectDiff& object);
    void RestoreGhostEntity(TreeId id);
    void RebuildGhostRows();

    HierarchyPanel& m_Panel;
    std::function<void()> m_IndicatorVisibilityToggle;
    std::function<std::vector<SceneObjectDiff>()> m_SceneDiffProvider;
    std::function<std::optional<std::filesystem::path>()> m_ScenePathProvider;
    std::optional<std::filesystem::path> m_SceneLockPath;
    VCSLockInfo m_SceneLockInfo;
    std::string m_SceneLockProviderName;
    bool m_LockStateQueried = false;
    std::unordered_map<std::string, SceneObjectDiff> m_SceneDiffByStableKey;
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> m_DescendantDiffCounts;
    std::string m_SceneDiffProviderName;
    // Stable keys of baseline-only entities, indexed by ghost row id.
    std::vector<std::string> m_GhostStableKeys;
    bool m_AwaitingProvider = false;
    bool m_AwaitingContent = false;
    uint32_t m_RetryFrames = 0;
    uint32_t m_RetriesRemaining = 0;
    // The scene the current diff describes, and a countdown for noticing that
    // it has changed. A scene load can complete after the retry budget is
    // spent, and there is no after-load hook to key off.
    std::optional<std::filesystem::path> m_DiffScenePath;
    uint32_t m_ScenePathPollFrames = 0;
    EditorVersionControlService::Subscription m_VcsSubscription;
    // Requested from the provider's status thread; cancelled before this controller dies so
    // neither a mid-flight notification nor an already-queued refresh can reach it.
    UI::UiCoalescedPost m_StatusRefresh;
    std::unique_ptr<INativeContextMenu> m_LockContextMenu;
};

} // namespace Editor
} // namespace GameEngine
