#include "VersionControl/Ui/HierarchyVcsController.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "EditorContext.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsStatusUi.h"
#include "EditorChangeNotifications.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Logger/Logger.h"
#include "Panels/HierarchyPanel.h"
#include "PlayMode/PlayModeManager.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/WorldSnapshotCommand.h"
#include "UI/EditorIcons.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/SceneDiffLiveOverlay.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr uint32_t kCmdLockEntity = 0x41B0;
constexpr uint32_t kCmdLockSceneAcquire = 0x41B1;
constexpr uint32_t kCmdLockSceneRelease = 0x41B2;
constexpr uint32_t kCmdLockSceneRequest = 0x41B3;
constexpr uint32_t kCmdLockSceneRefresh = 0x41B4;
constexpr uint32_t kCmdRevertEntityToBaseline = 0x41B5;
constexpr uint32_t kCmdRestoreGhostEntity = 0x41B6;

// Descendant-only markers are deliberately neutral: the row itself is
// unchanged, so it must not borrow an Added/Modified/Removed colour.
constexpr uint32_t kDescendantOnlyDotColor = 0xFF9AA0A6u;

// Reading the diff needs a provider process, so a scene whose baseline is
// momentarily unavailable is retried a bounded number of times and then left
// alone until something real (a save, a scene load, a status change) asks
// again. An open-ended retry turns a missing baseline into a permanent
// subprocess loop.
constexpr uint32_t kContentRetryFrameInterval = 30u;
constexpr uint32_t kContentRetryAttempts = 4u;
// How often to notice that the active scene changed while no diff is loaded.
constexpr uint32_t kScenePathPollFrameInterval = 30u;

const char* DiffClass(SceneDiffState state)
{
    switch (state)
    {
        case SceneDiffState::Added: return "hierarchy-vcs-diff-added";
        case SceneDiffState::Removed: return "hierarchy-vcs-diff-removed";
        case SceneDiffState::Modified: return "hierarchy-vcs-diff-modified";
        case SceneDiffState::Unchanged: return nullptr;
    }
    return nullptr;
}

size_t DiffCountIndex(SceneDiffState state)
{
    switch (state)
    {
        case SceneDiffState::Added: return 0;
        case SceneDiffState::Modified: return 1;
        case SceneDiffState::Removed: return 2;
        case SceneDiffState::Unchanged: break;
    }
    return 1;
}

std::string DiffTooltip(std::string_view providerName,
                        const SceneObjectDiff& object,
                        const std::array<uint32_t, 3>* descendantCounts)
{
    std::ostringstream tooltip;
    if (!providerName.empty())
        tooltip << providerName << " · ";
    tooltip << SceneDiffStateLabel(object.State);
    const std::string& label = object.CurrentLabel.empty() ? object.OriginalLabel : object.CurrentLabel;
    if (!label.empty())
        tooltip << "\nEntity: " << label;
    if (object.WasReparented())
        tooltip << "\nReparented · "
                << (object.OriginalParent.empty() ? "root" : object.OriginalParent)
                << " -> "
                << (object.CurrentParent.empty() ? "root" : object.CurrentParent);
    for (const ScenePropertyDiff& property : object.Properties)
    {
        if (property.State == SceneDiffState::Unchanged)
            continue;
        tooltip << '\n' << SceneDiffStateLabel(property.State) << " · " << property.Key;
        if (!property.OriginalValue.empty())
            tooltip << "\n  Baseline: " << property.OriginalValue;
        if (!property.CurrentValue.empty())
            tooltip << "\n  Current: " << property.CurrentValue;
    }
    if (descendantCounts)
    {
        const uint32_t total =
            (*descendantCounts)[0] + (*descendantCounts)[1] + (*descendantCounts)[2];
        if (total > 0)
            tooltip << "\nDescendants: " << total << " changed"
                    << "\n  Added: " << (*descendantCounts)[0]
                    << " · Modified: " << (*descendantCounts)[1]
                    << " · Removed: " << (*descendantCounts)[2];
    }
    return tooltip.str();
}

std::string DescendantTooltip(std::string_view providerName,
                              std::string_view entityLabel,
                              const std::array<uint32_t, 3>& counts)
{
    std::ostringstream tooltip;
    if (!providerName.empty())
        tooltip << providerName << " · ";
    tooltip << "Changed descendants";
    if (!entityLabel.empty())
        tooltip << "\nEntity: " << entityLabel;
    tooltip << "\nDescendants: " << counts[0] + counts[1] + counts[2] << " changed"
            << "\n  Added: " << counts[0]
            << " · Modified: " << counts[1]
            << " · Removed: " << counts[2];
    return tooltip.str();
}

ECS::EntityHandle EffectiveParent(ECS::World& world, ECS::EntityHandle entity)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return {};
    if (auto* parent = world.GetComponent<Components::Parent>(entity);
        parent && parent->parent.IsValid() && world.IsValid(parent->parent))
        return parent->parent;
    return {};
}

