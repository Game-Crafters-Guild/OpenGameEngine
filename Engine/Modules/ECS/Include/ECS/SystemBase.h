#pragma once

// ECS::SystemBase — an empty, OPTIONAL tag base for the no-boilerplate user-system path.
// Inheriting it (IN A HEADER) marks a struct as a user C++ system for the build-time
// scanner, which then generates its registration (no macro, mirroring ECS::ComponentBase
// for components):
//
//     #include <GameSDK/System.h>
//     struct SpinnerSystem : ECS::SystemBase {
//         void OnUpdate(ECS::World& w, float dt) {
//             w.Query<ECS::Read<Demo::Spinner>, ECS::Write<Transform>>().Each(
//                 [dt](const Demo::Spinner& s, Transform& xf){
//                     xf.RotationEuler.Y += s.Speed * dt;   // (fields illustrative)
//                 });
//         }
//     };
//
// Access is declared explicitly (D-SDK): Read<T>/Write<T> qualifiers plus
// concrete const-correct parameter types. Generic auto& parameters on
// unqualified component types are a compile error.
//
// LIFECYCLE METHODS are detected by name (all optional), invoked by the engine's user-system
// bridge during PLAY MODE:
//   * void OnStart(ECS::World&)            — once when play begins / after a hot-reload
//   * void OnUpdate(ECS::World&, float dt) — every play-mode frame
//   * void OnDestroy(ECS::World&)          — once when play ends / before a hot-reload unload
// They are duck-typed (matched via `requires` in UserSystemAdapter), not virtual overrides, so
// a system can declare only the ones it needs. The scanner warns if a SystemBase-derived struct
// has none of them.
//
// Unlike ComponentBase, a system is NOT a POD: it may hold member state (caches, handles), but
// that state is reset on every hot-reload (the instance is recreated). Durable world state lives
// in components (which hot-reload-migrate) or in entities the system manages.
//
// Must be in a header (.h/.hpp): the scanner reflects headers (the generated TU #includes them).

namespace GameEngine
{
namespace ECS
{
struct SystemBase
{
};
} // namespace ECS
} // namespace GameEngine
