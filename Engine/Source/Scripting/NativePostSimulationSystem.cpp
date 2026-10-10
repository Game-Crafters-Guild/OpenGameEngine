#include "Scripting/NativePostSimulationSystem.h"
#include "ECS/Entity.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "NativeScripting/UserSystemRegistry.h"

namespace GameEngine
{
void NativePostSimulationSystem::Update(ECS::World& world, float32)
{
    if (!NativeScripting::PostSimulateUserSystems(world))
        return;
    if (m_RenderServices)
    {
        auto& textures = m_RenderServices->Textures();
        // A late hook may create its lease for the first time in this tick.
        textures.ObserveCpuTextureWorld({world.GetWorldId(), world.GetLifecycleResetGeneration()});
        textures.FlushCpuTextureUploads();
    }
}
} // namespace GameEngine