ECS::EntityHandle DecodeTreeId(TreeId id)
{
    const uint32 raw = static_cast<uint32>(id);
    const ECS::EntityIndex index =
        static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
    const ECS::EntityVersion version = static_cast<ECS::EntityVersion>(
        (raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
    return {index, version};
}

ECS::EntityHandle FindEntityBySceneId(ECS::World& world, std::string_view sceneId)
{
    if (sceneId.empty())
        return {};
    std::vector<ECS::EntityHandle> alive;
    world.GetAliveEntitiesSnapshot(alive);
    for (const ECS::EntityHandle candidate : alive)
    {
        const auto* tag = world.GetComponent<Components::SceneEntityTag>(candidate);
        if (tag && sceneId == tag->View())
            return candidate;
    }
    return {};
}

struct ComponentBaseline
{
    std::string Name;
    // Field names are folded through SceneSchemaFieldName, so they are owned
    // strings rather than views into the diff keys.
    std::vector<std::pair<std::string, std::string>> Properties;
    bool PresentInBaseline = false;
    bool PresentInCurrent = false;
};

std::vector<std::pair<std::string_view, std::string_view>> PropertyViews(
    const ComponentBaseline& component)
{
    return {component.Properties.begin(), component.Properties.end()};
}
} // namespace

HierarchyVcsController::HierarchyVcsController(HierarchyPanel& panel)
    : m_Panel(panel)
{
}

HierarchyVcsController::~HierarchyVcsController()
{
    DetachService();
}

void HierarchyVcsController::DetachService()
{
    // Unsubscribing does not join a callback already running on the status
    // thread (Broadcast copies the list and invokes outside the lock). A running
    // callback only touches its own copy of the coalesced post, and Cancel keeps
    // any refresh it queued from running.
    m_StatusRefresh.Cancel();
    m_VcsSubscription.Reset();
}

void HierarchyVcsController::SetContext(const EditorContext* context)
{
    DetachService();
    if (!context || context->UIReplayActive || !context->VcsService)
        return;
    // The listener runs on the provider's status thread and does nothing but request: the
    // provider registry is main-thread-only (unlocked; package module reloads mutate it), so
    // even the has-anything-changed check waits for the UI thread. An idle poll then costs one
    // posted no-op.
    m_StatusRefresh = UI::UiCoalescedPost(m_Panel.GetPostHandle(), [this]()
    {
        // The broadcast fires once per poll whether or not anything changed; only a real
        // status transition invalidates and refreshes.
        if (!InvalidateVersionControlledSceneDiffsWhoseStatusChanged())
            return;
        Refresh();
    });
    m_VcsSubscription = context->VcsService->AddListener([refresh = m_StatusRefresh]() { refresh.Request(); });
}

void HierarchyVcsController::SetSceneDiffProvider(
    std::function<std::vector<SceneObjectDiff>()> provider)
{
    m_SceneDiffProvider = std::move(provider);
    Refresh();
}

void HierarchyVcsController::SetIndicatorVisibilityToggle(std::function<void()> toggle)
{
    m_IndicatorVisibilityToggle = std::move(toggle);
}

void HierarchyVcsController::ToggleIndicatorVisibility()
{
    if (m_IndicatorVisibilityToggle)
        m_IndicatorVisibilityToggle();
}

void HierarchyVcsController::SetScenePathProvider(
    std::function<std::optional<std::filesystem::path>()> provider)
{
    m_ScenePathProvider = std::move(provider);
    ReloadLockState(true);
    if (m_Panel.m_Tree)
        m_Panel.m_Tree->RefreshFromProvider();
}

void HierarchyVcsController::Refresh()
{
    if (!IsEditMode())
        return;
    // An explicit refresh is a real event (scene load or save, provider
    // arrival, settings change), so the retry budget starts over.
    m_RetriesRemaining = kContentRetryAttempts;
    // No-ops unless the active scene actually changed.
    ReloadLockState();
    ReloadDiffCache(false);
    RefreshLiveState();
}

void HierarchyVcsController::RefreshLiveState()
{
    if (m_Panel.m_Tree)
        m_Panel.m_Tree->RefreshFromProvider();
}

bool HierarchyVcsController::IsEditMode() const
{
    return !m_Panel.m_Context || !m_Panel.m_Context->PlayMode ||
           m_Panel.m_Context->PlayMode->GetState() == PlayModeState::Edit;
}

void HierarchyVcsController::ReloadDiffCache(bool isRetry)
{
    if (!IsEditMode())
        return;
    if (isRetry && m_RetriesRemaining > 0)
        --m_RetriesRemaining;
    // Lock state is not reloaded here: it changes on user action, not on every
    // hierarchy refresh, and querying it costs provider processes.
    m_SceneDiffByStableKey.clear();
    m_DescendantDiffCounts.clear();
    m_SceneDiffProviderName.clear();

    EditorVcsProviderDescriptor provider;
    const bool hasActiveProvider =
        EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
        provider.Integration && provider.Integration().IsRepository();
    m_AwaitingProvider = static_cast<bool>(m_SceneDiffProvider) && !hasActiveProvider;
    m_AwaitingContent = false;
    if (!m_SceneDiffProvider)
        return;

    m_DiffScenePath = m_ScenePathProvider ? m_ScenePathProvider() : std::nullopt;
    auto objects = m_SceneDiffProvider();
    m_AwaitingContent =
        hasActiveProvider && objects.empty() && m_RetriesRemaining > 0;
    m_RetryFrames = m_AwaitingContent ? kContentRetryFrameInterval : 0u;
    for (SceneObjectDiff& object : objects)
        if (object.IsEntity)
            m_SceneDiffByStableKey.emplace(object.StableKey, std::move(object));
    RebuildDescendantDiffCounts();
    if (hasActiveProvider)
        m_SceneDiffProviderName = provider.DisplayName;
    RebuildGhostRows();
}

void HierarchyVcsController::ReloadLockState(bool force)
{
    if (!IsEditMode())
        return;
    const auto nextPath = m_ScenePathProvider ? m_ScenePathProvider() : std::nullopt;
    // GetLockInfo spawns provider processes and, for Git LFS, talks to the lock
    // server with no timeout. Query it when the answer can actually have
    // changed — a different scene, or a user lock action — never on a timer.
    if (!force && nextPath == m_SceneLockPath && m_LockStateQueried)
        return;

    m_SceneLockPath = nextPath;
    m_LockStateQueried = true;
    m_SceneLockInfo = {};
    m_SceneLockProviderName.clear();
    EditorVcsProviderDescriptor provider;
    if (!m_SceneLockPath ||
        !EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) ||
        !provider.Integration || !provider.Integration().IsRepository())
        return;
    m_SceneLockInfo = provider.Integration().GetLockInfo(*m_SceneLockPath);
    m_SceneLockProviderName = provider.DisplayName;
}

