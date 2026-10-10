#pragma once

// GameSDK/System.h — author a user C++ ECS system (the behavior half of scripting).
//
//     #include <GameSDK/System.h>
//     struct SpinnerSystem : ECS::SystemBase {
//         void OnUpdate(ECS::World& w, float dt) {
//             w.Query<ECS::Read<Demo::Spinner>, ECS::Write<GameEngine::Components::Transform>>().Each(
//                 [dt](const Demo::Spinner& s, GameEngine::Components::Transform& xf){
//                     xf.RotationEuler.Y += s.Speed * dt;   // (component fields illustrative)
//                 });
//         }
//     };
//
// Access is declared explicitly (D-SDK): Read<T>/Write<T> query qualifiers plus concrete
// const-correct parameter types. Generic auto& parameters on unqualified component types are
// a compile error — the change filters need honest read/write intent.
//
// The build-time scanner detects `: ECS::SystemBase` and generates the registration (no macro).
//
// For a per-entity system, inherit ECS::EntitySystem<Self> and write a ForEach(Comp&..., float dt)
// instead of OnUpdate — the engine builds the Query from ForEach's parameters and runs it for every
// matching entity. See EntitySystem below.
//
// HEAVIER than <GameSDK/Component.h> on purpose: this pulls the typed World/Query API so OnUpdate
// can call w.Query<T>().Each(...). Include it ONLY in TUs that define systems — keep component-only
// headers on the light <GameSDK/Component.h> so their compiles stay fast (the PCH does not pull this).

#include "ECS/SystemBase.h"                      // ECS::SystemBase tag (scanner-detected)
#include "ECS/ECS.h"                             // EntityHandle, Component concept, GetComponentTypeId
#include "ECS/Entity.h"                          // World
#include "ECS/World.h"                           // Archetype / table
#include "ECS/Query.h"                           // Query
#include "ECS/ECSTemplates.h"                    // Query::Each template implementations
#include "NativeScripting/UserSystemRegistry.h"  // IUserSystem + RegisterUserSystem

#include <cstddef>     // std::size_t
#include <tuple>       // ForEach-parameter introspection
#include <type_traits> // std::remove_cvref_t, std::is_same_v
#include <utility>     // std::index_sequence

namespace GameEngine
{
namespace ECS
{

namespace Detail
{
// Maps a ForEach member-function pointer to a Query over its COMPONENT parameters. ForEach is
// authored as `void ForEach(Comp1&, Comp2&, ..., float dt)`; the leading reference parameters are
// the queried components and the trailing `float` is the frame delta. We drop the last parameter,
// strip cv/ref to recover the component types, build Query<Comp...>, and forward each entity's
// components (plus dt) back to ForEach. All compile-time — no codegen, no per-entity allocation.
template <class Derived, class MemFn>
struct EntitySystemForEach;

template <class Derived, class R, class... Args>
struct EntitySystemForEach<Derived, R (Derived::*)(Args...)>
{
    using Args_t = std::tuple<Args...>;
    static constexpr std::size_t kCount = sizeof...(Args);
    static_assert(kCount >= 2,
                  "EntitySystem::ForEach must take at least one component reference and a trailing 'float dt'");
    static_assert(std::is_same_v<std::remove_cvref_t<std::tuple_element_t<kCount - 1, Args_t>>, float>,
                  "EntitySystem::ForEach's last parameter must be 'float dt' (the per-frame delta time)");

    template <std::size_t... I>
    static void Run(Derived& self, World& world, float dt, std::index_sequence<I...>)
    {
        // The lambda parameters carry ForEach's own reference types, so a
        // `const Comp&` ForEach parameter is inferred (and invoked) as a read
        // — the user's declared intent flows into the change filters.
        world.Query<std::remove_cvref_t<std::tuple_element_t<I, Args_t>>...>().Each(
            [&self, dt](std::tuple_element_t<I, Args_t>... components)
            { self.ForEach(components..., dt); });
    }
};
} // namespace Detail

// Per-entity user system. Inherit EntitySystem<Self> and write a single
//
//     void ForEach(Demo::Health& hp, float dt) { hp.Current -= 10.0f * dt; }
//
// The queried components are INFERRED from ForEach's parameters (the trailing `float dt` is the
// frame delta), so there is no manual Query<...> and no boilerplate. EntitySystem derives
// SystemBase, so the build-time scanner detects + registers it and the engine bridge ticks it in
// play mode — identical lifecycle to a global SystemBase system, just per-entity iteration.
//
// ForEach must be one non-template, non-const member (so &Self::ForEach is unambiguous). Optional
// OnStart(World&)/OnDestroy(World&) may also be declared and are forwarded as usual.
template <class Derived>
struct EntitySystem : SystemBase
{
    void OnUpdate(World& world, float dt)
    {
        using ForEachInfo = Detail::EntitySystemForEach<Derived, decltype(&Derived::ForEach)>;
        ForEachInfo::Run(static_cast<Derived&>(*this), world, dt,
                         std::make_index_sequence<ForEachInfo::kCount - 1>{});
    }
};

// Wraps a user SystemBase-derived struct T as an IUserSystem the engine bridge can tick. Each
// lifecycle hook is forwarded only if T declares a compatible one (duck-typed via `requires`),
// so a system implements just the hooks it needs. Instantiated once per system in the generated
// registration TU — which is why THAT TU, not the light PCH, pays the Query template cost.
template <class T>
class UserSystemAdapter final : public ::GameEngine::NativeScripting::IUserSystem
{
public:
    explicit UserSystemAdapter(const char* name) : m_Name(name) {}

    void OnStart(World& world) override
    {
        if constexpr (requires(T& s, World& w) { s.OnStart(w); })
            m_System.OnStart(world);
    }
    void OnUpdate(World& world, float dt) override
    {
        if constexpr (requires(T& s, World& w, float d) { s.OnUpdate(w, d); })
            m_System.OnUpdate(world, dt);
    }
    void OnDestroy(World& world) override
    {
        if constexpr (requires(T& s, World& w) { s.OnDestroy(w); })
            m_System.OnDestroy(world);
    }
    const char* Name() const override { return m_Name; }
    void OnPostSimulation(World& world, float dt) override
    {
        if constexpr (requires(T& s, World& w, float d) { s.OnPostSimulation(w, d); })
            m_System.OnPostSimulation(world, dt);
    }
    bool HasPostSimulation() const override
    {
        return requires(T& s, World& w, float d) { s.OnPostSimulation(w, d); };
    }

private:
    T m_System;
    const char* m_Name;
};

} // namespace ECS
} // namespace GameEngine

// Short alias so user code can write `ECS::SystemBase`, `ECS::World`, etc. Identical to (and
// compatible with) the alias in <GameSDK/Component.h> when a TU includes both.
namespace ECS = ::GameEngine::ECS;
