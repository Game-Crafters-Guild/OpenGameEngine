#include "Editor/Entities/ComponentEnabledToggle.h"

#include "ECS/ComponentFlags.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kToggleComponentEnabledTooltip =
    "Switch this component on or off. While it is off, the engine stops processing it and keeps its values.";
constexpr const char* kMixedClickSwitchesOffTooltip =
    "The selected entities disagree; a click switches all of them off.";
constexpr const char* kMixedClickSwitchesOnTooltip =
    "The selected entities disagree; a click switches all of them on.";

// What the dot switches for a component type.
enum class ComponentSwitch
{
    None,      // no dot: a NotToggleable or unregistered type, or an enable-state tag
    Tag,       // the component's ECS::ComponentDisabled tag
    KeptField, // the type's own bool Enabled field (ECS::ComponentFlags::KeepsOwnEnabledField)
};

ComponentSwitch SwitchOf(ECS::ComponentTypeId typeId, std::uint32_t& outFieldOffset)
{
    if (ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return ComponentSwitch::Tag;
    const ECS::ComponentRegistry::ComponentInfo* info = ECS::ComponentRegistry::GetComponentInfo(typeId);
    if (!info || !ECS::HasAnyFlag(info->Flags, ECS::ComponentFlags::KeepsOwnEnabledField))
        return ComponentSwitch::None;
    outFieldOffset = info->EnabledFieldOffset;
    return ComponentSwitch::KeptField;
}

// Writes the state TryGetComponentEnabled reads.
bool TrySetComponentEnabled(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId, bool enabled)
{
    if (!world.HasComponent(entity, typeId))
        return false;
    std::uint32_t fieldOffset = 0;
    switch (SwitchOf(typeId, fieldOffset))
    {
    case ComponentSwitch::Tag:
        world.SetComponentEnabledImmediate(entity, typeId, enabled);
        return true;
    case ComponentSwitch::KeptField:
    {
        std::vector<uint8_t> bytes;
        if (!world.CaptureComponentBytes(entity, typeId, bytes) || fieldOffset >= bytes.size())
            return false;
        bytes[fieldOffset] = enabled ? 1 : 0;
        return world.ApplyComponentBytesImmediate(entity, typeId, bytes);
    }
    case ComponentSwitch::None:
        break;
    }
    return false;
}

// The dot's tooltip its owner registered for this type, or else the one the component that hosts
// its section registered for its entries; empty when neither did.
std::string RegisteredToggleTooltip(ECS::ComponentTypeId typeId)
{
    const EditorComponentTraitsRegistry& registry = EditorComponentTraitsRegistry::Get();
    EditorComponentTraits traits;
    if (registry.TryGet(typeId, traits) && !traits.EnableToggleTooltip.empty())
        return traits.EnableToggleTooltip;
    ECS::ComponentTypeId hostTypeId = 0;
    EditorComponentTraits hostTraits;
    if (registry.TryGetSectionHost(typeId, hostTypeId, hostTraits))
        return hostTraits.HostedEnableToggleTooltip;
    return {};
}

// The undo step's name: what the click switched, which way, and how many entities it wrote when it
// wrote more than one.
std::string ToggleUndoLabel(std::string_view componentTitle, bool enabled, std::size_t entityCount)
{
    std::string label = "Switch ";
    label += componentTitle;
    label += enabled ? " on" : " off";
    if (entityCount > 1)
        label += " (" + std::to_string(entityCount) + " entities)";
    return label;
}

// One click of the dot over a selection. The state each entity had before the click is the
// snapshot undo puts back, so an entity that was already in the clicked state stays in it. Undo and
// redo announce their change as UndoRedo, the kind that makes the inspector redraw the dot.
class SetComponentEnabledCommand final : public IEditorCommand
{
  public:
    SetComponentEnabledCommand(std::string name, ECS::World* world, EditorChangeNotifications* notifications,
                               std::span<const ECS::EntityHandle> entities, ECS::ComponentTypeId typeId, bool enabled)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_TypeId(typeId),
          m_Enabled(enabled)
    {
        m_Entries.reserve(entities.size());
        for (ECS::EntityHandle entity : entities)
        {
            bool wasEnabled = true;
            if (TryGetComponentEnabled(m_World, entity, m_TypeId, wasEnabled))
                m_Entries.push_back({entity, wasEnabled});
        }
    }

    const char* GetName() const override { return m_Name.c_str(); }

    // False when no selected entity has a switch for the type, or every one already has the clicked
    // state.
    bool ChangesAnything() const
    {
        for (const Entry& entry : m_Entries)
        {
            if (entry.WasEnabled != m_Enabled)
                return true;
        }
        return false;
    }

    void Do() override
    {
        for (const Entry& entry : m_Entries)
            Apply(entry.Entity, m_Enabled, EditorChangeNotifications::ChangeKind::Commit);
    }

    void Undo() override
    {
        for (const Entry& entry : m_Entries)
            Apply(entry.Entity, entry.WasEnabled, EditorChangeNotifications::ChangeKind::UndoRedo);
    }

    void Redo() override
    {
        for (const Entry& entry : m_Entries)
            Apply(entry.Entity, m_Enabled, EditorChangeNotifications::ChangeKind::UndoRedo);
    }

  private:
    struct Entry
    {
        ECS::EntityHandle Entity{};
        bool WasEnabled = true;
    };

    void Apply(ECS::EntityHandle entity, bool enabled, EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_World || !m_World->IsValid(entity))
            return;
        if (TrySetComponentEnabled(*m_World, entity, m_TypeId, enabled) && m_Notifications)
            m_Notifications->NotifyComponentChanged({m_World, entity, m_TypeId, kind});
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;                        // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::ComponentTypeId m_TypeId{};
    bool m_Enabled = true;
    std::vector<Entry> m_Entries;
};
} // namespace

