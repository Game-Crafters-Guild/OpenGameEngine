#include "UI/UIHotReload.h"
#include "UI/Internal/AttachDetachInternal.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Logger/Logger.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/AccordionItem.h"
#include "UI/Controls/BaseField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UITemplateNode.h"
#include "UIAttributeAccess.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{

namespace
{

// UITemplateNode already stores attributes with lowercase keys, so just copy.
static std::unordered_map<std::string, std::string> BuildLowerAttrMap(const UITemplateNode& templ)
{
    return templ.Attributes;
}

static std::unordered_set<std::string> ParseClassList(const std::string& s)
{
    std::unordered_set<std::string> out;
    size_t start = 0;
    while (start < s.size())
    {
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
            ++start;
        if (start >= s.size())
            break;
        size_t end = start;
        while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])))
            ++end;
        if (end > start)
        {
            out.emplace(s.substr(start, end - start));
        }
        start = end;
    }
    return out;
}

// Forward declare; definition lives further below.
static UIElement* FindByInstanceId(UIElement* root, uint64 instanceId);

static bool ShouldPreserveAttributeOnExisting(const UIElement& existing, const std::string& attrLower)
{
    // Preserve user-editable runtime state by default for existing element instances.
    // This avoids hot reload resetting interactive state like text field contents or toggles.
    //
    // Convention: an attribute belongs in this list when XML is best read as
    // "initial state" while the runtime widget owns the "current state".
    // New controls that introduce persistent user-mutated state (expanded /
    // selected / active-tab / scroll-offset / etc.) must add their attribute
    // names here AND pair the change with a survives-reload test in
    // UIHotReloadTests.
    if (attrLower == "checked")
    {
        return dynamic_cast<const ToggleBase*>(&existing) != nullptr;
    }
    if (attrLower == "value")
    {
        // String fields (TextField/TextArea/TextInput) should keep their current contents.
        if (dynamic_cast<const BaseField*>(&existing) != nullptr)
            return true;
        // Sliders keep their current dragged value.
        if (dynamic_cast<const Slider*>(&existing) != nullptr)
            return true;
    }
    if (attrLower == "rangestart")
    {
        if (dynamic_cast<const Slider*>(&existing) != nullptr)
            return true;
    }
    if (attrLower == "expanded")
    {
        // Foldout / AccordionItem expand state is mutated at runtime via the
        // disclosure click; XML expanded= is the initial value, not a binding.
        if (dynamic_cast<const Foldout*>(&existing) != nullptr)
            return true;
        if (dynamic_cast<const AccordionItem*>(&existing) != nullptr)
            return true;
    }
    return false;
}

static std::unique_ptr<UIElement> CloneFromTemplate(const UITemplateNode& templ, uint64 bindingId)
{
    using namespace UIRegistration;

    ElementFactoryRegistry& reg = ElementFactoryRegistry::Instance();
    const std::string tagLower = reg.CanonicalTagLower(templ.TagName);
    std::unique_ptr<UIElement> out = reg.Create(tagLower);
    if (!out)
    {
        // An unregistered tag is still an identity: stamping it is what lets the element
        // reconcile against its own tag on the next reload instead of being rebuilt.
        out = std::make_unique<UIElement>();
        UIAttributeAccess::SetCreatedTag(*out, templ.TagName);
    }

    // Copy identity
    if (!templ.Id.empty())
        out->SetId(templ.Id);

    // Mark this element as originating from this template binding. Note: we intentionally
    // do NOT recursively stamp constructor-created internal children (e.g., Dropdown
    // header/items, Checkbox box/label). Those should be treated as runtime/internal
    // and preserved across template reloads.
    out->SetHotReloadBindingId(bindingId);

    // Copy authored attributes first (so ApplyAttributes sees them).
    // Template attributes already have lowercase keys.
    for (const auto& kv : templ.Attributes)
    {
        UIAttributeAccess::SetAuthoredAttribute(*out, kv.first, kv.second, /*markDirty=*/false);
        if (kv.first == "style" && kv.second.find(':') != std::string::npos)
            UIAttributeAccess::SetInlineStyleAttribute(*out, kv.second);
    }

    // Copy CSS classes
    for (const auto& cls : templ.Classes)
        out->AddClass(cls);

    // Re-apply attribute bindings to ensure derived controls receive their configured state.
    std::string innerText;
    if (const std::string* t = templ.FindAttribute("text"))
        innerText = *t;
    reg.ApplyAttributes(*out, templ.Attributes, innerText);

    // Recurse
    for (const auto& child : templ.Children)
    {
        out->AddChild(CloneFromTemplate(*child, bindingId));
    }

    return out;
}

