// Explicit cross-DLL instantiation for the mark-up components MarkupECS owns.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT): editor
// code that reads a mark-up through World::GetComponent without including
// ECSTemplates.h (MarkupEditorSurfaces) imports these by name on macOS and Linux.
#include "Components/Markup/Markup.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::Markup);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::MarkupVolume);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::MarkupRegion);
} // namespace GameEngine::ECS
