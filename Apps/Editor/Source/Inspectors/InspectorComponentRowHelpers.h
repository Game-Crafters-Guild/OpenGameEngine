#pragma once

#include "Editor/Entities/EditorECSHelpers.h"
#include "InspectorRegistry.h"  // InspectorContext, used by GetAdditionalEntities below
#include "Inspectors/InspectorDragHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace InspectorDrag
{

/// Extract additional entities from an InspectorContext (all except the primary entity).
inline std::vector<ECS::EntityHandle> GetAdditionalEntities(const InspectorContext& ctx)
{
    std::vector<ECS::EntityHandle> extras;
    for (auto& ent : ctx.Entities)
        if (ent != ctx.Entity) extras.push_back(ent);
    return extras;
}

/// Build a SnapshotTarget that captures/restores a raw ECS component by value.
/// Apply writes bytes only (no notification); Notify fires the change event.
template<typename TComponent>
inline Editor::UndoRedoService::SnapshotTarget MakeComponentSnapshotTarget(
    std::function<ECS::World*()> resolveWorld, ECS::EntityHandle e,
    Editor::EditorChangeNotifications* n,
    const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [resolveWorld, e](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (!w)
            return false;
        auto* comp = w->GetComponent<TComponent>(e);
        if (!comp)
            return false;
        out.resize(sizeof(TComponent));
        std::memcpy(out.data(), comp, sizeof(TComponent));
        return true;
    };

    target.Apply = [resolveWorld, e](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snap) -> bool
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (snap.size() != sizeof(TComponent))
            return false;
        if (!w || !e.IsValid() || !w->IsValid(e))
            return false;
        TComponent comp{};
        std::memcpy(&comp, snap.data(), sizeof(TComponent));
        w->AddComponentImmediate(e, comp);
        return true;
    };

    target.Notify = [resolveWorld, e, n](Editor::EditorChangeNotifications::ChangeKind kind)
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (n && w)
            n->NotifyComponentChange<TComponent>(w, e, kind);
    };

    return target;
}

template<typename TComponent>
inline Editor::UndoRedoService::SnapshotTarget MakeComponentSnapshotTarget(
    ECS::World* w, ECS::EntityHandle e,
    Editor::EditorChangeNotifications* n,
    const std::string& label)
{
    return MakeComponentSnapshotTarget<TComponent>([w]() { return w; }, e, n, label);
}