static void ReconcileClasses(UIElement& existing, const UITemplateNode& templ)
{
    // Preserve runtime-added classes by diffing against the previously-applied template class list,
    // stored as the raw "class" attribute string.
    std::unordered_set<std::string> oldTemplate;
    if (const std::string* prev = UIAttributeAccess::FindAuthoredAttribute(existing, "class"))
    {
        oldTemplate = ParseClassList(*prev);
    }

    std::unordered_set<std::string> newTemplate;
    if (const std::string* cur = templ.FindAttribute("class"))
    {
        newTemplate = ParseClassList(*cur);
    }

    std::unordered_set<std::string> runtime;
    runtime.reserve(existing.GetClasses().size());
    for (const auto& cls : existing.GetClasses())
    {
        if (oldTemplate.find(cls) == oldTemplate.end())
            runtime.insert(cls);
    }

    // Desired = newTemplate + runtime
    std::unordered_set<std::string> desired = newTemplate;
    for (const auto& cls : runtime)
    {
        desired.insert(cls);
    }

    // Apply removals (classes present now but not desired)
    // Note: UIElement stores classes in an ordered set, so iteration is stable.
    std::vector<std::string> toRemove;
    for (const auto& cls : existing.GetClasses())
    {
        if (desired.find(cls) == desired.end())
            toRemove.push_back(cls);
    }
    for (const auto& cls : toRemove)
    {
        existing.RemoveClass(cls);
    }

    // Apply additions
    for (const auto& cls : desired)
    {
        if (!existing.HasClass(cls))
        {
            existing.AddClass(cls);
        }
    }
}

static bool ReconcileElement(UIElement& existing, const UITemplateNode& templ, uint64 bindingId);

static void ReconcileChildren(UIElement& parent, const UITemplateNode& templParent, uint64 bindingId)
{
    // Detach all existing children (preserves instance identity for potential reuse).
    std::vector<std::unique_ptr<UIElement>> detached;
    detached.reserve(parent.GetChildren().size());
    // The break below is what ends this loop during event dispatch, where
    // TakeChild detaches nothing and returns null.
    while (!parent.GetChildren().empty())
    {
        UIElement* ch = parent.GetChildren().front().get();
        if (auto owned = parent.TakeChild(ch))
        {
            detached.emplace_back(std::move(owned));
        }
        else
        {
            break;
        }
    }

    // Index by id for stable matching
    std::unordered_map<std::string, std::unique_ptr<UIElement>> byId;
    std::vector<std::unique_ptr<UIElement>> noId;
    byId.reserve(detached.size());
    noId.reserve(detached.size());
    for (auto& owned : detached)
    {
        if (!owned)
            continue;
        const std::string& id = owned->GetId();
        if (!id.empty())
        {
            byId.emplace(id, std::move(owned));
        }
        else
        {
            noId.emplace_back(std::move(owned));
        }
    }

    auto takeNoIdMatch = [&](const UITemplateNode& templChild) -> std::unique_ptr<UIElement>
    {
        // Prefer matching by concrete type + superset-of-template-classes. This preserves
        // control-internal children like Dropdown header/items even when they carry extra
        // runtime state classes (e.g., "open").
        using namespace UIRegistration;
        ElementFactoryRegistry& reg = ElementFactoryRegistry::Instance();
        const std::string templTag = reg.CanonicalTagLower(templChild.TagName);
        for (size_t i = 0; i < noId.size(); ++i)
        {
            if (!noId[i])
                continue;
            UIElement* cand = noId[i].get();
            if (!reg.IsSameType(*cand, templTag))
                continue;
            bool hasAll = true;
            for (const auto& cls : templChild.Classes)
            {
                if (!cand->HasClass(cls))
                {
                    hasAll = false;
                    break;
                }
            }
            if (!hasAll)
                continue;

            auto out = std::move(noId[i]);
            noId.erase(noId.begin() + static_cast<long long>(i));
            return out;
        }

        // Fallback: match by type only.
        for (size_t i = 0; i < noId.size(); ++i)
        {
            if (!noId[i])
                continue;
            UIElement* cand = noId[i].get();
            if (!reg.IsSameType(*cand, templTag))
                continue;
            auto out = std::move(noId[i]);
            noId.erase(noId.begin() + static_cast<long long>(i));
            return out;
        }
        return nullptr;
    };

    // Rebuild children in template order.
    for (const auto& templChPtr : templParent.Children)
    {
        if (!templChPtr)
            continue;
        const UITemplateNode& templChild = *templChPtr;

        std::unique_ptr<UIElement> use;
        if (!templChild.Id.empty())
        {
            auto it = byId.find(templChild.Id);
            if (it != byId.end())
            {
                use = std::move(it->second);
                byId.erase(it);
            }
        }
        else
        {
            use = takeNoIdMatch(templChild);
        }

        if (use)
        {
            using namespace UIRegistration;
            ElementFactoryRegistry& reg = ElementFactoryRegistry::Instance();
            const std::string templTag = reg.CanonicalTagLower(templChild.TagName);
            if (!reg.IsSameType(*use, templTag))
            {
                // Type changed under the same id/no-id slot; replace with new. The element
                // being replaced is DESTROYED right here, which is a destruction decision
                // like any other — announce it while it is still whole. The leftovers loop
                // at the end of this function cannot cover it: this one never reaches it.
                UI::DispatchDestructionDetach(use.get());
                use.reset();
            }
        }

        if (!use)
        {
            use = CloneFromTemplate(templChild, bindingId);
        }
        else
        {
            (void)ReconcileElement(*use, templChild, bindingId);
        }

        parent.AddChild(std::move(use));
    }

    // Preserve leftover runtime/code-added children (bindingId mismatch).
    auto keepIfRuntime = [&](std::unique_ptr<UIElement>& owned)
    {
        if (!owned)
            return false;
        return owned->GetHotReloadBindingId() != bindingId;
    };

    for (auto& kv : byId)
    {
        if (keepIfRuntime(kv.second))
        {
            parent.AddChild(std::move(kv.second));
        }
    }
    for (auto& owned : noId)
    {
        if (keepIfRuntime(owned))
        {
            parent.AddChild(std::move(owned));
        }
    }

    // Whatever is still held here is what the new template no longer wants: these unique_ptrs
    // release at the end of this scope and the elements are gone. That is a destruction
    // decision — the reconcile made it — so it is announced here, while they are still whole,
    // rather than silently in their destructors. Preserved children took the branches above
    // and are NOT in this set; they are re-added and the settle elides their round trip.
    for (auto& kv : byId)
    {
        if (kv.second)
            UI::DispatchDestructionDetach(kv.second.get());
    }
    for (auto& owned : noId)
    {
        if (owned)
            UI::DispatchDestructionDetach(owned.get());
    }
}

