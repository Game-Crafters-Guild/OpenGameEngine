#include "Scripting/PhysicsABI.h"

#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "Logger/Logger.h"

#include "PhysicsECS/PhysicsWorldService.h"

namespace
{
static GE_Result EnsureEngineInitializedForPhysicsAbi()
{
    return GameEngine::DllBootstrap::EnsureEngineInitialized();
}
} // namespace

extern "C"
{

GE_API GE_Result GE_CDECL GE_PhysicsABI_GetDefaultWorld(GE_Handle* outWorld)
{
    try
    {
        if (!outWorld)
            return GE_Result_InvalidArg;
        if (EnsureEngineInitializedForPhysicsAbi() != GE_Result_Ok)
            return GE_Result_Fail;

        GameEngine::PhysicsECS::PhysicsWorldService::Initialize();
        auto* w = GameEngine::PhysicsECS::PhysicsWorldService::TryGet();
        if (!w)
            return GE_Result_Fail;

        *outWorld = static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(w));
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_PhysicsABI_StepWorld(GE_Handle world, float deltaTime, int32_t collisionSteps)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;
        if (collisionSteps < 1)
            collisionSteps = 1;

        auto* w = reinterpret_cast<GameEngine::Physics::PhysicsWorld*>(static_cast<uintptr_t>(world));
        if (!w)
            return GE_Result_Fail;

        w->Step(deltaTime, collisionSteps);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_Physics_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes)
{
    try
    {
        if (!outTable || !outSizeBytes)
            return GE_Result_InvalidArg;

        const uint32_t major = GE_PHYSICS_ABI_VERSION_MAJOR(abiVersion);
        const uint32_t curMajor = GE_PHYSICS_ABI_VERSION_MAJOR(GE_PHYSICS_ABI_VERSION_CURRENT);
        if (major != curMajor)
        {
            *outTable = nullptr;
            *outSizeBytes = 0;
            return GE_Result_NotFound;
        }

        static GE_Physics_Interface_v1 s_Iface;
        s_Iface.sizeBytes = sizeof(GE_Physics_Interface_v1);
        s_Iface.abiVersion = GE_PHYSICS_ABI_VERSION_CURRENT;
        s_Iface.GetDefaultWorld = reinterpret_cast<GE_PhysicsABI_GetDefaultWorld_Fn>(&GE_PhysicsABI_GetDefaultWorld);
        s_Iface.StepWorld = reinterpret_cast<GE_PhysicsABI_StepWorld_Fn>(&GE_PhysicsABI_StepWorld);

        *outTable = &s_Iface;
        *outSizeBytes = sizeof(GE_Physics_Interface_v1);
        return GE_Result_Ok;
    }
    catch (...)
    {
        if (outTable)
            *outTable = nullptr;
        if (outSizeBytes)
            *outSizeBytes = 0;
        return GE_Result_Fail;
    }
}

} // extern "C"

