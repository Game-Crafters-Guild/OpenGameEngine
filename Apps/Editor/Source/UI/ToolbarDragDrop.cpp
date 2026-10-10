#include "UI/ToolbarDragDrop.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/UIEvents.h"
#include "UI/UIElement.h"
#include "UI/Controls/Button.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

namespace
{
    // Marks a button whose drag handlers are registered. Setup re-runs on every layout
    // pass, so the membership test is by precomputed id: HasClass(const std::string&)
    // would build a past-SSO temporary per button per pass just to hash it.
    constexpr const char* kInitializedClass = "toolbar-dragdrop-initialized";
    constexpr StringId kInitializedClassId = HashStringId(kInitializedClass);

    UIElement* FindContainerByClass(UIElement* el, StringId classId)
    {
        if (!el) return nullptr;
        if (el->HasClass(classId)) return el;
        for (const auto& child : el->GetChildren())
            if (auto* found = FindContainerByClass(child.get(), classId))
                return found;
        return nullptr;
    }
} // namespace

ToolbarDragDrop::ToolbarDragDrop() = default;
ToolbarDragDrop::~ToolbarDragDrop() = default;

void ToolbarDragDrop::Setup(UIElement* ghostRoot, const std::vector<ContainerConfig>& containers,
                            UIElement* searchRoot)
{
    if (!ghostRoot) return;
    if (!searchRoot) searchRoot = ghostRoot;

    // Re-resolve the container elements every pass — async UXML mounts them late — but
    // assign into the existing rows rather than clearing: std::string assignment reuses
    // its buffer, so a settled pass allocates nothing.
    m_Containers.resize(containers.size());
    for (size_t i = 0; i < containers.size(); ++i)
    {
        ResolvedContainer& rc = m_Containers[i];
        rc.ClassId = HashStringId(containers[i].cssClass);
        rc.SettingsKey = containers[i].settingsKey;
        UIElement* const element = FindContainerByClass(searchRoot, rc.ClassId);
        rc.Element = UIElement::MakeWeakRef(element);
        UIElement* zone = nullptr;
        if (!containers[i].dropZoneCssClass.empty())
            zone = FindContainerByClass(searchRoot, HashStringId(containers[i].dropZoneCssClass));
        rc.DropZone = UIElement::MakeWeakRef(zone ? zone : element);
    }

    // The handlers below read m_Containers through `this` instead of capturing the
    // caller's config by value; a copy per handler is what made a re-Setup allocate.
    auto setupButtonDrag = [this, ghostRoot](Button* btn)
    {
        if (!btn) return;
        if (btn->HasClass(kInitializedClassId)) return;
        btn->AddClass(kInitializedClass);

        btn->RegisterEventHandler(kEventMouseDown, [this, btn](UIEvent& e)
        {
            if (e.Button != 0) return;

            UIElement* container = btn->GetParent();
            if (!container) return;

            // Accept only buttons whose parent is one of our registered containers.
            bool inKnownContainer = false;
            for (const auto& rc : m_Containers)
                if (container->HasClass(rc.ClassId))
                    { inKnownContainer = true; break; }
            if (!inKnownContainer) return;

            m_DraggedToolbarButton   = btn;
            m_DraggedButtonContainer = UIElement::MakeWeakRef(container);
            m_TargetButtonContainer  = m_DraggedButtonContainer;
            m_DragStartX             = e.X;
            m_DragStartY             = e.Y;
            m_DragActive             = false;
            m_CurrentDropIndex       = -1;
            m_DragOrderChanged       = false;

            e.Capture(btn);
            e.Stop();
        });

        btn->RegisterEventHandler(kEventMouseMove, [this, btn, ghostRoot](UIEvent& e)
        {
            if (m_DraggedToolbarButton != btn || !m_DraggedButtonContainer.Get()) return;

            constexpr float kDragThreshold = 5.0f;
            if (!m_DragActive)
            {
                const float dx = e.X - m_DragStartX;
                const float dy = e.Y - m_DragStartY;
                if (std::sqrt(dx * dx + dy * dy) < kDragThreshold) return;

                m_DragActive = true;
                btn->AddClass("dragging");

                const float btnX = btn->GetLayoutX();
                const float btnY = btn->GetLayoutY();
                const float btnW = btn->GetLayoutWidth();
                const float btnH = btn->GetLayoutHeight();

                auto ghost = std::make_unique<Button>();
                ghost->SetId("toolbar-drag-ghost");
                for (const auto& cls : btn->GetClasses())
                    if (cls != "dragging") ghost->AddClass(cls);
                ghost->AddClass("drag-ghost");
                ghost->Overrides()
                    .Set(Style::Position,      PositionType::Absolute)
                    .Set(Style::PositionLeft,  StyleLength::Px(btnX))
                    .Set(Style::PositionTop,   StyleLength::Px(btnY))
                    .Set(Style::Width,         StyleLength::Px(btnW))
                    .Set(Style::Height,        StyleLength::Px(btnH))
                    .Set(Style::ZIndex,        10000)
                    .Set(Style::PointerEvents, false);
                m_DragGhostElement = ghost.get();
                ghostRoot->AddChild(std::move(ghost));

                auto indicator = std::make_unique<UIElement>();
                indicator->SetId("toolbar-insertion-indicator");
                indicator->AddClass("toolbar-insertion-indicator");
                indicator->Overrides()
                    .Set(Style::Position,      PositionType::Absolute)
                    .Set(Style::Display,       DisplayMode::None)
                    .Set(Style::PointerEvents, false);
                m_InsertionIndicator = indicator.get();
                ghostRoot->AddChild(std::move(indicator));
            }

            if (!m_DragActive) return;

            const float btnW = btn->GetLayoutWidth();
            const float btnH = btn->GetLayoutHeight();

            if (m_DragGhostElement)
            {
                m_DragGhostElement->Overrides()
                    .Set(Style::Position,      PositionType::Absolute)
                    .Set(Style::PositionLeft,  StyleLength::Px(e.X - btnW * 0.5f))
                    .Set(Style::PositionTop,   StyleLength::Px(e.Y - btnH * 0.5f))
                    .Set(Style::Width,         StyleLength::Px(btnW))
                    .Set(Style::Height,        StyleLength::Px(btnH))
                    .Set(Style::ZIndex,        10000)
                    .Set(Style::PointerEvents, false);
            }

            // Hit-test each container's drop zone, which is the container itself
            // unless a wider one was configured.
            UIElement* targetContainer = nullptr;
            for (const auto& rc : m_Containers)
            {
                UIElement* c = rc.Element.Get();
                UIElement* zone = rc.DropZone.Get();
                if (!c || !zone) continue;
                constexpr float kHitPad = 3.0f;
                const float cx = zone->GetLayoutX();
                const float cy = zone->GetLayoutY();
                const float cw = zone->GetLayoutWidth();
                const float ch = zone->GetLayoutHeight();
                if (e.X >= cx - kHitPad && e.X < cx + cw + kHitPad &&
                    e.Y >= cy - kHitPad && e.Y < cy + ch + kHitPad)
                {
                    targetContainer = c;
                    break;
                }
            }

            if (!targetContainer)
            {
                if (m_InsertionIndicator)
                    m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
                m_TargetButtonContainer = {};
                m_CurrentDropIndex = -1;
                e.Stop();
                return;
            }

            m_TargetButtonContainer = UIElement::MakeWeakRef(targetContainer);

            // Compute drop index from mouse X vs. sibling centers.
            const auto& targetChildren = targetContainer->GetChildren();
            std::vector<UIElement*> others;
            for (const auto& child : targetChildren)
                if (child.get() != btn) others.push_back(child.get());
            std::sort(others.begin(), others.end(),
                [](UIElement* a, UIElement* b){ return a->GetLayoutX() < b->GetLayoutX(); });

            int  targetIndex = 0;
            float indicatorX = targetContainer->GetLayoutX();

            if (!others.empty())
            {
                int dropSlot = static_cast<int>(others.size());
                for (size_t i = 0; i < others.size(); ++i)
                {
                    const float center = others[i]->GetLayoutX() + others[i]->GetLayoutWidth() * 0.5f;
                    if (e.X < center) { dropSlot = static_cast<int>(i); break; }
                }

                if (dropSlot < static_cast<int>(others.size()))
                {
                    indicatorX = others[dropSlot]->GetLayoutX();
                    for (size_t i = 0; i < targetChildren.size(); ++i)
                        if (targetChildren[i].get() == others[dropSlot])
                            { targetIndex = static_cast<int>(i); break; }
                }
                else
                {
                    const UIElement* last = others.back();
                    indicatorX  = last->GetLayoutX() + last->GetLayoutWidth();
                    targetIndex = static_cast<int>(targetChildren.size());
                }
            }

            if (m_InsertionIndicator)
            {
                m_InsertionIndicator->Overrides()
                    .Set(Style::Position,      PositionType::Absolute)
                    .Set(Style::PositionLeft,  StyleLength::Px(indicatorX))
                    .Set(Style::PositionTop,   StyleLength::Px(targetContainer->GetLayoutY()))
                    .Set(Style::Width,         StyleLength::Px(3.0f))
                    .Set(Style::Height,        StyleLength::Px(btnH))
                    .Set(Style::BackgroundColor, (m_AccentColor & 0x00FFFFFFu) | 0xCC000000u)
                    .Set(Style::ZIndex,        9999)
                    .Set(Style::PointerEvents, false)
                    .Set(Style::Display,       DisplayMode::Block);
            }
            m_CurrentDropIndex = targetIndex;
            e.Stop();
        });

        btn->RegisterEventHandler(kEventMouseUp, [this, btn](UIEvent& e)
        {
            if (m_DraggedToolbarButton != btn || e.Button != 0) return;

            const bool wasDragActive = m_DragActive;

            if (wasDragActive && m_CurrentDropIndex >= 0 && m_TargetButtonContainer.Get() && m_DraggedButtonContainer.Get())
            {
                const UIElement::WeakRef<Button> buttonRef = UIElement::MakeWeakRef(btn);
                const UIElement::WeakRef<> sourceRef = m_DraggedButtonContainer;
                const UIElement::WeakRef<> targetRef = m_TargetButtonContainer;
                const std::weak_ptr<bool> token = m_LifetimeToken;
                const int dropIndex = m_CurrentDropIndex;

                btn->PostAction([this, token, buttonRef, sourceRef, targetRef, dropIndex]()
                {
                    // The owning toolbar can be destroyed, and a .uxml hot reload can destroy
                    // the button or either container, between the mouse-up and this action.
                    // Any of those cancels the drop.
                    if (token.expired()) return;
                    Button* dragged = buttonRef.Get();
                    UIElement* sourceContainer = sourceRef.Get();
                    UIElement* targetContainer = targetRef.Get();
                    if (!dragged || !sourceContainer || !targetContainer) return;

                    // Find button's current index in source.
                    int currentIdx = -1;
                    const auto& src = sourceContainer->GetChildren();
                    for (size_t i = 0; i < src.size(); ++i)
                        if (src[i].get() == dragged) { currentIdx = static_cast<int>(i); break; }
                    if (currentIdx < 0) return;

                    if (sourceContainer != targetContainer)
                    {
                        // Cross-container move: only the moved button needs
                        // TakeChild/InsertChild; leave existing target children
                        // in place to avoid disrupting their resolved styles.
                        auto taken = sourceContainer->TakeChild(dragged);
                        if (!taken) return;

                        const int insertIdx = std::min(dropIndex, static_cast<int>(targetContainer->GetChildren().size()));
                        targetContainer->InsertChild(static_cast<size_t>(insertIdx), std::move(taken));

                        m_DragOrderChanged = true;
                    }
                    else
                    {
                        // Same-container reorder: shuffle the children vector
                        // directly instead of TakeChild/AddChild to avoid
                        // detaching elements from the UIManager (which can
                        // disrupt the CSS cascade and cause stale styles).
                        int finalPos = dropIndex;
                        if (dropIndex > currentIdx) finalPos--;
                        if (finalPos == currentIdx) return;

                        auto& children = targetContainer->GetMutableChildren();
                        auto moved = std::move(children[currentIdx]);
                        children.erase(children.begin() + currentIdx);
                        const int insertAt = std::clamp(finalPos, 0, static_cast<int>(children.size()));
                        children.insert(children.begin() + insertAt, std::move(moved));
                        targetContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                        UIManagerNotifyTreeStructureChanged(targetContainer->GetOwnerManager());

                        m_DragOrderChanged = true;
                    }

                    if (m_DragOrderChanged)
                    {
                        // Lookup settings key by matching container pointer against known configs.
                        auto saveIfKnown = [&](UIElement* container)
                        {
                            for (const auto& rc : m_Containers)
                                if (container && container->HasClass(rc.ClassId))
                                    { SaveButtonOrder(container, rc.SettingsKey); return; }
                        };
                        if (sourceContainer != targetContainer)
                            saveIfKnown(sourceContainer);
                        saveIfKnown(targetContainer);
                    }
                });
            }

            if (wasDragActive)
            {
                btn->RemoveClass("dragging");
                btn->RemoveClass("pressed");
                btn->RemoveClass("active");

                if (m_DragGhostElement)
                {
                    if (UIElement* p = m_DragGhostElement->GetParent()) p->RemoveChild(m_DragGhostElement);
                    m_DragGhostElement = nullptr;
                }
                if (m_InsertionIndicator)
                {
                    if (UIElement* p = m_InsertionIndicator->GetParent()) p->RemoveChild(m_InsertionIndicator);
                    m_InsertionIndicator = nullptr;
                }
            }

            m_DraggedToolbarButton   = nullptr;
            m_DraggedButtonContainer = {};
            m_TargetButtonContainer  = {};
            m_DragOrderChanged       = false;
            m_DragActive             = false;
            m_CurrentDropIndex       = -1;

            if (wasDragActive) e.Stop();
        });
    };

    for (const auto& rc : m_Containers)
    {
        UIElement* const element = rc.Element.Get();
        if (!element) continue;
        for (const auto& child : element->GetChildren())
            if (auto* btn = dynamic_cast<Button*>(child.get()))
                setupButtonDrag(btn);
    }
}