/// Type-erased SnapshotTarget for a component identified only by its runtime
/// ComponentTypeId (no compile-time T). Captures/restores via the World's
/// byte-level component I/O; Notify fires the by-typeId change event. Used by the
/// reflection-driven default inspector.
inline Editor::UndoRedoService::SnapshotTarget MakeComponentSnapshotTargetById(
    ECS::World* w, ECS::EntityHandle e, ECS::ComponentTypeId typeId,
    Editor::EditorChangeNotifications* n, const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [w, e, typeId](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool {
        return w && w->CaptureComponentBytes(e, typeId, out);
    };
    target.Apply = [w, e, typeId](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snap) -> bool {
        return w && w->ApplyComponentBytesImmediate(e, typeId, snap);
    };
    target.Notify = [w, e, typeId, n](Editor::EditorChangeNotifications::ChangeKind kind) {
        if (n)
            n->NotifyComponentChanged({w, e, typeId, kind});
    };

    return target;
}

/// Type-erased multi-entity SnapshotTarget (primary + extras) for a component
/// identified only by its runtime ComponentTypeId. The blob concatenates each
/// entity's component bytes; one snapshot = one undo entry covering every edited
/// entity. The by-typeId analogue of MakeMultiComponentSnapshotTarget<T>, used by
/// the reflection-driven default inspector for multi-selection edits.
inline Editor::UndoRedoService::SnapshotTarget MakeMultiComponentSnapshotTargetById(
    ECS::World* w, ECS::EntityHandle primary,
    const std::vector<ECS::EntityHandle>& additionalEntities, ECS::ComponentTypeId typeId,
    Editor::EditorChangeNotifications* n, const std::string& label)
{
    auto allEntities = std::make_shared<std::vector<ECS::EntityHandle>>();
    allEntities->push_back(primary);
    for (auto& ex : additionalEntities)
        allEntities->push_back(ex);

    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    using Snapshot = Editor::UndoRedoService::SnapshotTarget::Snapshot;

    // Each entity's blob is length-prefixed (uint32 byte count; 0 = absent). A
    // missing/destroyed entity contributes a zero-length entry instead of aborting
    // the whole capture, so an edit across a selection where one entity later loses
    // the component (or is deleted) still records a working undo entry for the rest.
    target.Capture = [w, allEntities, typeId](Snapshot& out) -> bool {
        if (!w)
            return false;
        out.clear();
        for (ECS::EntityHandle ent : *allEntities)
        {
            Snapshot one;
            const std::uint32_t len = w->CaptureComponentBytes(ent, typeId, one)
                                        ? static_cast<std::uint32_t>(one.size()) : 0u;
            const std::size_t at = out.size();
            out.resize(at + sizeof(len));
            std::memcpy(out.data() + at, &len, sizeof(len));
            out.insert(out.end(), one.begin(), one.end());
        }
        return true;
    };
    target.Apply = [w, allEntities, typeId](const Snapshot& snap) -> bool {
        if (!w)
            return false;
        std::size_t cursor = 0;
        for (ECS::EntityHandle ent : *allEntities)
        {
            if (cursor + sizeof(std::uint32_t) > snap.size())
                return false;
            std::uint32_t len = 0;
            std::memcpy(&len, snap.data() + cursor, sizeof(len));
            cursor += sizeof(len);
            if (len == 0)
                continue;  // entity was absent at capture time
            if (cursor + len > snap.size())
                return false;
            Snapshot one(snap.begin() + static_cast<std::ptrdiff_t>(cursor),
                         snap.begin() + static_cast<std::ptrdiff_t>(cursor + len));
            cursor += len;
            if (ent.IsValid() && w->IsValid(ent))
                w->ApplyComponentBytesImmediate(ent, typeId, one);
        }
        return true;
    };
    target.Notify = [w, allEntities, typeId, n](Editor::EditorChangeNotifications::ChangeKind kind) {
        if (!n)
            return;
        for (auto ent : *allEntities)
            n->NotifyComponentChanged({w, ent, typeId, kind});
    };

    return target;
}

/// Build a SnapshotTarget that captures/restores a component across multiple entities.
/// The snapshot blob stores each entity's component data concatenated.
template<typename TComponent>
inline Editor::UndoRedoService::SnapshotTarget MakeMultiComponentSnapshotTarget(
    std::function<ECS::World*()> resolveWorld, ECS::EntityHandle primary,
    const std::vector<ECS::EntityHandle>& additionalEntities,
    Editor::EditorChangeNotifications* n,
    const std::string& label)
{
    // Build the full list from entities that actually carry the component.
    // InspectorContext::Entities may contain a heterogeneous selection, while
    // the component section is rendered from the primary entity. Including a
    // peer without TComponent would make Capture fail and silently disable undo
    // for the peers that are editable.
    auto allEntities = std::make_shared<std::vector<ECS::EntityHandle>>();
    allEntities->push_back(primary);
    ECS::World* initialWorld = resolveWorld ? resolveWorld() : nullptr;
    for (auto& ex : additionalEntities)
    {
        if (initialWorld && initialWorld->GetComponent<TComponent>(ex))
            allEntities->push_back(ex);
    }

    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [resolveWorld, allEntities](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (!w)
            return false;
        out.resize(allEntities->size() * sizeof(TComponent));
        for (std::size_t i = 0; i < allEntities->size(); ++i)
        {
            auto* comp = w->GetComponent<TComponent>((*allEntities)[i]);
            if (!comp)
                return false;
            std::memcpy(out.data() + i * sizeof(TComponent), comp, sizeof(TComponent));
        }
        return true;
    };

    target.Apply = [resolveWorld, allEntities](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snap) -> bool
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (snap.size() != allEntities->size() * sizeof(TComponent))
            return false;
        for (std::size_t i = 0; i < allEntities->size(); ++i)
        {
            auto ent = (*allEntities)[i];
            if (!w || !ent.IsValid() || !w->IsValid(ent))
                continue;
            TComponent comp{};
            std::memcpy(&comp, snap.data() + i * sizeof(TComponent), sizeof(TComponent));
            w->AddComponentImmediate(ent, comp);
        }
        return true;
    };

    target.Notify = [resolveWorld, allEntities, n](Editor::EditorChangeNotifications::ChangeKind kind)
    {
        ECS::World* w = resolveWorld ? resolveWorld() : nullptr;
        if (!n || !w)
            return;
        for (auto& ent : *allEntities)
            n->NotifyComponentChange<TComponent>(w, ent, kind);
    };

    return target;
}

