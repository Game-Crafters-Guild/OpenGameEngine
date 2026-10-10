#include "NativeScripting/UserSystemRegistry.h"

#include "GuardedUserInvoke.h"
#include "Logger/Logger.h"
#include "ECS/World.h"
#include "ECS/Entity.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace GameEngine
{
namespace NativeScripting
{
namespace
{
// Single process-wide registry, owned by Engine.dll. The user DLL registers into it across the
// DLL boundary (RegisterUserSystem is exported); the bridge reads it. Function-local static so
// it is constructed on first use regardless of static-init order.
std::vector<UserSystemEntry>& Systems()
{
    static std::vector<UserSystemEntry> systems;
    return systems;
}

// The module id stamped onto registrations (set by LoadModule around a module's registrars).
std::string& ActiveModule()
{
    static std::string active;
    return active;
}

struct PostSimulationTick
{
    uint64 WorldId = 0;
    uint64 ResetGeneration = 0;
    float DeltaTime = 0.0f;
    bool Ready = false;
};
// Only scalar identity crosses the app-update / engine-simulation boundary.
// No world pointer or user-module callback is retained here.
uint64 s_StartedWorldId = 0;
PostSimulationTick s_PendingPostSimulation;

// Invoke a lifecycle hook on every registered system, isolated so a buggy script can't take down
// the host (most importantly not during play-exit teardown, where a crash would lose unsaved scene
// edits). Two layers: the inner try/catch logs a thrown C++ exception and lets the rest run;
// GuardedUserInvoke additionally catches a HARD fault (access violation) on Windows and, on fault,
// the system is logged and disabled for the session (its slot is nulled and skipped thereafter).
// Off Windows the guard is a passthrough — a hard fault dies via the process signal handlers.
template <class Fn>
void ForEachSystem(const char* hook, Fn&& invoke)
{
    std::vector<UserSystemEntry>& systems = Systems();
    for (std::size_t i = 0; i < systems.size(); ++i)
    {
        IUserSystem* system = systems[i].System;
        if (!system)
            continue;

        const bool completed = GuardedUserInvoke(
            [&]
            {
                try
                {
                    invoke(system);
                }
                catch (const std::exception& e)
                {
                    Logger::Log::Error("[NativeScripting] user system '{}' {} threw: {}", system->Name(), hook,
                                       e.what());
                }
                catch (...)
                {
                    Logger::Log::Error("[NativeScripting] user system '{}' {} threw a non-standard exception",
                                       system->Name(), hook);
                }
            });

        if (!completed)
        {
            Logger::Log::Error("[NativeScripting] user system '{}' {} hard-faulted (access violation); "
                               "disabling it for this session — fix required",
                               system->Name(), hook);
            systems[i].System = nullptr; // the null-skip at the top of the loop drops it on later frames
        }
    }
}
} // namespace

void SetActiveRegistrationModule(std::string_view moduleId)
{
    ActiveModule().assign(moduleId);
}

bool RegisterUserSystem(IUserSystem* system)
{
    if (!system)
        return false;
    Systems().push_back(UserSystemEntry{system, ActiveModule()});
    return true;
}

void ClearUserSystems(std::string_view moduleId)
{
    s_PendingPostSimulation = {};
    // Non-owning: drop the entries without deleting. The adapter objects were allocated in the
    // user DLL and leak with it (never unloaded today); deleting here would be a cross-DLL free.
    std::vector<UserSystemEntry>& systems = Systems();
    systems.erase(std::remove_if(systems.begin(), systems.end(),
                                 [moduleId](const UserSystemEntry& e) { return e.ModuleId == moduleId; }),
                  systems.end());
}

void ClearUserSystems()
{
    s_StartedWorldId = 0;
    s_PendingPostSimulation = {};
    Systems().clear();
}

const std::vector<UserSystemEntry>& GetUserSystems()
{
    return Systems();
}

void StartUserSystems(ECS::World& world)
{
    s_StartedWorldId = world.GetWorldId();
    s_PendingPostSimulation = {};
    ForEachSystem("OnStart", [&world](IUserSystem* s) { s->OnStart(world); });
}

void TickUserSystems(ECS::World& world, float dt)
{
    const uint64 worldId = world.GetWorldId();
    const uint64 reset = world.GetLifecycleResetGeneration();
    if (worldId == s_StartedWorldId)
        s_PendingPostSimulation = {worldId, reset, dt, false};
    ForEachSystem("OnUpdate", [&world, dt](IUserSystem* s) { s->OnUpdate(world, dt); });
    // A scene transition inside OnUpdate must not deliver a late callback
    // against a different scene from the one whose tick just began.
    if (worldId == s_StartedWorldId && reset == world.GetLifecycleResetGeneration() &&
        s_PendingPostSimulation.WorldId == worldId)
        s_PendingPostSimulation.Ready = true;
}

void StopUserSystems(ECS::World& world)
{
    if (s_StartedWorldId == world.GetWorldId())
    {
        s_StartedWorldId = 0;
        s_PendingPostSimulation = {};
    }
    ForEachSystem("OnDestroy", [&world](IUserSystem* s) { s->OnDestroy(world); });
}

bool PostSimulateUserSystems(ECS::World& world)
{
    if (!s_PendingPostSimulation.Ready || s_PendingPostSimulation.WorldId != world.GetWorldId())
        return false;
    const PostSimulationTick tick = s_PendingPostSimulation;
    s_PendingPostSimulation = {}; // consume before invoking any user code
    if (tick.WorldId != s_StartedWorldId || tick.ResetGeneration != world.GetLifecycleResetGeneration())
        return false;
    bool dispatched = false;
    ForEachSystem("OnPostSimulation", [&](IUserSystem* system)
    {
        if (tick.WorldId == s_StartedWorldId && tick.ResetGeneration == world.GetLifecycleResetGeneration() &&
            system->HasPostSimulation())
        {
            dispatched = true;
            system->OnPostSimulation(world, tick.DeltaTime);
        }
    });
    return dispatched;
}

} // namespace NativeScripting
} // namespace GameEngine
