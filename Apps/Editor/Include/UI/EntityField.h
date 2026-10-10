#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ECS/ECS.h"  // EntityHandle, ComponentTypeId
#include "UI/ReferenceFieldBase.h"

namespace GameEngine
{

namespace ECS { class World; }

/// Inspector field for selecting an entity reference (EntityHandle).
///
/// Displays the referenced entity's Name (falling back to "Entity <id>", or
/// "(None)" when unset/invalid) and supports:
///   - Drag-drop of a HierarchyEntityDragPayload (drag an entity from the Hierarchy)
///   - Clear button to remove the reference
///   - An optional required-components filter: only entities that have ALL of the
///     given components are accepted (e.g. restrict to entities with a PhysicsCollider)
///
/// Usage:
///   auto field = std::make_unique<EntityField>();
///   field->SetWorld(world);
///   field->SetValue(currentHandle);
///   field->SetOnValueChanged([](ECS::EntityHandle h) { ... });
///   parent->AddChild(std::move(field));
class EntityField : public ReferenceFieldBase
{
public:
    using ValueChangedCallback = std::function<void(ECS::EntityHandle newValue)>;

    EntityField();

    /// World used to validate handles, resolve names, and check required components.
    void SetWorld(ECS::World* world);

    /// Set the current value (updates the display). Does not fire the callback.
    void SetValue(ECS::EntityHandle handle);
    ECS::EntityHandle GetValue() const { return m_Value; }

    /// Clear to an invalid handle (fires the callback).
    void Clear();

    /// Callback invoked when the value changes (drop or clear).
    void SetOnValueChanged(ValueChangedCallback cb) { m_OnValueChanged = std::move(cb); }

    /// Restrict accepted entities to those that have ALL of these components
    /// (empty = any entity). Drops failing the filter are rejected.
    void SetRequiredComponents(std::vector<ECS::ComponentTypeId> required);

    // -- IDropTarget: payload-specific parts (generic hit-test/preview in base) --
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;

protected:
    std::unique_ptr<ISearchProvider> CreateProvider() override;
    void OnResultSelected(const SearchResultItem& item) override;
    void PositionDialog(SearchDialog& dialog) override;
    SearchItemId CurrentResultId() const override;
    void OnClearRequested() override { Clear(); }

private:
    void UpdateDisplay();
    void ApplyValue(ECS::EntityHandle handle);
    bool MeetsRequirements(ECS::EntityHandle handle) const;

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Value;
    std::vector<ECS::ComponentTypeId> m_Required;
    ValueChangedCallback m_OnValueChanged;
};

} // namespace GameEngine
