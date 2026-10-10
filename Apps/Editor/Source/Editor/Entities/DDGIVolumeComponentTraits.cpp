#include "Editor/Entities/DDGIVolumeComponentTraits.h"

#include "Components/Rendering/DDGIVolume.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{

void RegisterDDGIVolumeComponentTraits()
{
    EditorComponentTraits traits;
    // "DDGIVolume" reflects to a run-together section title; the spaced form
    // matches how every other volume component reads in the inspector.
    traits.DisplayName = "DDGI Volume";
    // One registration feeds both surfaces: InspectorPanel's ComponentIconClass
    // consults the registry before its built-in chain, and
    // HierarchyEntityIcon's traits scan adds HierarchyRowClass to the entity's
    // row. Both classes resolve to the same icon in views.css, so the
    // inspector section and the hierarchy row cannot drift apart.
    traits.InspectorIconClass = "inspector-section-icon-ddgi-volume";
    traits.HierarchyRowClass = "hierarchy-entity-ddgi-volume";
    EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::DDGIVolume>(), std::move(traits));
}

} // namespace GameEngine::Editor