template<typename TComponent>
inline Editor::UndoRedoService::SnapshotTarget MakeMultiComponentSnapshotTarget(
    ECS::World* w, ECS::EntityHandle primary,
    const std::vector<ECS::EntityHandle>& additionalEntities,
    Editor::EditorChangeNotifications* n,
    const std::string& label)
{
    return MakeMultiComponentSnapshotTarget<TComponent>(
        [w]() { return w; }, primary, additionalEntities, n, label);
}

/// One-shot commit with undo support. Captures before state, applies the lambda,
/// writes to world, and pushes a SnapshotCommand. Falls back to CommitComponentUpdate
/// when undo is null.
template<typename TComponent, typename ApplyFn>
inline void CommitComponentWithUndo(ECS::World* w, ECS::EntityHandle e,
                                    Editor::EditorChangeNotifications* n,
                                    Editor::UndoRedoService* undo,
                                    const std::string& name, ApplyFn&& apply)
{
    auto* comp = w->GetComponent<TComponent>(e);
    if (!comp)
        return;

    TComponent updated = *comp;
    apply(updated);

    if (undo)
    {
        auto target = MakeComponentSnapshotTarget<TComponent>(w, e, n, name);
        auto edit = undo->BeginInteractiveEdit(name, std::move(target));
        w->AddComponentImmediate(e, updated);
        edit.Commit();
    }
    else
    {
        Editor::CommitComponentUpdate(w, e, n, updated);
    }
}

/// Applies `apply` to entity `e`'s TComponent and writes it back: in place when an undo edit
/// records the change, through CommitComponentUpdate (which notifies) when none does.
template<typename TComponent, typename ApplyFn>
inline void ApplyComponentEdit(ECS::World* w, ECS::EntityHandle e, Editor::EditorChangeNotifications* n,
                               bool recorded, ApplyFn& apply)
{
    const auto* comp = w->GetComponent<TComponent>(e);
    if (!comp)
        return;
    TComponent updated = *comp;
    apply(updated);
    if (recorded)
        w->AddComponentImmediate(e, updated);
    else
        Editor::CommitComponentUpdate(w, e, n, updated);
}

/// CommitComponentWithUndo for a selection: the same edit to `primary` and to every entity of
/// `additionalEntities` holding TComponent, recorded as one undo entry.
template<typename TComponent, typename ApplyFn>
inline void CommitComponentsWithUndo(ECS::World* w, ECS::EntityHandle primary,
                                     const std::vector<ECS::EntityHandle>& additionalEntities,
                                     Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo,
                                     const std::string& name, ApplyFn&& apply)
{
    std::optional<Editor::UndoRedoService::InteractiveEdit> edit;
    if (undo)
        edit.emplace(undo->BeginInteractiveEdit(
            name, MakeMultiComponentSnapshotTarget<TComponent>(w, primary, additionalEntities, n, name)));
    ApplyComponentEdit<TComponent>(w, primary, n, edit.has_value(), apply);
    for (ECS::EntityHandle e : additionalEntities)
        if (e != primary)
            ApplyComponentEdit<TComponent>(w, e, n, edit.has_value(), apply);
    if (edit)
        edit->Commit();
}