bool HierarchyVcsController::IsSceneLocked() const
{
    // Holding the lock is what grants the right to edit, so only someone else's
    // lock presents the hierarchy as locked.
    return m_SceneLockInfo.State == VCSLockState::LockedByOthers;
}

std::string HierarchyVcsController::LockTooltip(bool locallyLocked) const
{
    std::string text = locallyLocked ? "Editor lock: entity is locked"
                                     : "Editor lock: entity is unlocked";
    if (m_SceneLockInfo.State == VCSLockState::LockedByMe)
        text += "\n" + m_SceneLockProviderName + ": scene file locked by you";
    else if (m_SceneLockInfo.State == VCSLockState::LockedByOthers)
        text += "\n" + m_SceneLockProviderName + ": scene file locked by " +
                (m_SceneLockInfo.Owner.empty() ? std::string("another user")
                                               : m_SceneLockInfo.Owner);
    text += "\nRight-click for lock actions";
    return text;
}

void HierarchyVcsController::DecorateRow(ECS::EntityHandle entity, UIElement& row)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass("tree-row-lock"))
        {
            const bool locallyLocked =
                m_Panel.m_LockedEntityIds.count(entity.id) != 0;
            child->SetTooltip(LockTooltip(locallyLocked));
            break;
        }

    UIElement* diffDot = nullptr;
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass("hierarchy-vcs-diff-dot"))
        {
            diffDot = child.get();
            break;
        }

    if (!AreVcsSceneDiffIndicatorsVisible())
    {
        row.RemoveClass("has-vcs-diff");
        if (diffDot)
        {
            diffDot->Overrides().Set(Style::Display, DisplayMode::None);
            diffDot->SetTooltip("");
        }
        return;
    }

    const auto* sceneTag =
        m_Panel.m_World
            ? m_Panel.m_World->GetComponent<Components::SceneEntityTag>(entity)
            : nullptr;
    const auto diffIt = sceneTag && sceneTag->value[0] != '\0'
                            ? m_SceneDiffByStableKey.find(std::string("entity:") + std::string(sceneTag->View()))
                            : m_SceneDiffByStableKey.end();
    const bool liveAdded = IsLiveAddedEntity(entity);
    // Removed cannot describe a row that exists: it means the entity is gone
    // from the file but alive in the world (an unsaved restore), which the
    // ghost suppression already accounts for.
    const bool hasDiff =
        liveAdded ||
        (diffIt != m_SceneDiffByStableKey.end() &&
         diffIt->second.State != SceneDiffState::Unchanged &&
         diffIt->second.State != SceneDiffState::Removed);
    const auto descendantIt = m_DescendantDiffCounts.find(entity.id);
    const bool hasDescendantDiff =
        descendantIt != m_DescendantDiffCounts.end() &&
        descendantIt->second[0] + descendantIt->second[1] + descendantIt->second[2] > 0;

    if ((hasDiff || hasDescendantDiff) && !diffDot)
    {
        auto dot = std::make_unique<UIElement>();
        dot->AddClass("hierarchy-vcs-diff-dot");
        diffDot = dot.get();
        row.AddChild(std::move(dot));
    }
    if (!diffDot)
    {
        row.RemoveClass("has-vcs-diff");
        return;
    }

    diffDot->RemoveClass("hierarchy-vcs-diff-added");
    diffDot->RemoveClass("hierarchy-vcs-diff-removed");
    diffDot->RemoveClass("hierarchy-vcs-diff-modified");
    diffDot->RemoveClass("hierarchy-vcs-diff-descendant");
    if (!hasDiff && !hasDescendantDiff)
    {
        row.RemoveClass("has-vcs-diff");
        diffDot->Overrides().Set(Style::Display, DisplayMode::None);
        diffDot->SetTooltip("");
        return;
    }

    // Only a row that really shows the dot reserves its strip; reserving it on
    // every row cuts names short for a marker that is not there.
    row.AddClass("has-vcs-diff");

    const SceneDiffState state =
        liveAdded ? SceneDiffState::Added
                  : (hasDiff ? diffIt->second.State
                             : SceneDiffState::Modified);
    if (const char* cssClass = DiffClass(state))
        diffDot->AddClass(cssClass);
    if (!hasDiff)
        diffDot->AddClass("hierarchy-vcs-diff-descendant");
    diffDot->Overrides()
        .Set(Style::BackgroundTint,
             hasDiff ? SceneDiffStateColorArgb(state) : kDescendantOnlyDotColor)
        .Set(Style::Display, DisplayMode::Flex);
    if (hasDiff)
    {
        if (liveAdded)
        {
            const SceneObjectDiff object = MakeLiveAddedDiff(entity);
            diffDot->SetTooltip(DiffTooltip(
                m_SceneDiffProviderName, object,
                hasDescendantDiff ? &descendantIt->second : nullptr));
        }
        else
        {
            diffDot->SetTooltip(DiffTooltip(
                m_SceneDiffProviderName, diffIt->second,
                hasDescendantDiff ? &descendantIt->second : nullptr));
        }
    }
    else
    {
        const auto* name =
            m_Panel.m_World
                ? m_Panel.m_World->GetComponent<Components::Name>(entity)
                : nullptr;
        diffDot->SetTooltip(DescendantTooltip(
            m_SceneDiffProviderName,
            name ? name->View() : std::string_view{},
            descendantIt->second));
    }
}

