#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AssetCore/GUID.h"
#include "Types/Types.h"
#include "UI/UIElement.h"

namespace GameEngine
{

class AssetManager;
class UIElement;
class UIManager;
struct Stylesheet;

// UIHotReload listens to AssetManager asset reload events and queues UI-related
// reload work to be applied on the UI thread during UIManager::Update().
//
// IMPORTANT: The AssetManager event callback may execute while AssetManager is
// holding internal locks; the callback must therefore be O(1) and must not call
// back into AssetManager. All expensive work is deferred to Pump().
class UIHotReload final
{
  public:
    UIHotReload(UIManager& owner, AssetManager& assets);
    ~UIHotReload();

    UIHotReload(const UIHotReload&) = delete;
    UIHotReload& operator=(const UIHotReload&) = delete;

    // Applies any queued reloads. Must be called on the UI thread (from
    // UIManager::Update()).
    void Pump();

    enum class LayoutBindMode : uint8_t
    {
        // Reconcile the bound target element itself + its children (used for root layout).
        ReconcileSelf = 0,
        // Reconcile only the bound target element's children against the asset root's children
        // (used for binding a UILayout to a custom control element like UIDemoPanel).
        ReconcileChildren
    };

    // Register a UILayout asset binding for a live element subtree. The bindingId returned
    // is stamped onto the managed nodes so future reloads can remove template-authored
    // nodes while preserving runtime-added nodes.
    uint64 RegisterLayoutBinding(const GUID& layoutGuid, UIElement* target, LayoutBindMode mode);

    // Apply a specific layout/style reload immediately (UI thread). These are useful
    // for initial binding/application after registering a binding.
    void ApplyLayoutNow(const GUID& layoutGuid) { ApplyLayoutReload(layoutGuid); }
    void ApplyStyleNow(const GUID& styleGuid) { ApplyStyleReload(styleGuid); }

    enum class StyleBindMode : uint8_t
    {
        Global = 0, // affects entire UI tree
        Subtree     // affects only target subtree
    };

    // Register a UIStyle asset binding. The stylesheet itself is expected to already be attached
    // (globally or on the element). On AssetReloaded(UIStyle), the binding marks the appropriate
    // subtree dirty and bumps UIManager's stylesheet-content generation.
    void RegisterStyleBinding(const GUID& styleGuid, UIElement* target, StyleBindMode mode);

    // Remove the style binding for a specific (styleGuid, target) pair. Used when a
    // subtree's style asset is swapped at runtime so the previous style's binding
    // doesn't linger (and re-apply the old cascade on a later hot-edit). Scoped to
    // the pair so other styles attached to the same target are untouched.
    void UnregisterStyleBinding(const GUID& styleGuid, UIElement* target);

  private:
    void InstallAssetEventCallback();
    void UninstallAssetEventCallback();

    void ApplyLayoutReload(const GUID& layoutGuid);
    // Announce a layout this manager reloaded itself so every other UI manager
    // adopts it; this manager applies it directly and drops its own copy.
    void PublishLayoutReload(const GUID& layoutGuid);
    // Drop subtree bindings whose target was destroyed, releasing the stylesheet
    // snapshots they retained.
    void ReleaseDestroyedSubtreeBindings();
    void ApplyStyleReload(const GUID& styleGuid);

    // Track UIStyle @import dependencies so style changes can propagate to importing themes.
    void RefreshStyleImportGraphFor(const GUID& styleGuid);

  private:
    UIManager* m_Owner = nullptr;      // not owned
    AssetManager* m_Assets = nullptr;  // not owned
    uint32 m_AssetCallbackHandle = 0;  // AssetEventDispatcher callback handle

    std::mutex m_Mutex;
    std::vector<GUID> m_PendingLayouts;
    // AssetLoaded notifications for UIStyle assets. Pump uses these to apply styles only
    // once the asset (and its @import dependencies) are ready.
    std::vector<GUID> m_PendingStylesReady;
    // AssetReloaded notifications for UIStyle assets. These should apply immediately
    // (AssetManager already performed the reload).
    std::vector<GUID> m_PendingStylesReloaded;
    // AssetLoadFailed notifications for UIStyle assets (used to unblock dependency waits).
    std::vector<GUID> m_PendingStylesFailed;
    std::vector<GUID> m_PendingLayoutsModified;
    std::vector<GUID> m_PendingStylesModified;

    struct LayoutBinding
    {
        GUID layoutGuid;
        UIElement* target = nullptr;      // not owned; owned by UI tree
        uint64 targetInstanceId = 0;      // for debugging/sanity
        uint64 bindingId = 0;             // stamped onto template-owned nodes
        LayoutBindMode mode = LayoutBindMode::ReconcileSelf;
        // ReconcileChildren only: the asset-root classes this binding applied to the
        // target. Tracked so a layout SWAP removes the prior layout's root classes
        // (preserving runtime-added ones) instead of accumulating both. Empty for
        // ReconcileSelf, which diffs classes via ReconcileClasses.
        std::vector<std::string> appliedRootClasses;
    };

    // bindingId 0 is the reserved "unbound / code-added" sentinel: nodes with this id
    // were never stamped by a layout binding, so ReconcileChildren preserves them as
    // runtime-added. m_NextBindingId starts at 1 and only increments, so a real binding
    // can never collide with it.
    static constexpr uint64 kUnboundBindingId = 0;
    uint64 m_NextBindingId = 1;
    std::vector<LayoutBinding> m_LayoutBindings;

    struct StyleBinding
    {
        GUID styleGuid;
        // Detached dock/pool panels still adopt snapshots; destruction expires the target.
        UIElement::WeakRef<> target; // empty for Global
        StyleBindMode mode = StyleBindMode::Global;
        // Prior snapshots define this binding's slots in the flat attached list
        // and retain their rules until the target has adopted a new cascade.
        std::vector<StylesheetHandle> attachedCascade;
    };

    std::vector<StyleBinding> m_StyleBindings;

    // importer -> transitive imported guids
    std::unordered_map<GUID, std::vector<GUID>> m_StyleImports;
    // imported -> set of importers
    std::unordered_map<GUID, std::unordered_set<GUID>> m_StyleImportedBy;

    // Async hot-reload coordination (UI thread only; guarded by Pump())
    // Root styles awaiting "ready to apply" (asset loaded/reloaded and deps loaded).
    std::unordered_set<GUID> m_StyleApplyRequested;
    // Root style -> set of remaining dependency style GUIDs to load before applying.
    std::unordered_map<GUID, std::unordered_set<GUID>> m_StyleApplyWaitingDeps;

    // Best-effort dedupe for LoadAssetAsync requests (UI thread only; guarded by Pump()).
    std::unordered_set<GUID> m_StyleLoadInFlight;
};

} // namespace GameEngine