/// Build the (preview, commit) callbacks that drive an interactive ECS-component edit
/// for a value-typed row. The first preview begins one interactive
/// undo edit and each subsequent preview/commit feeds it; a typed commit with no prior
/// preview records its own entry. Multi-selection mirrors the edit to `additionalEntities`.
/// Shared by component-backed Inspector fields and sliders so this multi-entity
/// undo lifecycle lives in exactly one place.
///
/// `applyPreview` is what an edit in flight writes and `applyCommit` what its commit
/// writes. Every preview and the commit start from the live component, which holds
/// the last preview, so a write that settles other fields from the component's own
/// state belongs to the commit alone: in a preview it would settle them again from
/// the last preview's result, and a value passed on the way would stick.
template<typename TComponent, typename V, typename PreviewFn, typename CommitFn>
inline std::pair<std::function<void(V)>, std::function<void(V)>>
MakeComponentInteractiveHandlers(ECS::World* w, ECS::EntityHandle e,
                                 Editor::EditorChangeNotifications* n,
                                 Editor::UndoRedoService* undo,
                                 std::string name, PreviewFn applyPreview, CommitFn applyCommit,
                                 const std::vector<ECS::EntityHandle>& additionalEntities,
                                 std::function<ECS::World*()> getWorld = {})
{
    using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto editPtr = std::make_shared<OptEdit>();
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    // Inspector controls may briefly outlive the world binding they were built
    // for. Reject callbacks after a rebind instead of touching expectedWorld.
    auto resolveWorld = [expectedWorld = w, getWorld = std::move(getWorld)]() -> ECS::World*
    {
        if (!getWorld)
            return expectedWorld;
        ECS::World* currentWorld = getWorld();
        return currentWorld == expectedWorld ? currentWorld : nullptr;
    };

    std::function<void(V)> preview =
        [resolveWorld, e, n, undo, name, apply = std::move(applyPreview), editPtr, extras](V v)
        {
            ECS::World* activeWorld = resolveWorld();
            if (!activeWorld)
            {
                // The widget was rebound while an interaction was active. The
                // snapshot target is bound to the edited world, so cancelling
                // here restores any preview instead of stranding it there.
                if (editPtr->has_value())
                {
                    editPtr->value().Cancel();
                    editPtr->reset();
                }
                return;
            }
            auto* comp = activeWorld->GetComponent<TComponent>(e);
            if (!comp)
                return;

            if (!editPtr->has_value() && undo)
            {
                auto target = extras->empty()
                    ? MakeComponentSnapshotTarget<TComponent>(activeWorld, e, n, name)
                    : MakeMultiComponentSnapshotTarget<TComponent>(activeWorld, e, *extras, n, name);
                editPtr->emplace(undo->BeginInteractiveEdit(name, std::move(target)));
            }

            TComponent updated = *comp;
            apply(updated, v);

            if (editPtr->has_value() && editPtr->value())
            {
                TComponent captured = updated;
                editPtr->value().Preview([activeWorld, e, captured, extras, apply, v]{
                    activeWorld->AddComponentImmediate(e, captured);
                    for (auto& ex : *extras)
                    {
                        auto* c = activeWorld->GetComponent<TComponent>(ex);
                        if (!c) continue;
                        TComponent u = *c;
                        apply(u, v);
                        activeWorld->AddComponentImmediate(ex, u);
                    }
                });
                // InteractiveEdit::Preview dispatches SnapshotTarget::Notify for
                // the primary component and every additional entity.
            }
            else
            {
                Editor::PreviewComponentUpdate(activeWorld, e, n, updated);
                for (auto& ex : *extras)
                {
                    auto* c = activeWorld->GetComponent<TComponent>(ex);
                    if (!c) continue;
                    TComponent u = *c;
                    apply(u, v);
                    Editor::PreviewComponentUpdate(activeWorld, ex, n, u);
                }
            }
        };

    std::function<void(V)> commit =
        [resolveWorld, e, n, undo, name, apply = std::move(applyCommit), editPtr, extras](V v)
        {
            ECS::World* activeWorld = resolveWorld();
            if (!activeWorld)
            {
                if (editPtr->has_value())
                {
                    editPtr->value().Cancel();
                    editPtr->reset();
                }
                return;
            }
            auto* comp = activeWorld->GetComponent<TComponent>(e);
            if (!comp)
                return;

            TComponent updated = *comp;
            apply(updated, v);

            if (editPtr->has_value() && editPtr->value())
            {
                activeWorld->AddComponentImmediate(e, updated);
                for (auto& ex : *extras)
                {
                    auto* c = activeWorld->GetComponent<TComponent>(ex);
                    if (!c) continue;
                    TComponent u = *c;
                    apply(u, v);
                    activeWorld->AddComponentImmediate(ex, u);
                }
                editPtr->value().Commit();
                // InteractiveEdit::Commit dispatches SnapshotTarget::Notify.
                editPtr->reset();
            }
            else
            {
                // A typed value or label double-click can commit without a
                // preceding preview. Use the same snapshot target here so a
                // multi-selection reset remains one atomic undo operation.
                std::optional<Editor::UndoRedoService::InteractiveEdit> oneShot;
                if (undo)
                {
                    auto target = extras->empty()
                        ? MakeComponentSnapshotTarget<TComponent>(activeWorld, e, n, name)
                        : MakeMultiComponentSnapshotTarget<TComponent>(activeWorld, e, *extras, n, name);
                    oneShot.emplace(undo->BeginInteractiveEdit(name, std::move(target)));
                }

                activeWorld->AddComponentImmediate(e, updated);
                for (auto& ex : *extras)
                {
                    auto* c = activeWorld->GetComponent<TComponent>(ex);
                    if (!c) continue;
                    TComponent u = *c;
                    apply(u, v);
                    activeWorld->AddComponentImmediate(ex, u);
                }

                const bool committedWithUndo =
                    oneShot.has_value() && oneShot->operator bool();
                if (committedWithUndo)
                    oneShot->Commit();

                // A valid InteractiveEdit owns the notification. Preserve the
                // explicit fallback for callers without undo or when the initial
                // snapshot could not be captured.
                if (n && !committedWithUndo)
                {
                    n->NotifyComponentCommit<TComponent>(activeWorld, e);
                    for (auto& ex : *extras)
                        n->NotifyComponentCommit<TComponent>(activeWorld, ex);
                }
            }
        };

    return {std::move(preview), std::move(commit)};
}