void HierarchyVcsController::ShowLockContextMenu(TreeId id, float x, float y)
{
    if (!m_Panel.m_Window || !m_Panel.m_World || id == 0 ||
        id == std::numeric_limits<TreeId>::max())
        return;
    const ECS::EntityHandle handle = DecodeTreeId(id);
    if (!handle.IsValid() || !m_Panel.m_World->IsValid(handle))
        return;

    if (!m_LockContextMenu)
        m_LockContextMenu = CreateContextMenu();
    if (!m_LockContextMenu)
        return;
    m_LockContextMenu->Clear();

    const bool locallyLocked = m_Panel.m_LockedEntityIds.count(handle.id) != 0;
    // The cached diff describes the scene file, which lags the world by every
    // unsaved edit. Revert claims to restore the entity, not the last save, so
    // it works from the live-overlaid diff.
    std::optional<SceneObjectDiff> revertDiff;
    if (const SceneObjectDiff* entityDiff = FindEntityDiff(handle))
    {
        SceneObjectDiff live = OverlaySceneDiffWithLiveValues(
            *entityDiff, *m_Panel.m_World, handle);
        if (live.State != SceneDiffState::Unchanged)
            revertDiff = std::move(live);
    }
    else if (IsLiveAddedEntity(handle))
        revertDiff = MakeLiveAddedDiff(handle);
    if (revertDiff)
    {
        m_LockContextMenu->AddItem(
            0, "Revert Entity to Baseline", kCmdRevertEntityToBaseline);
        m_LockContextMenu->SetItemIcon(kCmdRevertEntityToBaseline, EditorIcons::kReset);
        m_LockContextMenu->AddSeparator(0);
    }
    m_LockContextMenu->AddItem(
        0, locallyLocked ? "Unlock Entity" : "Lock Entity", kCmdLockEntity);
    m_LockContextMenu->SetItemIcon(kCmdLockEntity,
                                   locallyLocked ? EditorIcons::kLockOpen : EditorIcons::kLockClosed);
    if (m_SceneLockInfo.State != VCSLockState::Unsupported && m_SceneLockPath)
    {
        m_LockContextMenu->AddSeparator(0);
        if (m_SceneLockInfo.CanAcquire)
        {
            m_LockContextMenu->AddItem(0, "Lock Scene File", kCmdLockSceneAcquire);
            m_LockContextMenu->SetItemIcon(kCmdLockSceneAcquire, EditorIcons::kLockClosed);
        }
        if (m_SceneLockInfo.CanRelease)
        {
            m_LockContextMenu->AddItem(0, "Unlock Scene File", kCmdLockSceneRelease);
            m_LockContextMenu->SetItemIcon(kCmdLockSceneRelease, EditorIcons::kLockOpen);
        }
        if (m_SceneLockInfo.CanRequest)
        {
            m_LockContextMenu->AddItem(0, "Request Scene Lock", kCmdLockSceneRequest);
            m_LockContextMenu->SetItemIcon(kCmdLockSceneRequest, EditorIcons::kLockClosed);
        }
        if (m_SceneLockInfo.State == VCSLockState::LockedByOthers)
        {
            const std::string owner =
                m_SceneLockInfo.Owner.empty() ? "another user" : m_SceneLockInfo.Owner;
            m_LockContextMenu->AddItem(
                0, "Locked by " + owner, 0, MenuItemFlag_Disabled);
        }
        m_LockContextMenu->AddItem(
            0, "Refresh Lock Status", kCmdLockSceneRefresh);
        m_LockContextMenu->SetItemIcon(kCmdLockSceneRefresh, EditorIcons::kReset);
    }

    m_LockContextMenu->SetCommandHandler([this, handle, revertDiff](uint32_t command)
    {
        if (command == kCmdRevertEntityToBaseline && revertDiff)
        {
            RevertEntityToBaseline(handle, *revertDiff);
            return;
        }
        if (command == kCmdLockEntity)
        {
            if (m_Panel.m_LockedEntityIds.count(handle.id))
                m_Panel.m_LockedEntityIds.erase(handle.id);
            else
                m_Panel.m_LockedEntityIds.insert(handle.id);
        }
        else if (m_SceneLockPath)
        {
            if (auto* vcs = EditorVcsProviderRegistry::Get().ActiveIntegration())
            {
                if (command == kCmdLockSceneAcquire)
                    (void)vcs->AcquireLock(*m_SceneLockPath);
                else if (command == kCmdLockSceneRelease)
                    (void)vcs->ReleaseLock(*m_SceneLockPath);
                else if (command == kCmdLockSceneRequest)
                    (void)vcs->RequestLock(*m_SceneLockPath);
                vcs->RefreshStatus(*m_SceneLockPath);
            }
        }
        ReloadLockState(true);
        if (m_Panel.m_Tree)
            m_Panel.m_Tree->RefreshFromProvider();
    });
    m_LockContextMenu->Show(
        m_Panel.m_Window, static_cast<int>(x), static_cast<int>(y));
}

