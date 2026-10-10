// Explicit cross-DLL instantiation for DDGIVolume — needed now that the
// editor (Apps/Editor/Source/SceneView/DDGIVolumeGizmo.cpp) queries it
// directly from outside the Engine DLL. Optimized Mach-O links do not retain
// a reliable implicit representative for these templates (CoreComponentInstantiations.cpp's
// doc), so a cross-DLL consumer needs Engine to own and export this
// explicitly. Component-only authoring means DDGIVolume itself stays plain,
// scanner-registered POD — this is purely the ABI-export boilerplate
// ECSTemplates.h documents as required whenever an engine component gains
// its first cross-DLL consumer.

#include "Components/Rendering/DDGIVolume.h"
#include "Components/Rendering/GIEmitter.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::DDGIVolume);
// The editor's Add/Remove Component path reaches this from Apps/Editor, so it
// needs the same cross-DLL instantiation an optimized Mach-O link will not
// synthesize (see CoreComponentInstantiations.cpp).
GE_INSTANTIATE_ENGINE_COMPONENT(Components::GIEmitter);
} // namespace GameEngine::ECS
