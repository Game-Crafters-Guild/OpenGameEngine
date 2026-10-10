#pragma once

// ModuleRegistration — the process-wide "which module DLL is registering right
// now" stamp (C12 module-reload protocol).
//
// NativeScriptManager::LoadModule brackets a module's registrars (static init
// at LoadLibrary + GE_UserModule_Register_v1) with SetActiveRegistrationModule,
// so every registry a module touches (components, reflected fields, engine
// plugins, scene schemas, schedule registrations) can attribute its entries to
// the owning module and its load GENERATION. The attribution powers three
// things:
//   - load-abort purge: a DLL unmapped after a failed handshake had already run
//     its registrars; its entries must be dropped before the image goes away,
//   - reload reconcile: a re-registration from a NEWER generation of the same
//     module replaces the older generation's entry (handler/system/schema code
//     must come from the newest mapped image),
//   - unload quiesce ledger: a module version may only be unmapped when no
//     registry holds an entry stamped with an older generation of it.
//
// Registrations made outside a bracket (engine static init, tests) carry the
// empty stamp — never purged, never counted stale. Main thread only, matching
// the registries themselves.

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine {
namespace ECS {

struct ModuleRegistrationStamp
{
    std::string ModuleId;     // empty = not module-owned (engine/static/tests)
    std::uint64_t Generation = 0; // bumped by the loader per (re)load of the module

    bool IsSet() const { return !ModuleId.empty(); }

    // True when `other` is an entry of the SAME module from an OLDER load —
    // i.e. it must be replaced on re-registration and blocks unload if it
    // survives the reload pass.
    bool Supersedes(const ModuleRegistrationStamp& other) const
    {
        return IsSet() && other.ModuleId == ModuleId && other.Generation < Generation;
    }

    bool Matches(std::string_view moduleId, std::uint64_t generation) const
    {
        return ModuleId == moduleId && Generation == generation;
    }
};

// Stamp subsequent registrations with {moduleId, generation}. The loader sets
// this around a module's registrars and clears it after (empty id = clear).
void SetActiveRegistrationModule(std::string_view moduleId, std::uint64_t generation);
void ClearActiveRegistrationModule();
const ModuleRegistrationStamp& GetActiveRegistrationModule();

} // namespace ECS
} // namespace GameEngine
