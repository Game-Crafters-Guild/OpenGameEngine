// Explicit cross-DLL instantiations for core components reached from outside the
// Engine library: by Engine systems in other modules (LensFlareExtractionSystem) and
// by editor code. Optimized Mach-O links do not retain a reliable implicit
// representative for these templates, so the library that owns the component
// exports them explicitly (the rule ECSTemplates.h documents).

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Transform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::WorldTransform);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Parent);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Light);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::RuntimeOnlyEntity);
// Reached from editor code that EditorTests compiles without the editor's own
// EditorComponentInstantiations.cpp, so the Editor linked and the test binary did not;
// Engine owns these, so Engine exports them. MeshGPUData is the same shape: its only
// definitions in that link came from two test TUs that happen to touch it.
GE_INSTANTIATE_ENGINE_COMPONENT(Components::Name);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::SceneEntityTag);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::LocalBounds);
GE_INSTANTIATE_ENGINE_COMPONENT(Components::MeshGPUData);
} // namespace GameEngine::ECS