const SceneObjectDiff* HierarchyVcsController::FindEntityDiff(
    ECS::EntityHandle entity) const
{
    if (!m_Panel.m_World || !entity.IsValid() ||
        !m_Panel.m_World->IsValid(entity))
        return nullptr;
    const auto* tag =
        m_Panel.m_World->GetComponent<Components::SceneEntityTag>(entity);
    if (!tag || tag->value[0] == '\0')
        return nullptr;
    const auto it =
        m_SceneDiffByStableKey.find(std::string("entity:") + std::string(tag->View()));
    return it != m_SceneDiffByStableKey.end() ? &it->second : nullptr;
}

bool HierarchyVcsController::IsLiveAddedEntity(
    ECS::EntityHandle entity) const
{
    if (!IsEditMode() || !m_Panel.m_World || !entity.IsValid() ||
        !m_Panel.m_World->IsValid(entity) ||
        m_SceneDiffProviderName.empty() || !m_SceneLockPath ||
        m_Panel.m_World->HasComponent<Components::RuntimeOnlyEntity>(entity))
        return false;
    const auto* tag =
        m_Panel.m_World->GetComponent<Components::SceneEntityTag>(entity);
    return !tag || tag->value[0] == '\0';
}

SceneObjectDiff HierarchyVcsController::MakeLiveAddedDiff(
    ECS::EntityHandle entity) const
{
    SceneObjectDiff object{};
    object.IsEntity = true;
    object.State = SceneDiffState::Added;
    if (m_Panel.m_World)
        if (const auto* name =
                m_Panel.m_World->GetComponent<Components::Name>(entity))
            object.CurrentLabel = name->View();
    return object;
}

void HierarchyVcsController::RevertEntityToBaseline(
    ECS::EntityHandle entity, const SceneObjectDiff& object)
{
    if (!m_Panel.m_World || !entity.IsValid() ||
        !m_Panel.m_World->IsValid(entity))
        return;

    ECS::World& world = *m_Panel.m_World;
    const std::vector<uint8_t> before = world.SerializeWorld();
    bool applied = true;
    std::string error;

    if (object.State == SceneDiffState::Added)
    {
        auto subtree = DeleteEntitiesCommand::CollectSubtree(world, entity);
        std::reverse(subtree.begin(), subtree.end()); // children before parents
        world.DestroyEntitiesImmediate(subtree);
    }
    else
    {
        std::vector<ComponentBaseline> components;
        std::unordered_map<std::string, size_t> componentIndices;
        for (const ScenePropertyDiff& property : object.Properties)
        {
            const size_t dot = property.Key.find('.');
            if (dot == std::string::npos)
                continue;
            const std::string componentName = property.Key.substr(0, dot);
            auto [indexIt, inserted] =
                componentIndices.try_emplace(componentName, components.size());
            if (inserted)
                components.push_back(ComponentBaseline{componentName});
            ComponentBaseline& component = components[indexIt->second];
            if (!property.OriginalValue.empty())
                component.PresentInBaseline = true;
            if (!property.CurrentValue.empty())
                component.PresentInCurrent = true;
            if (property.State != SceneDiffState::Unchanged &&
                !property.OriginalValue.empty())
                component.Properties.emplace_back(
                    SceneSchemaFieldName(
                        std::string_view(property.Key).substr(dot + 1)),
                    property.OriginalValue);
        }

        Scene::SceneLoadContext loadContext{};
        loadContext.TargetWorld = &world;
        for (const ComponentBaseline& component : components)
        {
            const Scene::ISceneComponentSchema* schema =
                Scene::SceneSchemaRegistry::Find(component.Name);
            if (!schema)
            {
                applied = false;
                error = "no scene schema for component '" + component.Name + "'";
                break;
            }
            if (!component.PresentInBaseline)
            {
                if (component.PresentInCurrent && !schema->Remove(world, entity))
                {
                    applied = false;
                    error = "could not remove added component '" +
                            component.Name + "'";
                    break;
                }
                continue;
            }
            if (component.Properties.empty())
                continue;
            size_t failedIndex = 0;
            const auto views = PropertyViews(component);
            if (!schema->ApplyProperties(
                    world, entity, loadContext, views, &error, &failedIndex))
            {
                applied = false;
                break;
            }
        }
    }

    // Parenting lives in the section header rather than in a component, so it
    // is restored here rather than through a schema.
    if (applied && object.State != SceneDiffState::Added && object.WasReparented())
    {
        const ECS::EntityHandle baselineParent =
            FindEntityBySceneId(world, object.OriginalParent);
        if (!object.OriginalParent.empty() && !baselineParent.IsValid())
        {
            applied = false;
            error = "baseline parent '" + object.OriginalParent +
                    "' is not in the scene";
        }
        else if (baselineParent == entity)
        {
            applied = false;
            error = "baseline parent is the entity itself";
        }
        else if (auto* parent =
                     world.GetComponentForWrite<Components::Parent>(entity))
        {
            parent->parent = baselineParent;
        }
        else
        {
            world.AddComponentImmediate(entity, Components::Parent{baselineParent});
        }
    }

    world.ProcessCommands();
    if (!applied)
    {
        world.DeserializeWorld(before);
        world.ProcessCommands();
        Logger::Log::Warning(
            "Hierarchy scene diff: failed to revert entity '{}': {}",
            object.CurrentLabel.empty() ? object.OriginalLabel
                                        : object.CurrentLabel,
            error);
        return;
    }

    const std::vector<uint8_t> after = world.SerializeWorld();
    if (before == after)
        return;
    if (m_Panel.m_Undo)
        m_Panel.m_Undo->CommitAlreadyApplied(
            std::make_unique<WorldSnapshotCommand>(
                "Revert Entity to VCS Baseline", &world,
                m_Panel.m_ChangeNotifications, before, after));
    if (m_Panel.m_ChangeNotifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent event{};
        event.world = &world;
        event.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Panel.m_ChangeNotifications->NotifyWorldStructureChanged(event);
    }
    RefreshLiveState();
}

