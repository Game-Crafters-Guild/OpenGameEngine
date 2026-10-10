#pragma once

// GameSDK/Component.h — the minimal include for the no-boilerplate component path.
//
// A macro-free component only needs the tag base to inherit from:
//
//     #include <GameSDK/Component.h>
//     struct Health : ECS::ComponentBase { float Current = 100.0f; float Max = 100.0f; };
//
// The build-time scanner detects the inheritance (in a header) and generates the
// registration into a separate TU — so the registration macros (GE_REGISTER_COMPONENT,
// ComponentRegistration.h, the field/factory registries) are pulled in only by the
// generated code, NOT by your component header. That keeps user headers light. Use the
// full <GameSDK/GameSDK.h> only when you want the explicit GE_REGISTER_COMPONENT macro.

#include "ECS/ComponentBase.h" // GameEngine::ECS::ComponentBase tag (scanner-detected)

// Short alias so user code can write `ECS::ComponentBase` (and ECS::World, etc.). NOTE:
// this claims the top-level name `ECS`, so a user TU that includes it must not declare its
// own top-level `namespace ECS` / a different `ECS` alias — spell the base fully-qualified
// (`GameEngine::ECS::ComponentBase`) instead if you need to.
namespace ECS = ::GameEngine::ECS;
