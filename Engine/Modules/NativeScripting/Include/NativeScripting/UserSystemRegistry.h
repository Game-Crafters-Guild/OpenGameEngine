#pragma once

// User C++ system registry — the engine-side contract a hot-reloaded user DLL registers its
// systems into, and that the engine's user-system bridge ticks during play mode.
//
// A user authors a system as an ECS::SystemBase-derived struct (see <GameSDK/System.h>); the
// build-time scanner generates a registration TU that wraps each one in a UserSystemAdapter<T>
// (an IUserSystem) and calls RegisterUserSystem at module load. The bridge (Phase C) walks
// GetUserSystems() each play-mode frame.
//
// Registrations carry an owning MODULE id (P1 packages): NativeScriptManager::LoadModule
// brackets a module's registrars with SetActiveRegistrationModule, so reloading one module
// (the project's, or one package's) clears and re-registers ONLY that module's systems while
// every other loaded module's systems survive.

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}

namespace NativeScripting
{

// Type-erased interface the bridge calls. Concrete impls (UserSystemAdapter<T>) live in the
// user DLL and forward to the user struct's optional lifecycle hooks.
class IUserSystem
{
public:
    virtual ~IUserSystem() = default;
    virtual void OnStart(ECS::World& world) = 0;
    virtual void OnUpdate(ECS::World& world, float dt) = 0;
    virtual void OnDestroy(ECS::World& world) = 0;
    virtual const char* Name() const = 0;
    // Optional presentation preparation after engine movement and transforms,
    // before extraction. Runs exclusively on the world's calling thread.
    // Derive extraction inputs (for example renderer membership) from current
    // state; do not advance simulation, change scenes, or rewrite camera,
    // terrain or hierarchy inputs already consumed earlier in this schedule.
    // Deferred component commands remain deferred; no implicit flush occurs.
    virtual void OnPostSimulation(ECS::World&, float) {}
    virtual bool HasPostSimulation() const { return false; }
};

// One registration: the system plus the module that owns it. System is nulled (not erased)
// when a hard fault disables it for the session; iteration skips null slots.
struct UserSystemEntry
{
    IUserSystem* System = nullptr;
    std::string ModuleId;
};

// Stamp subsequent RegisterUserSystem calls with `moduleId`. LoadModule sets this around a
// module's LoadLibrary + Register_v1 (the only paths that run user registrars) and resets it
// after; direct test registrations without a bracket land on the empty module id. Main thread
// only, like the registries themselves.
void SetActiveRegistrationModule(std::string_view moduleId);

// Register a user system under the active registration module. NON-OWNING: the user DLL
// allocates the adapter and the registry keeps the raw pointer. Entries are dropped WITHOUT
// deleting (the adapter leaks with the never-unloaded user DLL — consistent with the
// module-leak model). Called from the user DLL's generated registration at module load.
// Returns true (so the generated TU can seed a file-scope static initializer); false if
// `system` is null.
bool RegisterUserSystem(IUserSystem* system);

// Drop the systems registered by ONE module. Called from LoadModule just before re-running
// that module's registrars, so clear+register are paired per module and reloading one module
// never drops another's systems. Edit-time reloads drop systems WITHOUT OnDestroy (they were
// never started — OnStart/OnDestroy are play-bound); in-play reloads are deferred to
// play-exit, where StopUserSystems fires OnDestroy first.
void ClearUserSystems(std::string_view moduleId);

// Drop ALL registered user systems (tests + shutdown).
void ClearUserSystems();

// The registered user systems, in registration order.
const std::vector<UserSystemEntry>& GetUserSystems();

// Play-mode lifecycle: the editor's PlayModeManager (and the standalone Player around its game
// loop) call StartUserSystems on play-enter, TickUserSystems each play frame, and StopUserSystems
// on play-exit — so user systems run ONLY during play. Each is crash-isolated: a null entry is
// skipped, a throwing hook is logged (by name) and the rest still run, and on Windows a hard fault
// (access violation) in a hook is caught — the faulting system is logged and disabled for the
// session rather than taking down the host (passthrough on other platforms). A disabled system
// returns on the next module hot-reload (per-module clear + re-register rebuilds its entries).
// OnStart/OnUpdate/OnDestroy are forwarded to the user struct only if it declares them.
void StartUserSystems(ECS::World& world);
void TickUserSystems(ECS::World& world, float dt);
void StopUserSystems(ECS::World& world);

// Consume the latest completed native tick for this exact world/reset once.
// A different view/world does not consume it. Stop, module replacement and an
// in-place world reset revoke it. No tick means no post hook (edit/pause/redraw).
// Uses the native tick's delta. Returns true when a late hook was dispatched.
// Ordinary deferred ECS command semantics apply: this is not a command flush.
bool PostSimulateUserSystems(ECS::World& world);

} // namespace NativeScripting
} // namespace GameEngine