void HierarchyVcsController::RestoreGhostEntity(TreeId id)
{
    if (!m_Panel.m_World || !IsGhostTreeId(id))
        return;
    const uint32_t index = GhostTreeIdIndex(id);
    if (index >= m_GhostStableKeys.size())
        return;
    const auto diffIt = m_SceneDiffByStableKey.find(m_GhostStableKeys[index]);
    if (diffIt == m_SceneDiffByStableKey.end())
        return;
    const SceneObjectDiff object = diffIt->second;

    ECS::World& world = *m_Panel.m_World;
    const std::string sceneId =
        object.StableKey.rfind("entity:", 0) == 0 ? object.StableKey.substr(7)
                                                  : object.StableKey;
    // A stale row can outlive the entity it describes by a frame; restoring
    // through it must not mint a second copy.
    if (FindEntityBySceneId(world, sceneId).IsValid())
    {
        Logger::Log::Warning(
            "Hierarchy scene diff: '{}' is already in the scene; not restoring a second copy",
            object.OriginalLabel.empty() ? sceneId : std::string_view(object.OriginalLabel));
        Refresh();
        return;
    }
    const std::vector<uint8_t> before = world.SerializeWorld();

    const ECS::EntityHandle entity = world.CreateEntity();
    bool applied = entity.IsValid();
    std::string error = applied ? std::string{} : "could not create the entity";

    // The scene id is what ties the entity back to its baseline; without it the
    // restored entity would immediately read as a new addition.
    if (applied)
    {
        Components::SceneEntityTag tag{};
        const size_t copied = std::min(sceneId.size(), sizeof(tag.value) - 1);
        std::memcpy(tag.value, sceneId.data(), copied);
        tag.value[copied] = '\0';
        world.AddComponentImmediate(entity, tag);
    }

    if (applied)
    {
        std::vector<ComponentBaseline> components;
        std::unordered_map<std::string, size_t> componentIndices;
        for (const ScenePropertyDiff& property : object.Properties)
        {
            if (property.OriginalValue.empty())
                continue;
            const size_t dot = property.Key.find('.');
            if (dot == std::string::npos)
                continue;
            const std::string componentName = property.Key.substr(0, dot);
            auto [indexIt, inserted] =
                componentIndices.try_emplace(componentName, components.size());
            if (inserted)
                components.push_back(ComponentBaseline{componentName});
            components[indexIt->second].Properties.emplace_back(
                SceneSchemaFieldName(
                    std::string_view(property.Key).substr(dot + 1)),
                property.OriginalValue);
        }

        Scene::SceneLoadContext loadContext{};
        loadContext.TargetWorld = &world;
        for (const ComponentBaseline& component : components)
        {
            const Scene::ISceneComponentSchema* schema =
                Scene::SceneSchemaRegistry::Find(component.Name);
            if (!schema)
            {
                applied = false;
                error = "no scene schema for component '" + component.Name + "'";
                break;
            }
            if (component.Properties.empty())
                continue;
            size_t failedIndex = 0;
            const auto views = PropertyViews(component);
            if (!schema->ApplyProperties(world, entity, loadContext, views,
                                         &error, &failedIndex))
            {
                applied = false;
                break;
            }
        }
    }

    // Restoring under a parent that is itself still deleted would silently
    // reroot the entity, so that is refused rather than half-done.
    if (applied && !object.OriginalParent.empty())
    {
        const ECS::EntityHandle parent =
            FindEntityBySceneId(world, object.OriginalParent);
        if (!parent.IsValid())
        {
            applied = false;
            error = "baseline parent '" + object.OriginalParent +
                    "' is not in the scene — restore it first";
        }
        else
        {
            world.AddComponentImmediate(entity, Components::Parent{parent});
        }
    }

    world.ProcessCommands();
    if (!applied)
    {
        world.DeserializeWorld(before);
        world.ProcessCommands();
        Logger::Log::Warning(
            "Hierarchy scene diff: failed to restore entity '{}': {}",
            object.OriginalLabel, error);
        return;
    }

    const std::vector<uint8_t> after = world.SerializeWorld();
    if (before == after)
        return;
    if (m_Panel.m_Undo)
        m_Panel.m_Undo->CommitAlreadyApplied(
            std::make_unique<WorldSnapshotCommand>(
                "Restore Entity from VCS Baseline", &world,
                m_Panel.m_ChangeNotifications, before, after));
    if (m_Panel.m_ChangeNotifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent event{};
        event.world = &world;
        event.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Panel.m_ChangeNotifications->NotifyWorldStructureChanged(event);
    }
    // The structure notification above makes the panel rebuild its provider;
    // this controller still owns retiring the ghost, which only ReloadDiffCache
    // re-evaluates against the now-live entity. The panel's own Refresh stops
    // at RefreshLiveState and would leave the ghost standing.
    Refresh();
}