/// The same for a row whose previews and commit write the same thing.
template<typename TComponent, typename V, typename ApplyFn>
inline std::pair<std::function<void(V)>, std::function<void(V)>>
MakeComponentInteractiveHandlers(ECS::World* w, ECS::EntityHandle e,
                                 Editor::EditorChangeNotifications* n,
                                 Editor::UndoRedoService* undo,
                                 std::string name, ApplyFn apply,
                                 const std::vector<ECS::EntityHandle>& additionalEntities,
                                 std::function<ECS::World*()> getWorld = {})
{
    return MakeComponentInteractiveHandlers<TComponent, V, ApplyFn, ApplyFn>(
        w, e, n, undo, std::move(name), apply, apply, additionalEntities, std::move(getWorld));
}

/// Float row: preview (live drag / typing) + commit (mouse up) for an ECS component field.
template<typename TComponent, typename ApplyFn>
inline FloatField* AddComponentFloatRowWithDrag(UIElement* parent, const std::string& label, float initial,
                                                ECS::World* w, ECS::EntityHandle e,
                                                Editor::EditorChangeNotifications* n, ApplyFn&& apply,
                                                const char* tooltip = nullptr,
                                                const std::vector<ECS::EntityHandle>& additionalEntities = {},
                                                float minValue = -std::numeric_limits<float>::infinity(),
                                                float maxValue = std::numeric_limits<float>::infinity())
{
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    return AddFloatRowWithDrag(parent, label, initial,
        [w, e, n, apply, extras](float v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::PreviewComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::PreviewComponentUpdate(w, ex, n, u);
            }
        },
        [w, e, n, apply, extras](float v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::CommitComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::CommitComponentUpdate(w, ex, n, u);
            }
        },
        std::numeric_limits<float>::quiet_NaN(),
        tooltip,
        minValue,
        maxValue);
}

/// Float row with undo support. Drag gestures are tracked as a single interactive
/// edit (one undo entry per drag). Direct keyboard commits create their own entry.
template<typename TComponent, typename ApplyFn>
inline FloatField* AddComponentFloatRowWithDrag(UIElement* parent, const std::string& label, float initial,
                                                ECS::World* w, ECS::EntityHandle e,
                                                Editor::EditorChangeNotifications* n,
                                                Editor::UndoRedoService* undo,
                                                const std::string& name, ApplyFn&& apply,
                                                float defaultValue = 0.0f,
                                                const char* tooltip = nullptr,
                                                const std::vector<ECS::EntityHandle>& additionalEntities = {},
                                                float minValue = -std::numeric_limits<float>::infinity(),
                                                float maxValue = std::numeric_limits<float>::infinity())
{
    auto handlers = MakeComponentInteractiveHandlers<TComponent, float>(
        w, e, n, undo, name, std::forward<ApplyFn>(apply), additionalEntities);
    return AddFloatRowWithDrag(parent, label, initial,
        std::move(handlers.first), std::move(handlers.second),
        defaultValue, tooltip, minValue, maxValue);
}

