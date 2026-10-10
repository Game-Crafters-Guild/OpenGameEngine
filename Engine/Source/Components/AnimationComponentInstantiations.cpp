// Explicit cross-DLL instantiation for the animation retargeting components.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
//
// Without this, consumers linking Engine.dll only (HumanoidRetargetSystemTests
// and any future Engine-only tool touching these components) fail to resolve
// World::AddComponent<T> symbols — previously masked by
// test executables splicing the rendering OBJECT libraries beside Engine.dll.

#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"

#include "ECS/ECSTemplates.h"

namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::HumanoidRetargeterComponent);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::SkeletonRef);
} // namespace GameEngine::ECS