void HierarchyVcsController::RebuildGhostRows()
{
    m_GhostStableKeys.clear();
    if (!AreVcsSceneDiffIndicatorsVisible())
    {
        m_Panel.SetGhostRows({});
        return;
    }

    // Removed entities, sorted by stable key so the rows keep a deterministic
    // order across rebuilds (the map has already lost the baseline's file
    // order; subtree shape comes from parent nesting, not from this sort).
    // The file diff lags the
    // world: an entity restored from baseline (or re-created by hand) is still
    // Removed on disk until the next save, and a ghost standing beside the
    // live row would offer to restore a second copy.
    std::vector<const SceneObjectDiff*> removed;
    for (const auto& [key, object] : m_SceneDiffByStableKey)
    {
        if (object.State != SceneDiffState::Removed)
            continue;
        if (m_Panel.m_World)
        {
            const std::string_view sceneId =
                key.rfind("entity:", 0) == 0 ? std::string_view(key).substr(7)
                                             : std::string_view(key);
            if (FindEntityBySceneId(*m_Panel.m_World, sceneId).IsValid())
                continue;
        }
        removed.push_back(&object);
    }
    std::sort(removed.begin(), removed.end(),
              [](const SceneObjectDiff* a, const SceneObjectDiff* b)
              { return a->StableKey < b->StableKey; });
    if (removed.empty())
    {
        m_Panel.SetGhostRows({});
        return;
    }

    // A removed entity's baseline parent may itself be removed; resolving the
    // ghost ids first lets a whole deleted subtree nest under its own root.
    std::unordered_map<std::string, TreeId> ghostIdByStableKey;
    m_GhostStableKeys.reserve(removed.size());
    for (const SceneObjectDiff* object : removed)
    {
        ghostIdByStableKey.emplace(
            object->StableKey,
            MakeGhostTreeId(static_cast<uint32_t>(m_GhostStableKeys.size())));
        m_GhostStableKeys.push_back(object->StableKey);
    }

    std::vector<HierarchyPanel::GhostRow> rows;
    rows.reserve(removed.size());
    for (const SceneObjectDiff* object : removed)
    {
        HierarchyPanel::GhostRow row;
        row.Id = ghostIdByStableKey[object->StableKey];
        row.Label = object->OriginalLabel.empty() ? object->StableKey
                                                  : object->OriginalLabel;
        row.ParentId = kHierarchyRootTreeId;
        if (!object->OriginalParent.empty())
        {
            const std::string parentKey = "entity:" + object->OriginalParent;
            if (const auto ghost = ghostIdByStableKey.find(parentKey);
                ghost != ghostIdByStableKey.end())
                row.ParentId = ghost->second;
            else if (m_Panel.m_World)
            {
                const ECS::EntityHandle live =
                    FindEntityBySceneId(*m_Panel.m_World, object->OriginalParent);
                if (live.IsValid())
                    row.ParentId = HierarchyPanel::EntityTreeId(live);
            }
        }
        rows.push_back(std::move(row));
    }
    m_Panel.SetGhostRows(std::move(rows));
}

std::string HierarchyVcsController::GhostLabel(TreeId id) const
{
    const uint32_t index = GhostTreeIdIndex(id);
    if (index >= m_GhostStableKeys.size())
        return {};
    const auto it = m_SceneDiffByStableKey.find(m_GhostStableKeys[index]);
    if (it == m_SceneDiffByStableKey.end())
        return {};
    return it->second.OriginalLabel.empty() ? it->second.StableKey
                                            : it->second.OriginalLabel;
}

void HierarchyVcsController::DecorateGhostRow(TreeId id, UIElement& row)
{
    row.AddClass("hierarchy-ghost-row");
    row.AddClass("has-vcs-diff");

    UIElement* dot = nullptr;
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass("hierarchy-vcs-diff-dot"))
        {
            dot = child.get();
            break;
        }
    if (!dot)
    {
        auto created = std::make_unique<UIElement>();
        created->AddClass("hierarchy-vcs-diff-dot");
        dot = created.get();
        row.AddChild(std::move(created));
    }
    dot->RemoveClass("hierarchy-vcs-diff-added");
    dot->RemoveClass("hierarchy-vcs-diff-modified");
    dot->RemoveClass("hierarchy-vcs-diff-descendant");
    dot->AddClass("hierarchy-vcs-diff-removed");
    dot->Overrides()
        .Set(Style::BackgroundTint, SceneDiffStateColorArgb(SceneDiffState::Removed))
        .Set(Style::Display, DisplayMode::Flex);

    const uint32_t index = GhostTreeIdIndex(id);
    const SceneObjectDiff* object =
        index < m_GhostStableKeys.size()
            ? [&]() -> const SceneObjectDiff*
              {
                  const auto it = m_SceneDiffByStableKey.find(m_GhostStableKeys[index]);
                  return it == m_SceneDiffByStableKey.end() ? nullptr : &it->second;
              }()
            : nullptr;
    if (object)
        dot->SetTooltip(DiffTooltip(m_SceneDiffProviderName, *object, nullptr));
}