/// Bounded float row that shows the range AND the number: the shared slider-with-value
/// row (track + thumb + an editable field) bound to an ECS component field. Use this for a
/// quantity whose range is part of its meaning, where an author still needs to read and type
/// the exact value - a plain slider hides the number and a plain field hides the range.
/// The two halves stay in step: dragging the track updates the field, and committing the
/// field moves the thumb.
template<typename TComponent, typename ApplyFn>
inline SliderWithFloatValueRow AddComponentSliderWithValueRow(
    UIElement* parent, const std::string& label, float initial,
    float minValue, float maxValue,
    ECS::World* w, ECS::EntityHandle e,
    Editor::EditorChangeNotifications* n, ApplyFn&& apply,
    const char* tooltip = nullptr,
    const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    auto applyTo = [w, n, apply, extras](ECS::EntityHandle target, float v, bool commit)
    {
        auto* comp = w->GetComponent<TComponent>(target);
        if (!comp)
            return;
        TComponent updated = *comp;
        apply(updated, v);
        if (commit)
            Editor::CommitComponentUpdate(w, target, n, updated);
        else
            Editor::PreviewComponentUpdate(w, target, n, updated);
    };
    auto run = [applyTo, e, extras](float v, bool commit)
    {
        applyTo(e, v, commit);
        for (auto& ex : *extras)
            applyTo(ex, v, commit);
    };

    auto row = AddSliderWithFloatValueRow(parent, label, initial, minValue, maxValue, tooltip);
    Slider* slider = row.Slider;
    FloatField* field = row.ValueField;
    slider->SetOnValueChanging([run, field](const float& v) { field->SetValue(v); run(v, false); });
    slider->SetOnValueChanged([run, field](const float& v) { field->SetValue(v); run(v, true); });
    // The field accepts anything the component's own clamp accepts, so echo the clamped
    // result back to the thumb rather than the typed value.
    field->SetOnValueChanged([run, slider, minValue, maxValue](const float& v)
    {
        const float clamped = std::clamp(v, minValue, maxValue);
        slider->SetValueWithoutNotify(clamped);
        run(clamped, true);
    });
    return row;
}

/// A [0,1] multiplier presented the way an author reads it: the field shows a PERCENTAGE of
/// whatever the multiplier scales and the thumb sits at the multiplier itself.
/// `labelFor` returns the row's label for a stored value, so a caller can carry the absolute the
/// percentage lands on ("Height (0.36 m)"); it is re-evaluated on every change. The field always
/// reads a bare percentage with a "%" suffix, because a unit long enough to hold an absolute does
/// not fit beside the number.
template<typename TComponent, typename ApplyFn, typename LabelFn>
inline SliderWithFloatValueRow AddComponentMultiplierPercentRow(
    UIElement* parent, float initial,
    ECS::World* w, ECS::EntityHandle e,
    Editor::EditorChangeNotifications* n, ApplyFn&& apply, LabelFn&& labelFor,
    const char* tooltip,
    const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    auto run = [w, e, n, apply, extras](float value, bool commit)
    {
        auto applyTo = [&](ECS::EntityHandle target)
        {
            auto* comp = w->GetComponent<TComponent>(target);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, value);
            if (commit)
                Editor::CommitComponentUpdate(w, target, n, updated);
            else
                Editor::PreviewComponentUpdate(w, target, n, updated);
        };
        applyTo(e);
        for (auto& ex : *extras)
            applyTo(ex);
    };

    auto row = AddSliderWithFloatValueRow(parent, labelFor(initial), initial * 100.0f,
                                          0.0f, 100.0f, tooltip);
    Slider* slider = row.Slider;
    FloatField* field = row.ValueField;
    Label* rowLabel = row.Label;
    slider->SetMin(0.0f);
    slider->SetMax(1.0f);
    slider->SetValueWithoutNotify(initial);
    field->SetValue(initial * 100.0f);
    field->SetSuffix("%");

    auto show = [field, rowLabel, labelFor](float value)
    {
        field->SetValue(value * 100.0f);
        if (rowLabel)
            rowLabel->SetText(labelFor(value));
    };
    slider->SetOnValueChanging([run, show](const float& v)
    {
        show(v);
        run(v, false);
    });
    slider->SetOnValueChanged([run, show](const float& v)
    {
        show(v);
        run(v, true);
    });
    field->SetOnValueChanged([run, slider, rowLabel, labelFor](const float& percent)
    {
        const float v = std::clamp(percent / 100.0f, 0.0f, 1.0f);
        slider->SetValueWithoutNotify(v);
        if (rowLabel)
            rowLabel->SetText(labelFor(v));
        run(v, true);
    });
    return row;
}

