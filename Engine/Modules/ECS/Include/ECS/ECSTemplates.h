#pragma once

// ECSTemplates.h - Header-only template implementations for user-side instantiation
// Include this file in your .cpp files to get access to ECS template implementations
// for custom components with C++20 auto-registration.

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/AutoRegistration.h"

// Include the template implementations
#include "ECS/WorldTemplateImplementations.inl"

// ============================================================================
// USER GUIDE FOR C++20 AUTO-REGISTRATION
// ============================================================================
//
// To use custom components with auto-registration:
//
// 1. Define your component as a POD struct/class:
//    struct MyComponent {
//        float value;
//        int count;
//    };
//
// 2. Include this header in your .cpp file:
//    #include "ECS/ECSTemplates.h"
//
// 3. Use your component - it will be automatically registered:
//    auto entity = world->create();
//    entity.set(MyComponent{42.0f, 5});  // Auto-registered!
//    auto* comp = entity.get<MyComponent>();  // Works perfectly!
//
// ✅ BENEFITS:
// - Zero manual registration required
// - Same API for built-in and custom components
// - Full C++20 type safety with concepts
// - Zero runtime overhead
// - No engine modifications needed
//
// ============================================================================
// CROSS-DLL EXPLICIT INSTANTIATION FOR ENGINE-BUILT-IN COMPONENTS
// ============================================================================
//
// `World::AddComponent<T>` and the other typed World operations are templates.
// Inside a single user DLL this is fine — every TU that includes
// `ECSTemplates.h` and uses `World::AddComponent<MyComponent>` instantiates the
// template locally and the linker deduplicates within the DLL.
//
// **Cross-DLL is different.** Engine modules (PathfindingECS, PhysicsECS, ...)
// are OBJECT libs spliced into Engine.dll. Their TUs implicitly instantiate
// `World::AddComponent<NavigationGrid>` etc. when they use those components.
// Consumers (Editor.exe, Player.exe, GameEngine.Native.dll) also write
// `World::AddComponent<NavigationGrid>` in inspector / scene-load code, which
// triggers their own implicit instantiation.
//
// On Windows, MSVC's weakly-defined COMDAT symbols survive cross-DLL binding
// and the linker picks a representative without complaint. On macOS (Mach-O)
// and Linux (ELF), the linker is stricter: implicit template instantiations
// in OBJECT-lib TUs aren't always exported with default visibility, so
// Editor.exe sees an unresolved external when it can't find an imported
// instantiation for what it locally tried to instantiate.
//
// **Fix:** the OBJECT lib that OWNS the component types provides one
// `*ComponentInstantiations.cpp` file containing explicit instantiations.
// On Mach-O and ELF those are exported from the Engine library and consumers
// import them by name. On Windows they are inert: the .def generator
// (cmake/GenerateEngineExportsDef.cmake) strips every template instantiation
// from Engine.dll's export table, so each consumer keeps the COMDAT copy it
// instantiated itself — which is what already linked there.
// Auto-registration via `AutoComponentRegistrar<T>` is unaffected — it still
// fires when Engine.dll's TUs reference the component, and
// `ComponentRegistry::RegisterComponent` is already idempotent so duplicate
// calls from a consumer-side re-instantiation would be harmless anyway.
//
// Use the macro below in each engine ECS module's
// `<Module>ComponentInstantiations.cpp`:
//
//   #include "PhysicsECS/Components/PhysicsBody.h"
//   #include "ECS/ECSTemplates.h"
//   namespace GameEngine::ECS {
//       GE_INSTANTIATE_ENGINE_COMPONENT(Components::PhysicsBody);
//       // ... one line per public component owned by this module ...
//   }
//
// This is the only required boilerplate for engine modules whose components
// are consumed cross-DLL. User-defined components in user DLLs do NOT need
// this — they live in one DLL and auto-registration covers them fully.
//
// Component handlers are type-erased and defined in ECS; these instantiations
// supply only the typed World operations consumed across module boundaries.
#define GE_INSTANTIATE_ENGINE_COMPONENT(ComponentType)                                                  \
    template void World::AddComponent<ComponentType>(EntityHandle, const ComponentType&);               \
    template void World::AddComponentImmediate<ComponentType>(EntityHandle, const ComponentType&);     \
    template ComponentType* World::GetComponentForWrite<ComponentType>(EntityHandle);                   \
    template const ComponentType* World::GetComponent<ComponentType>(EntityHandle) const;               \
    template bool World::HasComponent<ComponentType>(EntityHandle) const;                               \
    template void World::RemoveComponent<ComponentType>(EntityHandle);                                  \
    template void World::RemoveComponentImmediate<ComponentType>(EntityHandle)