void ToolbarDragDrop::SaveButtonOrder(UIElement* container, const std::string& settingsKey)
{
    if (!container || settingsKey.empty()) return;

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    nlohmann::json ids = nlohmann::json::array();
    for (const auto& child : container->GetChildren())
        if (!child->GetId().empty()) ids.push_back(child->GetId());

    prefs.SetJson(settingsKey, ids);
    (void)prefs.Save(&err);
}

void ToolbarDragDrop::LoadButtonOrder(UIElement* searchRoot, const std::vector<ContainerConfig>& containers)
{
    if (!searchRoot) return;

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    struct LoadedContainer
    {
        UIElement* element = nullptr;
        std::string settingsKey;
        std::vector<std::string> savedOrder;
    };
    std::vector<LoadedContainer> loaded;
    loaded.reserve(containers.size());

    // Resolve and snapshot every container before applying any persisted
    // cross-container moves. This preserves the authored reset order even when
    // a saved button now belongs to a different toolbar group.
    for (const auto& cfg : containers)
    {
        UIElement* container = FindContainerByClass(searchRoot, HashStringId(cfg.cssClass));
        if (!container) continue;

        // Snapshot the UXML-defined order before any reordering, once per container
        // instance: a .uxml hot reload that replaces the container mounts the new
        // authored order, and Reset returns to that one. UXML
        // elements that ship without an id (e.g. <UIElement class="toolbar-gap" />
        // separators) get a stable positional identifier "__noid_<n>__" so
        // ResetButtonOrder can later rebuild the same identifiers from the
        // current children and round-trip them back to their original spot
        // instead of dropping them at the end of their container.
        DefaultOrder& snapshot = m_DefaultOrders[cfg.settingsKey];
        if (snapshot.ContainerInstanceId != container->GetInstanceId())
        {
            snapshot.ContainerInstanceId = container->GetInstanceId();
            snapshot.Ids.clear();
            snapshot.Ids.reserve(container->GetChildren().size());
            size_t noIdCounter = 0;
            for (const auto& child : container->GetChildren())
            {
                const std::string& realId = child->GetId();
                if (!realId.empty()) snapshot.Ids.push_back(realId);
                else snapshot.Ids.push_back("__noid_" + std::to_string(noIdCounter++) + "__");
            }
        }

        LoadedContainer state;
        state.element = container;
        state.settingsKey = cfg.settingsKey;
        const auto& json = prefs.Json();
        auto it = json.find(cfg.settingsKey);
        if (it != json.end() && it->is_array())
            for (const auto& item : *it)
                if (item.is_string()) state.savedOrder.push_back(item.get<std::string>());
        loaded.push_back(std::move(state));
    }

    // Saved target lists also encode group ownership. Reconstruct that first;
    // the previous implementation only reordered children already present in
    // each UXML group, so a cross-group drag was saved but lost after restart.
    std::unordered_map<std::string, size_t> savedOwner;
    for (size_t containerIndex = 0; containerIndex < loaded.size(); ++containerIndex)
        for (const std::string& id : loaded[containerIndex].savedOrder)
            savedOwner[id] = containerIndex;

    for (size_t targetIndex = 0; targetIndex < loaded.size(); ++targetIndex)
    {
        UIElement* target = loaded[targetIndex].element;
        for (const std::string& id : loaded[targetIndex].savedOrder)
        {
            const auto owner = savedOwner.find(id);
            if (owner == savedOwner.end() || owner->second != targetIndex)
                continue;

            bool alreadyInTarget = false;
            for (const auto& child : target->GetChildren())
                if (child->GetId() == id)
                    { alreadyInTarget = true; break; }
            if (alreadyInTarget)
                continue;

            for (LoadedContainer& sourceState : loaded)
            {
                UIElement* source = sourceState.element;
                if (source == target)
                    continue;

                UIElement* childToMove = nullptr;
                for (const auto& child : source->GetChildren())
                    if (child->GetId() == id)
                        { childToMove = child.get(); break; }
                if (!childToMove)
                    continue;

                auto moved = source->TakeChild(childToMove);
                if (moved)
                    target->InsertChild(target->GetChildren().size(), std::move(moved));
                break;
            }
        }
    }

    // Apply each group's exact saved order after all children are back in their
    // persisted groups. Hidden buttons remain in this child order, so enabling
    // one later restores it at the position where the user moved it.
    for (LoadedContainer& state : loaded)
    {
        UIElement* container = state.element;
        const std::vector<std::string>& savedOrder = state.savedOrder;
        if (savedOrder.empty())
            continue;

        // Build id->index map for current children.
        auto& children = container->GetMutableChildren();
        std::unordered_map<std::string, size_t> indexById;
        for (size_t i = 0; i < children.size(); ++i)
            if (!children[i]->GetId().empty()) indexById[children[i]->GetId()] = i;

        // Compute desired final order: saved ids first (filtered to present ids), then remainder.
        std::vector<size_t> newOrder;
        newOrder.reserve(children.size());
        std::vector<bool> used(children.size(), false);
        for (const auto& id : savedOrder)
        {
            auto it2 = indexById.find(id);
            if (it2 == indexById.end()) continue;
            newOrder.push_back(it2->second);
            used[it2->second] = true;
        }
        for (size_t i = 0; i < children.size(); ++i)
            if (!used[i]) newOrder.push_back(i);

        if (newOrder.size() != children.size()) continue;

        // Check if already in order to avoid unnecessary dirty.
        bool alreadySorted = true;
        for (size_t i = 0; i < newOrder.size(); ++i)
            if (newOrder[i] != i) { alreadySorted = false; break; }
        if (alreadySorted) continue;

        // Reorder in-place without detaching elements (preserves resolved CSS styles).
        std::vector<std::unique_ptr<UIElement>> sorted(children.size());
        for (size_t i = 0; i < newOrder.size(); ++i)
            sorted[i] = std::move(children[newOrder[i]]);
        children = std::move(sorted);
        container->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        UIManagerNotifyTreeStructureChanged(container->GetOwnerManager());
    }
}