/// Bounded float slider row (visible track + thumb + value bubble): preview (live
/// drag) + commit (release) for an ECS component field with a known [min,max] range.
template<typename TComponent, typename ApplyFn>
inline Slider* AddComponentFloatSliderRow(UIElement* parent, const std::string& label, float initial,
                                          float minValue, float maxValue,
                                          ECS::World* w, ECS::EntityHandle e,
                                          Editor::EditorChangeNotifications* n, ApplyFn&& apply,
                                          const char* tooltip = nullptr,
                                          const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    return AddFloatSliderRow(parent, label, initial, minValue, maxValue,
        [w, e, n, apply, extras](float v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::PreviewComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::PreviewComponentUpdate(w, ex, n, u);
            }
        },
        [w, e, n, apply, extras](float v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::CommitComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::CommitComponentUpdate(w, ex, n, u);
            }
        },
        tooltip).Slider;
}

/// Bounded float slider row with undo support. Live drag updates share one
/// interactive edit, so releasing the thumb records a single undo entry.
/// Programmatic/direct commits without a preceding preview also record one entry.
template<typename TComponent, typename ApplyFn>
inline Slider* AddComponentFloatSliderRow(UIElement* parent, const std::string& label, float initial,
                                          float minValue, float maxValue,
                                          ECS::World* w, ECS::EntityHandle e,
                                          Editor::EditorChangeNotifications* n,
                                          Editor::UndoRedoService* undo,
                                          const std::string& name, ApplyFn&& apply,
                                          const char* tooltip = nullptr,
                                          const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto handlers = MakeComponentInteractiveHandlers<TComponent, float>(
        w, e, n, undo, name, std::forward<ApplyFn>(apply), additionalEntities);
    return AddFloatSliderRow(parent, label, initial, minValue, maxValue,
        std::move(handlers.first), std::move(handlers.second), tooltip).Slider;
}

/// Int row: preview + commit for an ECS component field.
template<typename TComponent, typename ApplyFn>
inline IntField* AddComponentIntRowWithDrag(UIElement* parent, const std::string& label, int initial,
                                            ECS::World* w, ECS::EntityHandle e,
                                            Editor::EditorChangeNotifications* n, ApplyFn&& apply,
                                            const char* tooltip = nullptr,
                                            const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto extras = std::make_shared<std::vector<ECS::EntityHandle>>(additionalEntities);
    return AddIntRowWithDrag(parent, label, initial,
        [w, e, n, apply, extras](int v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::PreviewComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::PreviewComponentUpdate(w, ex, n, u);
            }
        },
        [w, e, n, apply, extras](int v)
        {
            auto* comp = w->GetComponent<TComponent>(e);
            if (!comp)
                return;
            TComponent updated = *comp;
            apply(updated, v);
            Editor::CommitComponentUpdate(w, e, n, updated);
            for (auto& ex : *extras)
            {
                auto* c = w->GetComponent<TComponent>(ex);
                if (!c) continue;
                TComponent u = *c;
                apply(u, v);
                Editor::CommitComponentUpdate(w, ex, n, u);
            }
        },
        std::numeric_limits<int>::min(),
        tooltip);
}

/// Int row with undo support. Same gesture-tracking approach as the float overload.
template<typename TComponent, typename ApplyFn>
inline IntField* AddComponentIntRowWithDrag(UIElement* parent, const std::string& label, int initial,
                                            ECS::World* w, ECS::EntityHandle e,
                                            Editor::EditorChangeNotifications* n,
                                            Editor::UndoRedoService* undo,
                                            const std::string& name, ApplyFn&& apply,
                                            int defaultValue = 0,
                                            const char* tooltip = nullptr,
                                            const std::vector<ECS::EntityHandle>& additionalEntities = {})
{
    auto handlers = MakeComponentInteractiveHandlers<TComponent, int>(
        w, e, n, undo, name, std::forward<ApplyFn>(apply), additionalEntities);
    return AddIntRowWithDrag(parent, label, initial,
        std::move(handlers.first), std::move(handlers.second),
        defaultValue, tooltip);
}

} // namespace InspectorDrag
} // namespace GameEngine