bool TryGetComponentEnabled(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                            bool& outEnabled)
{
    if (!world || !world->HasComponent(entity, typeId))
        return false;
    std::uint32_t fieldOffset = 0;
    switch (SwitchOf(typeId, fieldOffset))
    {
    case ComponentSwitch::Tag:
        outEnabled = world->IsComponentEnabled(entity, typeId);
        return true;
    case ComponentSwitch::KeptField:
    {
        std::vector<uint8_t> bytes;
        if (!world->CaptureComponentBytes(entity, typeId, bytes) || fieldOffset >= bytes.size())
            return false;
        outEnabled = bytes[fieldOffset] != 0;
        return true;
    }
    case ComponentSwitch::None:
        break;
    }
    return false;
}

bool IsComponentEnabledMixed(ECS::World* world, std::span<const ECS::EntityHandle> selection,
                             ECS::ComponentTypeId typeId)
{
    bool anyOn = false;
    bool anyOff = false;
    for (ECS::EntityHandle entity : selection)
    {
        bool enabled = true;
        if (!TryGetComponentEnabled(world, entity, typeId, enabled))
            continue;
        if (enabled)
            anyOn = true;
        else
            anyOff = true;
        if (anyOn && anyOff)
            return true;
    }
    return false;
}

std::string ComponentEnabledToggleTooltip(ECS::World* world, ECS::EntityHandle primary,
                                          std::span<const ECS::EntityHandle> selection, ECS::ComponentTypeId typeId)
{
    std::string tooltip = RegisteredToggleTooltip(typeId);
    if (tooltip.empty())
        tooltip = kToggleComponentEnabledTooltip;
    bool primaryEnabled = true;
    if (IsComponentEnabledMixed(world, selection, typeId) &&
        TryGetComponentEnabled(world, primary, typeId, primaryEnabled))
    {
        tooltip += " ";
        tooltip += primaryEnabled ? kMixedClickSwitchesOffTooltip : kMixedClickSwitchesOnTooltip;
    }
    return tooltip;
}

void CommitComponentEnabledToggle(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                                  ECS::EntityHandle primary, std::span<const ECS::EntityHandle> selection,
                                  ECS::ComponentTypeId typeId, std::string_view componentTitle, bool enabled)
{
    std::vector<ECS::EntityHandle> entities{primary};
    for (ECS::EntityHandle entity : selection)
    {
        if (entity != primary && world.HasComponent(entity, typeId))
            entities.push_back(entity);
    }

    const std::string label = ToggleUndoLabel(componentTitle, enabled, entities.size());
    auto command = std::make_unique<SetComponentEnabledCommand>(label, &world, notifications, entities, typeId, enabled);
    if (!command->ChangesAnything())
        return;
    if (!undo)
    {
        command->Do();
        return;
    }
    CommitGenericEdit(world, *undo, label, [&]() { undo->Execute(std::move(command)); });
}

} // namespace GameEngine::Editor