void ToolbarDragDrop::ResetButtonOrder(UIElement* searchRoot, const std::vector<ContainerConfig>& containers)
{
    if (!searchRoot) return;

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool changed = false;
    for (const auto& cfg : containers)
    {
        if (prefs.Contains(cfg.settingsKey))
        {
            prefs.Remove(cfg.settingsKey);
            changed = true;
        }

        auto it = m_DefaultOrders.find(cfg.settingsKey);
        if (it == m_DefaultOrders.end()) continue;

        UIElement* container = FindContainerByClass(searchRoot, HashStringId(cfg.cssClass));
        if (!container) continue;

        const std::vector<std::string>& defaultIds = it->second.Ids;
        auto& children = container->GetMutableChildren();

        // Index by id, with the same "__noid_<n>__" synthetic identifier the
        // snapshot captured for un-id'd UXML elements. Without these synthetic
        // entries, the loop below would treat un-id'd children as "unused"
        // and append them to the end of the container — which is what made
        // Reset move the UXML toolbar-gap separators away from their seams.
        std::unordered_map<std::string, size_t> indexById;
        size_t noIdCounter = 0;
        for (size_t i = 0; i < children.size(); ++i)
        {
            const std::string& realId = children[i]->GetId();
            if (!realId.empty())
                indexById[realId] = i;
            else
                indexById["__noid_" + std::to_string(noIdCounter++) + "__"] = i;
        }

        std::vector<size_t> newOrder;
        newOrder.reserve(children.size());
        std::vector<bool> used(children.size(), false);
        for (const auto& id : defaultIds)
        {
            auto it2 = indexById.find(id);
            if (it2 == indexById.end()) continue;
            newOrder.push_back(it2->second);
            used[it2->second] = true;
        }
        for (size_t i = 0; i < children.size(); ++i)
            if (!used[i]) newOrder.push_back(i);

        if (newOrder.size() != children.size()) continue;

        bool alreadySorted = true;
        for (size_t i = 0; i < newOrder.size(); ++i)
            if (newOrder[i] != i) { alreadySorted = false; break; }
        if (alreadySorted) continue;

        std::vector<std::unique_ptr<UIElement>> sorted(children.size());
        for (size_t i = 0; i < newOrder.size(); ++i)
            sorted[i] = std::move(children[newOrder[i]]);
        children = std::move(sorted);
        container->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        UIManagerNotifyTreeStructureChanged(container->GetOwnerManager());
    }

    if (changed)
        (void)prefs.Save(&err);
}

} // namespace GameEngine::Editor