static bool ReconcileElement(UIElement& existing, const UITemplateNode& templ, uint64 bindingId)
{
    using namespace UIRegistration;
    ElementFactoryRegistry& reg = ElementFactoryRegistry::Instance();
    const std::string templTag = reg.CanonicalTagLower(templ.TagName);
    if (!reg.IsSameType(existing, templTag))
        return false;

    existing.SetHotReloadBindingId(bindingId);

    // Best-effort: if existing lacked an id but template provides one, adopt it so focus/state can persist by id.
    if (existing.GetId().empty() && !templ.Id.empty())
    {
        existing.SetId(templ.Id);
    }

    // Update classes while preserving runtime additions.
    ReconcileClasses(existing, templ);

    // Update raw attributes (preserving some runtime state attributes).
    // Template attributes already have lowercase keys.
    for (const auto& kv : templ.Attributes)
    {
        if (kv.first == "id")
            continue;
        if (kv.first == "class")
        {
            // Keep raw class string updated so future diffs preserve runtime-added classes.
            UIAttributeAccess::SetAuthoredAttribute(existing, kv.first, kv.second, /*markDirty=*/false);
            continue;
        }
        if (kv.first == "style" && kv.second.find(':') != std::string::npos)
        {
            UIAttributeAccess::SetInlineStyleAttribute(existing, kv.second);
        }

        if (ShouldPreserveAttributeOnExisting(existing, kv.first))
        {
            continue;
        }

        UIAttributeAccess::SetAuthoredAttribute(existing, kv.first, kv.second, /*markDirty=*/true);
    }

    // Re-apply attribute bindings to update derived control state, but skip stateful attributes.
    auto attrsLower = BuildLowerAttrMap(templ);
    for (auto it = attrsLower.begin(); it != attrsLower.end();)
    {
        if (ShouldPreserveAttributeOnExisting(existing, it->first))
        {
            it = attrsLower.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::string innerText;
    if (const std::string* t = templ.FindAttribute("text"))
        innerText = *t;
    reg.ApplyAttributes(existing, attrsLower, innerText);

    // Reconcile children.
    ReconcileChildren(existing, templ, bindingId);
    return true;
}

static UIElement* FindByInstanceId(UIElement* root, uint64 instanceId)
{
    if (!root || instanceId == 0)
        return nullptr;
    if (root->GetInstanceId() == instanceId)
        return root;
    for (const auto& ch : root->GetChildren())
    {
        if (auto* found = FindByInstanceId(ch.get(), instanceId))
        {
            return found;
        }
    }

    // Mount targets are not part of the child list but must still participate in
    // hot reload target discovery (Dockspace mounts panels via Mount).
    if (auto* m = dynamic_cast<Mount*>(root))
    {
        if (UIElement* tgt = m->GetTarget())
        {
            if (auto* found = FindByInstanceId(tgt, instanceId))
            {
                return found;
            }
        }
    }

    return nullptr;
}

void DispatchLayoutReconciled(UIElement& target)
{
    UIEvent e{};
    e.Id = kEventLayoutReconciled;
    e.Target = &target;
    e.CurrentTarget = &target;
    target.DispatchEvent<kEventLayoutReconciled>(e);
}

} // namespace

UIHotReload::UIHotReload(UIManager& owner, AssetManager& assets)
    : m_Owner(&owner), m_Assets(&assets)
{
    InstallAssetEventCallback();
}

UIHotReload::~UIHotReload()
{
    UninstallAssetEventCallback();
}

void UIHotReload::InstallAssetEventCallback()
{
    if (!m_Assets || m_AssetCallbackHandle != 0)
    {
        return;
    }

    // Keep the callback strictly queue-only; do not call back into AssetManager here.
    m_AssetCallbackHandle = m_Assets->GetEventDispatcher().AddCallback(
        [this](const AssetEvent& e)
        {
            if (e.Type != AssetType::UILayout && e.Type != AssetType::UIStyle)
            {
                return;
            }

            std::lock_guard<std::mutex> lk(m_Mutex);
            if (e.EventType == AssetEventType::AssetModified)
            {
                if (e.Type == AssetType::UILayout)
                {
                    m_PendingLayoutsModified.push_back(e.AssetGuid);
                }
                else
                {
                    m_PendingStylesModified.push_back(e.AssetGuid);
                }
            }
            else if (e.EventType == AssetEventType::AssetReloaded)
            {
                if (e.Type == AssetType::UILayout)
                {
                    m_PendingLayouts.push_back(e.AssetGuid);
                }
                else
                {
                    m_PendingStylesReloaded.push_back(e.AssetGuid);
                }
            }
            else if (e.EventType == AssetEventType::AssetLoaded)
            {
                if (e.Type == AssetType::UIStyle)
                {
                    m_PendingStylesReady.push_back(e.AssetGuid);
                }
            }
            else if (e.EventType == AssetEventType::AssetLoadFailed)
            {
                if (e.Type == AssetType::UIStyle)
                {
                    m_PendingStylesFailed.push_back(e.AssetGuid);
                }
            }
        });
}

void UIHotReload::UninstallAssetEventCallback()
{
    if (!m_Assets || m_AssetCallbackHandle == 0)
    {
        return;
    }

    m_Assets->GetEventDispatcher().RemoveCallback(m_AssetCallbackHandle);
    m_AssetCallbackHandle = 0;
}

void UIHotReload::ReleaseDestroyedSubtreeBindings()
{
    m_StyleBindings.erase(std::remove_if(m_StyleBindings.begin(), m_StyleBindings.end(),
        [](const StyleBinding& binding)
        { return binding.mode == StyleBindMode::Subtree && !binding.target.Get(); }), m_StyleBindings.end());
}

void UIHotReload::Pump()
{
    std::vector<GUID> layouts;
    std::vector<GUID> stylesReady;
    std::vector<GUID> stylesReloaded;
    std::vector<GUID> stylesFailed;
    std::vector<GUID> layoutsModified;
    std::vector<GUID> stylesModified;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        layouts.swap(m_PendingLayouts);
        stylesReady.swap(m_PendingStylesReady);
        stylesReloaded.swap(m_PendingStylesReloaded);
        stylesFailed.swap(m_PendingStylesFailed);
        layoutsModified.swap(m_PendingLayoutsModified);
        stylesModified.swap(m_PendingStylesModified);
    }

    // Coalesce duplicates to avoid repeated Apply calls when file watchers emit
    // multiple reload events for the same asset in quick succession.
    auto dedupe = [](std::vector<GUID>& v)
    {
        if (v.size() <= 1)
            return;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    // A destroyed panel's binding still holds its prior snapshots. Bindings are
    // only read when a style event arrives, so release them then (and on
    // registration) rather than scanning every frame: a closed panel's rules
    // outlive it until the next style event, and no longer.
    if (!stylesReady.empty() || !stylesReloaded.empty() || !stylesFailed.empty() || !stylesModified.empty())
        ReleaseDestroyedSubtreeBindings();
    dedupe(layouts);
    dedupe(stylesReady);
    dedupe(stylesReloaded);
    dedupe(stylesFailed);
    dedupe(layoutsModified);
    dedupe(stylesModified);

    // Process reloaded events first; then handle modified events that haven't already been
    // satisfied by a reload. This avoids double-reloading when both events are queued.
    std::unordered_set<GUID> processedLayouts;
    processedLayouts.reserve(layouts.size() + layoutsModified.size());
    for (const GUID& g : layouts)
    {
        if (!processedLayouts.insert(g).second)
            continue;
        ApplyLayoutReload(g);
    }
    for (const GUID& g : layoutsModified)
    {
        if (!processedLayouts.insert(g).second)
            continue;
        if (m_Assets)
        {
            if (m_Assets->ReloadAssetNow(g) == ReloadOutcome::Reloaded)
                PublishLayoutReload(g);
        }
        ApplyLayoutReload(g);
    }

    // Styles: never block the UI thread. We request loads and only apply once the
    // root asset and all of its @import dependencies are loaded (best-effort).
    if (m_Assets)
    {
        auto requestLoad = [&](const GUID& g)
        {
            if (g.IsNull())
                return;
            if (m_Assets->IsAssetLoaded(g))
            {
                // The load completed between the caller's check and this request
                // (already-loaded loads resolve without dispatching AssetLoaded),
                // so no event will clear the marker. Drop it here so a later
                // modify can request a fresh load.
                m_StyleLoadInFlight.erase(g);
                return;
            }
            if (m_StyleLoadInFlight.find(g) != m_StyleLoadInFlight.end())
                return;
            m_StyleLoadInFlight.insert(g);
            // Best-effort fire-and-forget; completion is observed via AssetLoaded/AssetLoadFailed.
            (void)m_Assets->LoadAssetAsync(g, AssetLoadPriority::High);
        };

        auto tryFinalizeApply = [&](const GUID& rootGuid) -> bool
        {
            if (rootGuid.IsNull())
                return false;
            if (m_StyleApplyRequested.find(rootGuid) == m_StyleApplyRequested.end())
                return false;

            auto asset = m_Assets->GetAsset(rootGuid);
            if (!asset || asset->GetType() != AssetType::UIStyle)
                return false;
            auto* style = static_cast<UIStyleAsset*>(asset.get());
            if (!style || !style->IsLoaded() || style->HasFailed())
                return false;

            std::unordered_set<GUID> missing;
            const auto& deps = style->GetImportedStyleGuids();
            missing.reserve(deps.size());
            for (const GUID& dep : deps)
            {
                if (dep.IsNull() || dep == rootGuid)
                    continue;
                if (!m_Assets->IsAssetLoaded(dep))
                {
                    missing.insert(dep);
                    requestLoad(dep);
                }
            }

            if (!missing.empty())
            {
                m_StyleApplyWaitingDeps[rootGuid] = std::move(missing);
                return false;
            }

            m_StyleApplyWaitingDeps.erase(rootGuid);
            m_StyleApplyRequested.erase(rootGuid);
            m_StyleLoadInFlight.erase(rootGuid);
            ApplyStyleReload(rootGuid);
            return true;
        };

        // AssetManager commits loaded styles once and publishes AssetReloaded.
        // Each UI manager only adopts that snapshot; another observer must not
        // reload the same shared asset while this manager is using its rules.
        // Unloaded styles still need a load before they can be applied.
        for (const GUID& g : stylesModified)
        {
            if (g.IsNull())
                continue;
            if (m_Assets->IsAssetLoaded(g))
            {
                continue;
            }
            m_StyleApplyRequested.insert(g);
            requestLoad(g);
        }

        // Resolve any roots waiting on these dependency completions (success or failure).
        auto onDepComplete = [&](const GUID& depGuid)
        {
            if (depGuid.IsNull())
                return;
            std::vector<GUID> rootsToTry;
            rootsToTry.reserve(m_StyleApplyWaitingDeps.size());
            for (auto& kv : m_StyleApplyWaitingDeps)
            {
                auto& waiting = kv.second;
                waiting.erase(depGuid);
                if (waiting.empty())
                    rootsToTry.push_back(kv.first);
            }
            for (const GUID& root : rootsToTry)
                (void)tryFinalizeApply(root);
        };

        // AssetManager already performed the reload; apply immediately so UI updates even if
        // AssetModified events are missing/coalesced by the host watcher.
        for (const GUID& g : stylesReloaded)
        {
            m_StyleLoadInFlight.erase(g);
            onDepComplete(g);
            m_StyleApplyRequested.insert(g);
            (void)tryFinalizeApply(g);
        }

        for (const GUID& g : stylesReady)
        {
            m_StyleLoadInFlight.erase(g);
            // A loaded/reloaded style may be a root or a dependency.
            onDepComplete(g);
            (void)tryFinalizeApply(g);
        }
        for (const GUID& g : stylesFailed)
        {
            m_StyleLoadInFlight.erase(g);
            // Treat failures as "completed" so roots aren't stuck forever.
            onDepComplete(g);
            // If this was itself a root request, drop it to avoid spinning.
            m_StyleApplyWaitingDeps.erase(g);
            m_StyleApplyRequested.erase(g);
        }

        // Self-heal check-then-act races: a root or dependency can finish loading
        // between an IsAssetLoaded check and the matching LoadAssetAsync request
        // (already-loaded loads resolve their future without dispatching
        // AssetLoaded), in which case no event will ever drain the pending apply.
        // Retry pending roots against current load state every pump so a lost
        // notification delays an apply by one frame instead of wedging it.
        // tryFinalizeApply recomputes the missing-dependency set from scratch, so
        // this also reconciles m_StyleApplyWaitingDeps.
        if (!m_StyleApplyRequested.empty())
        {
            const std::vector<GUID> pendingRoots(m_StyleApplyRequested.begin(), m_StyleApplyRequested.end());
            for (const GUID& root : pendingRoots)
            {
                (void)tryFinalizeApply(root);
            }
        }
    }
}

uint64 UIHotReload::RegisterLayoutBinding(const GUID& layoutGuid, UIElement* target, LayoutBindMode mode)
{
    if (!target)
    {
        return 0;
    }

    // Replace any prior binding for this target (layout roots can be reloaded/replaced).
    // Reuse the prior binding's id so the binding identity is STABLE across a layout
    // SWAP: ReconcileChildren stamps every child it creates with the binding id and
    // preserves only children whose id differs (treating them as runtime/code-added).
    // If a swap minted a fresh id, the OLD layout's children — stamped with the prior
    // id — would look runtime-added and get re-attached, leaving stale content behind.
    // Keeping the id stable makes those leftovers binding-owned (dropped when absent
    // from the new layout) while genuine code-added children (id 0) stay preserved.
    uint64 reuseBindingId = kUnboundBindingId;
    std::vector<std::string> carriedRootClasses;
    for (const auto& existing : m_LayoutBindings)
    {
        if (existing.target == target)
        {
            reuseBindingId = existing.bindingId;
            // Carry the prior layout's applied root classes so ApplyLayoutReload can
            // remove them when binding the new layout (else they accumulate on a swap).
            carriedRootClasses = existing.appliedRootClasses;
            break;
        }
    }
    m_LayoutBindings.erase(
        std::remove_if(m_LayoutBindings.begin(), m_LayoutBindings.end(),
                       [target](const LayoutBinding& b)
                       { return b.target == target; }),
        m_LayoutBindings.end());

    LayoutBinding b{};
    b.layoutGuid = layoutGuid;
    b.target = target;
    b.targetInstanceId = target->GetInstanceId();
    b.bindingId = reuseBindingId != kUnboundBindingId ? reuseBindingId : m_NextBindingId++;
    b.appliedRootClasses = std::move(carriedRootClasses);
    b.mode = mode;

    m_LayoutBindings.push_back(b);
    return b.bindingId;
}

void UIHotReload::PublishLayoutReload(const GUID& layoutGuid)
{
    const auto asset = m_Assets->GetAsset(layoutGuid);
    if (!asset)
        return;
    m_Assets->GetEventDispatcher().DispatchEvent(
        AssetEvents::AssetReloaded(layoutGuid, asset->GetType(), asset->GetPath().string()));
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_PendingLayouts.erase(std::remove(m_PendingLayouts.begin(), m_PendingLayouts.end(), layoutGuid),
                           m_PendingLayouts.end());
}

void UIHotReload::ApplyLayoutReload(const GUID& layoutGuid)
{
    if (!m_Assets)
        return;

    // Find bindings first (fast reject) to avoid hitting AssetManager unnecessarily.
    bool any = false;
    for (const auto& b : m_LayoutBindings)
    {
        if (b.layoutGuid == layoutGuid)
        {
            any = true;
            break;
        }
    }
    if (!any)
        return;

    auto asset = m_Assets->GetAsset(layoutGuid);
    if (!asset || asset->GetType() != AssetType::UILayout)
    {
        return;
    }
    auto* layout = static_cast<UILayoutAsset*>(asset.get());
    UITemplateNode* templRoot = layout->GetRoot();
    if (!templRoot)
    {
        return;
    }

    UIElement* liveRoot = m_Owner ? m_Owner->GetRootElement() : nullptr;

    // Reconciling destroys and re-clones every template-owned element the new
    // template no longer matches, so each reconciled target is told, with
    // kEventLayoutReconciled, once the whole pass is done. Held weakly: a handler of
    // one target's event may destroy another target before its turn.
    std::vector<UIElement::WeakRef<>> reconciledTargets;

    // Apply to all targets bound to this layout.
    for (auto it = m_LayoutBindings.begin(); it != m_LayoutBindings.end();)
    {
        LayoutBinding& b = *it;
        if (b.layoutGuid != layoutGuid)
        {
            ++it;
            continue;
        }

        // Resolve the live target by instanceId to avoid holding stale pointers if the UI
        // tree was replaced (e.g., LoadLayoutFromAsset called again).
        UIElement* target = FindByInstanceId(liveRoot, b.targetInstanceId);
        if (!target)
        {
            // Target may be temporarily unreachable from the root (e.g., a dock panel
            // not currently mounted). Keep the binding so it can apply when the
            // subtree becomes active again.
            ++it;
            continue;
        }
        b.target = target; // cache

        if (b.mode == LayoutBindMode::ReconcileSelf)
        {
            if (!ReconcileElement(*target, *templRoot, b.bindingId))
            {
                // Fallback: if we cannot reconcile the root element, skip. A later step will
                // optionally support full root replacement via UIManager::SetRoot.
                Logger::Log::Warning("UIHotReload: Layout reload skipped for target (type mismatch) guid={}", layoutGuid.ToString());
            }
            else
            {
                reconciledTargets.push_back(UIElement::MakeWeakRef(target));
            }
        }
        else
        {
            // Custom element target: reconcile only its children against the asset root's children.
            // Historically we did not apply the asset root's own attributes/classes to the custom element,
            // but this makes UXML root classes ineffective for panels that bind "children-only".
            // Apply template root classes (only) so CSS selectors like `.sceneview-panel` work.
            {
                // Remove the classes the PREVIOUS layout applied (tracked per binding), so a
                // layout swap replaces them instead of accumulating both layouts' root classes.
                // Runtime-added classes were never recorded here, so they survive.
                for (const auto& cls : b.appliedRootClasses)
                {
                    if (!cls.empty())
                        target->RemoveClass(cls);
                }
                b.appliedRootClasses.clear();
                // Add this layout's root classes and record them for the next swap.
                for (const auto& cls : templRoot->Classes)
                {
                    if (!cls.empty())
                    {
                        target->AddClass(cls);
                        b.appliedRootClasses.push_back(cls);
                    }
                }
            }
            ReconcileChildren(*target, *templRoot, b.bindingId);
            reconciledTargets.push_back(UIElement::MakeWeakRef(target));
        }
        ++it;
    }

    // After large tree mutations, hover/capture pointers may be stale. Clear hover/capture
    // state conservatively; focus is preserved by id.
    if (m_Owner)
        m_Owner->ClearHover();

    // Announced after the binding walk, never inside it: a handler that binds a layout
    // (RegisterLayoutBinding) would otherwise reallocate m_LayoutBindings under the loop.
    for (const UIElement::WeakRef<>& reconciled : reconciledTargets)
    {
        if (UIElement* target = reconciled.Get())
            DispatchLayoutReconciled(*target);
    }
}

void UIHotReload::UnregisterStyleBinding(const GUID& styleGuid, UIElement* target)
{
    m_StyleBindings.erase(
        std::remove_if(m_StyleBindings.begin(), m_StyleBindings.end(),
                       [&](const StyleBinding& b) { return b.styleGuid == styleGuid && b.target.Get() == target; }),
        m_StyleBindings.end());
}

void UIHotReload::RegisterStyleBinding(const GUID& styleGuid, UIElement* target, StyleBindMode mode)
{
    ReleaseDestroyedSubtreeBindings();
    // Idempotent registration: keep a single entry per (guid,target,mode).
    for (const auto& existing : m_StyleBindings)
    {
        if (existing.styleGuid == styleGuid && existing.target.Get() == target && existing.mode == mode)
        {
            return;
        }
    }

    StyleBinding b{};
    b.styleGuid = styleGuid;
    b.target = UIElement::MakeWeakRef(target);
    b.mode = mode;
    m_StyleBindings.push_back(b);

    RefreshStyleImportGraphFor(styleGuid);

    // Snapshot the currently attached cascade order for this binding. This lets us
    // rebuild the block if the importer changes its @import ordering later.
    if (m_Assets)
    {
        auto asset = m_Assets->GetAsset(styleGuid);
        if (asset && asset->GetType() == AssetType::UIStyle)
        {
            auto* style = static_cast<UIStyleAsset*>(asset.get());
            if (style && style->IsLoaded() && !style->HasFailed())
            {
                auto cascade = style->GetCascadeHandles(*m_Assets);
                if (cascade.empty())
                    cascade.push_back(style->GetStylesheetHandle());
                m_StyleBindings.back().attachedCascade = std::move(cascade);
            }
        }
    }
}

void UIHotReload::RefreshStyleImportGraphFor(const GUID& styleGuid)
{
    if (!m_Assets || styleGuid.IsNull())
        return;

    // Only track imports for UIStyle assets that are actually loaded.
    auto asset = m_Assets->GetAsset(styleGuid);
    if (!asset || asset->GetType() != AssetType::UIStyle)
        return;

    auto* style = static_cast<UIStyleAsset*>(asset.get());
    const std::vector<GUID>& deps = style->GetImportedStyleGuids();

    // Remove old reverse edges for this importer.
    auto itOld = m_StyleImports.find(styleGuid);
    if (itOld != m_StyleImports.end())
    {
        for (const GUID& dep : itOld->second)
        {
            auto itRev = m_StyleImportedBy.find(dep);
            if (itRev != m_StyleImportedBy.end())
            {
                itRev->second.erase(styleGuid);
                if (itRev->second.empty())
                    m_StyleImportedBy.erase(itRev);
            }
        }
    }

    // Store new forward + reverse edges (deduped).
    std::unordered_set<GUID> unique;
    unique.reserve(deps.size());
    std::vector<GUID> forward;
    forward.reserve(deps.size());

    for (const GUID& g : deps)
    {
        if (g.IsNull() || g == styleGuid)
            continue;
        if (!unique.insert(g).second)
            continue;
        forward.push_back(g);
        m_StyleImportedBy[g].insert(styleGuid);

        // Ensure imported style assets are loaded so AssetManager hot-reload emits events.
        m_Assets->LoadAsset(g, AssetLoadResultCallback{}, AssetLoadPriority::Low);
    }

    m_StyleImports[styleGuid] = std::move(forward);
}

void UIHotReload::ApplyStyleReload(const GUID& styleGuid)
{
    // Defensive: ignore null style GUIDs or calls when UI/Asset systems are unavailable.
    if (!m_Owner || !m_Assets || styleGuid.IsNull())
        return;

    // Find any relevant bindings for this style guid.
    bool anyDirect = false;
    for (const auto& b : m_StyleBindings)
    {
        if (b.styleGuid == styleGuid)
        {
            anyDirect = true;
            break;
        }
    }

    // Even if this style isn't directly bound, it may be imported by bound themes.
    bool anyImporters = (m_StyleImportedBy.find(styleGuid) != m_StyleImportedBy.end());
    if (!anyDirect && !anyImporters)
    {
        // Defensive self-heal: dependency GUIDs may have changed (source rebind/remap)
        // while this graph was built from older GUIDs. Refresh all bound style graphs
        // and retry importer lookup once before giving up.
        for (const auto& b : m_StyleBindings)
        {
            if (b.styleGuid.IsNull())
                continue;
            RefreshStyleImportGraphFor(b.styleGuid);
        }
        anyImporters = (m_StyleImportedBy.find(styleGuid) != m_StyleImportedBy.end());
        if (!anyImporters)
            return;
    }

    // Gather transitive importers and mark all impacted bindings dirty so CSS is recomputed.
    // With the @import segment model, we no longer need to reload importers on leaf changes.
    std::unordered_set<GUID> visited;
    visited.reserve(16);

    std::vector<GUID> queue;
    queue.reserve(8);
    queue.push_back(styleGuid);
    visited.insert(styleGuid);

    while (!queue.empty())
    {
        const GUID cur = queue.back();
        queue.pop_back();

        auto itRev = m_StyleImportedBy.find(cur);
        if (itRev == m_StyleImportedBy.end())
            continue;

        for (const GUID& importer : itRev->second)
        {
            if (!visited.insert(importer).second)
                continue;
            queue.push_back(importer);
        }
    }

    // The changed asset may have acquired new imports even when it is only
    // indirectly bound. Keep those edges so later nested edits reach its roots.
    RefreshStyleImportGraphFor(styleGuid);
    // Rebuild once per attachment target. Different bindings can share an
    // imported sheet, whose established position must survive every reload.
    std::vector<UIElement*> targets;
    bool includeGlobal = false;
    for (const auto& binding : m_StyleBindings)
    {
        if (visited.find(binding.styleGuid) == visited.end())
            continue;
        if (binding.mode == StyleBindMode::Global)
        {
            includeGlobal = true;
            continue;
        }
        UIElement* target = binding.target.Get();
        if (target && (!target->GetOwnerManager() || target->GetOwnerManager() == m_Owner) &&
            std::find(targets.begin(), targets.end(), target) == targets.end())
            targets.push_back(target);
    }
    if (includeGlobal)
        targets.push_back(nullptr);

    bool changed = false;
    for (UIElement* target : targets)
    {
        const auto& attached = target ? target->GetStylesheets() : m_Owner->GetStylesheets();
        struct Adoption
        {
            StyleBinding* binding;
            std::vector<StylesheetHandle> cascade;
            size_t anchor;
        };
        std::vector<Adoption> adoptions;
        std::vector<int> slotOwners(attached.size(), -1);
        bool hasNewSnapshot = false;
        for (auto& binding : m_StyleBindings)
        {
            if (target ? binding.mode != StyleBindMode::Subtree || binding.target.Get() != target
                       : binding.mode != StyleBindMode::Global)
                continue;
            auto cascade = binding.attachedCascade;
            auto asset = m_Assets->GetAsset(binding.styleGuid);
            if (asset && asset->GetType() == AssetType::UIStyle && asset->IsLoaded() && !asset->HasFailed())
            {
                auto* style = static_cast<UIStyleAsset*>(asset.get());
                cascade = style->GetCascadeHandles(*m_Assets);
                if (cascade.empty())
                    cascade.push_back(style->GetStylesheetHandle());
            }
            hasNewSnapshot |= cascade != binding.attachedCascade;
            const int owner = static_cast<int>(adoptions.size());
            size_t anchor = attached.size();
            for (size_t i = 0; i < attached.size(); ++i)
            {
                if (slotOwners[i] == -1 &&
                    std::find(binding.attachedCascade.begin(), binding.attachedCascade.end(), attached[i]) !=
                        binding.attachedCascade.end())
                {
                    slotOwners[i] = owner;
                    anchor = std::min(anchor, i);
                }
            }
            adoptions.push_back({&binding, std::move(cascade), anchor});
        }
        if (!hasNewSnapshot)
            continue;

        std::vector<StylesheetHandle> replacement;
        std::unordered_set<const Stylesheet*> seen;
        const auto append = [&](const StylesheetHandle& sheet)
        {
            if (sheet && seen.insert(sheet.get()).second)
                replacement.push_back(sheet);
        };
        for (size_t i = 0; i < attached.size(); ++i)
        {
            const int owner = slotOwners[i];
            if (owner == -1)
                append(attached[i]); // Preserve unrelated runtime overrides in place.
            else if (adoptions[owner].anchor == i)
                for (const auto& sheet : adoptions[owner].cascade)
                    append(sheet);
        }
        for (const auto& adoption : adoptions)
            if (adoption.anchor == attached.size())
                for (const auto& sheet : adoption.cascade)
                    append(sheet);

        std::vector<const Stylesheet*> oldBlock;
        oldBlock.reserve(attached.size());
        for (const auto& sheet : attached)
            oldBlock.push_back(sheet.get());
        if (target)
            target->ReplaceStylesheetBlock(oldBlock, replacement);
        else
            m_Owner->ReplaceGlobalStylesheetBlock(oldBlock, replacement);
        for (auto& adoption : adoptions)
            adoption.binding->attachedCascade = std::move(adoption.cascade);
        changed = true;
    }
    if (changed)
        m_Owner->NotifyStylesheetContentChanged();
}

} // namespace GameEngine
