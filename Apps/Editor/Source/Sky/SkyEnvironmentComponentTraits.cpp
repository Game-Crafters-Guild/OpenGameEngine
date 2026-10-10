#include "Sky/SkyEnvironmentComponentTraits.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Sky/SkySunLinkUndo.h"

#include <utility>

namespace GameEngine::Editor
{

void RegisterSkyEnvironmentComponentTraits()
{
    EditorComponentTraits traits;
    // The sky drives its sun light's color, so an edit that starts the drive records that color.
    traits.BeforeGenericEdit = SkySunLinkUndo::BeforeGenericEdit;
    EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::SkyEnvironment>(),
                                                  std::move(traits));
}

} // namespace GameEngine::Editor