bool HierarchyVcsController::ShowGhostContextMenu(TreeId id, float x, float y)
{
    if (!m_Panel.m_Window || !IsGhostTreeId(id))
        return false;
    const uint32_t index = GhostTreeIdIndex(id);
    if (index >= m_GhostStableKeys.size())
        return false;

    if (!m_LockContextMenu)
        m_LockContextMenu = CreateContextMenu();
    if (!m_LockContextMenu)
        return false;
    m_LockContextMenu->Clear();
    m_LockContextMenu->AddItem(0, "Restore Entity from Baseline",
                               kCmdRestoreGhostEntity);
    m_LockContextMenu->SetItemIcon(kCmdRestoreGhostEntity, EditorIcons::kReset);
    m_LockContextMenu->SetCommandHandler([this, id](uint32_t command)
    {
        if (command == kCmdRestoreGhostEntity)
            RestoreGhostEntity(id);
    });
    m_LockContextMenu->Show(m_Panel.m_Window, static_cast<int>(x),
                            static_cast<int>(y));
    return true;
}

void HierarchyVcsController::RebuildDescendantDiffCounts()
{
    m_DescendantDiffCounts.clear();
    if (!m_Panel.m_World)
        return;

    std::vector<ECS::EntityHandle> alive;
    m_Panel.m_World->GetAliveEntitiesSnapshot(alive);
    for (const ECS::EntityHandle entity : alive)
    {
        const auto* tag =
            m_Panel.m_World->GetComponent<Components::SceneEntityTag>(entity);
        SceneDiffState state = SceneDiffState::Unchanged;
        if (IsLiveAddedEntity(entity))
            state = SceneDiffState::Added;
        else if (tag && tag->value[0] != '\0')
        {
            const auto diffIt =
                m_SceneDiffByStableKey.find(std::string("entity:") + std::string(tag->View()));
            if (diffIt != m_SceneDiffByStableKey.end())
                state = diffIt->second.State;
        }
        // Removed on an alive entity is the unsaved-restore case; see DecorateRow.
        if (state == SceneDiffState::Unchanged || state == SceneDiffState::Removed)
            continue;

        ECS::EntityHandle parent = EffectiveParent(*m_Panel.m_World, entity);
        std::unordered_set<uint32_t> visited;
        while (parent.IsValid() && m_Panel.m_World->IsValid(parent) &&
               visited.insert(parent.id).second)
        {
            ++m_DescendantDiffCounts[parent.id][DiffCountIndex(state)];
            parent = EffectiveParent(*m_Panel.m_World, parent);
        }
    }
}

void HierarchyVcsController::Update()
{
    if (!IsEditMode())
        return;
    if (m_AwaitingProvider)
    {
        EditorVcsProviderDescriptor provider;
        if (EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
            provider.Integration && provider.Integration().IsRepository())
            Refresh();
    }
    else if (m_AwaitingContent)
    {
        if (m_RetryFrames > 0)
            --m_RetryFrames;
        if (m_RetryFrames == 0)
        {
            ReloadDiffCache(true);
            RefreshLiveState();
        }
    }
    else if (m_SceneDiffByStableKey.empty() && m_SceneDiffProvider)
    {
        // Nothing is loaded and the retries are spent. The scene path can still
        // arrive later — a scene load finishing after the budget expired leaves
        // the markers permanently absent until something else forces a reload.
        // Polling stops as soon as a diff exists, and never starts without an
        // active provider.
        if (m_ScenePathPollFrames > 0)
        {
            --m_ScenePathPollFrames;
            return;
        }
        m_ScenePathPollFrames = kScenePathPollFrameInterval;
        EditorVcsProviderDescriptor provider;
        if (!EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) ||
            !provider.Integration || !provider.Integration().IsRepository())
            return;
        const auto path = m_ScenePathProvider ? m_ScenePathProvider() : std::nullopt;
        if (path != m_DiffScenePath)
            Refresh();
    }
}

bool HierarchyVcsController::IsVcsRowControlPoint(float x, float y) const
{
    const auto contains = [x, y](const UIElement& element)
    {
        const float width = element.GetLayoutWidth();
        const float height = element.GetLayoutHeight();
        if (width <= 0.0f || height <= 0.0f)
            return false;
        const float left = element.GetLayoutX();
        const float top = element.GetLayoutY();
        return x >= left && x < left + width && y >= top && y < top + height;
    };

    if (!m_Panel.m_Tree)
        return false;
    std::vector<TreeId> held;
    m_Panel.m_Tree->CollectBoundIds(held);
    for (const TreeId id : held)
    {
        const UIElement* row = m_Panel.m_Tree->FindBoundRow(id);
        if (!row || !contains(*row))
            continue;
        for (const auto& child : row->GetChildren())
        {
            if (!child)
                continue;
            const bool isVcsControl = child->HasClass("tree-row-lock") ||
                                      child->HasClass("hierarchy-vcs-diff-dot");
            if (isVcsControl && contains(*child))
                return true;
        }
        // The point is inside this row and not on one of its VCS controls; no
        // other row can contain it.
        return false;
    }
    return false;
}

} // namespace GameEngine::Editor
