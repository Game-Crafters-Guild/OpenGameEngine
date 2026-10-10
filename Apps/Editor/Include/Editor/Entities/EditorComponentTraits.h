#pragma once

// EditorComponentTraits — data-level editor behavior for a component type,
// registered by the component's owning editor code or package module. Each
// field is a per-component hook the editor consults without naming the
// component: the inspector display name, icon, category and tooltip, whether it has
// a section of its own, the hierarchy row class, pick-root and gizmo-pivot
// behavior, whether the section leads the inspector, which sections it hosts,
// what switching the component off means, and the undo record for what a
// generic edit makes the component's system overwrite on other entities.
//
// Lives in EditorSDK.dll so Editor.exe and editor-kind package modules share
// one registry instance. Registration is replace-by-typeId (module hot-reload
// re-registers; the old module DLL stays mapped, so replaced callables never
// dangle).

#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

// Runs once a generic edit is applied, inside the edit's undo step (see
// EditorComponentTraits::BeforeGenericEdit). `label` names that step.
using GenericEditUndoRecorder = std::function<void(UndoRedoService& undo, const std::string& label)>;

struct EditorComponentTraits
{
    // Inspector section title (empty = fall back to the built-in name table /
    // reflected type name).
    std::string DisplayName;
    // CSS class for the inspector section icon (empty = built-in chain).
    std::string InspectorIconClass;
    // Add Component menu category the component is listed under (empty = the
    // built-in category table).
    std::string InspectorCategory;
    // Inspector section header tooltip (empty = the built-in tooltip table).
    std::string InspectorTooltip;
    // The component gets no inspector section of its own: another component's
    // inspector shows what an author needs of it.
    bool HideInInspector = false;
    // CSS class added to the entity's hierarchy row when the component is
    // present (empty = no contribution).
    std::string HierarchyRowClass;
    // The entity is its own pick target: the instance-root upward walk stops
    // here instead of grouping it under a model root.
    bool IsPickInstanceRoot = false;
    // The transform gizmo pivots at the entity origin (the local pivot
    // transformed to world), not at rendered-content anchors.
    bool GizmoPivotAtEntityOrigin = false;
    // The inspector section leads the entity's sections, above Transform: for
    // a component that is the one thing an author edits on an entity whose
    // other sections are generated (a fence's span piece).
    bool LeadsInspector = false;
    // For a component that owns a stack of other components, the way a volume
    // owns its effects: true for each component type that is an entry of that
    // stack. On an entity with this component, an entry's inspector section
    // renders inside this component's section and drag-reorders among the other
    // entries there. Register refuses traits that claim a component type,
    // registered before them, that another host already claims; a type
    // registered after two hosts that claim it is not refused (#2216). Unset =
    // the section hosts no other sections.
    std::function<bool(ECS::ComponentTypeId)> HostsInspectorSection;
    // CSS class added to the body of each section this one hosts (empty = none).
    std::string HostedSectionBodyClass;
    // The enable dot's tooltip for every entry this component hosts that registers none of
    // its own, in the form of EnableToggleTooltip below, naming what it switches as an effect
    // ("Switch this effect on or off. Off ..."). Empty = the generic tooltip.
    std::string HostedEnableToggleTooltip;
    // An entry on an entity without this component is inactive and stays a
    // top-level section; its header carries this badge, with this warning as the
    // badge's and the header's tooltip.
    std::string MissingHostBadge;
    std::string MissingHostWarning;
    // The section header's enable dot tooltip for this type, replacing the generic
    // one: what the dot switches, then what off does, as in "Switch this component on
    // or off. Off stops its sound." (ComponentEnabledToggleTooltip). Empty = the
    // generic tooltip.
    std::string EnableToggleTooltip;
    // For a component whose system writes to other entities (a sky drives its
    // linked light's color): editor code that edits without knowing the
    // component (a component's enable toggle, an entity's creation) calls
    // this before the edit and runs the recorder it returns once the edit is
    // applied, inside the same undo step, so undoing the edit also puts back
    // what the system overwrites because of it. Called before
    // every such edit, whatever entity it touches: what the system writes
    // follows from the whole world, not from the edited entity alone. Unset,
    // or an empty recorder, records nothing.
    std::function<GenericEditUndoRecorder(ECS::World&)> BeforeGenericEdit;
};

class EditorComponentTraitsRegistry
{
public:
    static EditorComponentTraitsRegistry& Get();

    // Replace-semantics: a re-registration for the same type id overwrites the
    // previous traits (module hot-reload replays its registrations). Traits whose
    // HostsInspectorSection claims a registered component type that another
    // component's traits already host are refused with an error naming both
    // hosts and the type.
    void Register(ECS::ComponentTypeId typeId, EditorComponentTraits traits);

    // Copy-out lookup: callers get a stable snapshot, never a pointer into the
    // map (registrations can land at any time from module loads).
    bool TryGet(ECS::ComponentTypeId typeId, EditorComponentTraits& outTraits) const;

    // The component whose traits host `hostedTypeId`'s inspector section (see
    // EditorComponentTraits::HostsInspectorSection), with its traits; false when
    // no registered component hosts it. Register keeps the host unique for a
    // component type registered before its hosts; for a type registered after
    // two hosts that claim it, the first match in map order answers (#2216).
    bool TryGetSectionHost(ECS::ComponentTypeId hostedTypeId, ECS::ComponentTypeId& outHostTypeId,
                           EditorComponentTraits& outHostTraits) const;

    // Snapshot of (typeId, traits) pairs for presence scans (hierarchy row
    // classes, pick roots). Small: one entry per plugin-owned component.
    std::vector<std::pair<ECS::ComponentTypeId, EditorComponentTraits>> Snapshot() const;

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every entry attributed to `moduleId` (the traits' accessor callables are
    // module code and pin its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorComponentTraitsRegistry() = default;

    mutable std::mutex m_Mutex;
    // Held for a whole Register, so a host's claim check and its insert are one
    // step; the claim predicates run outside m_Mutex.
    std::mutex m_RegistrationMutex;
    std::unordered_map<ECS::ComponentTypeId, EditorComponentTraits> m_Traits;
    // Module stamp per registration (attribution is registry bookkeeping, not
    // part of the module-facing traits struct).
    std::unordered_map<ECS::ComponentTypeId, ECS::ModuleRegistrationStamp> m_ModuleOwners;
};

} // namespace GameEngine::Editor
