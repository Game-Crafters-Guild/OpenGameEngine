#include "PhysicsECS/PhysicsWorldService.h"

#include "Logger/Logger.h"

namespace GameEngine::PhysicsECS
{
std::unique_ptr<Physics::PhysicsWorld> PhysicsWorldService::s_World;
Physics::PhysicsWorldSettings PhysicsWorldService::s_Settings{};
float32 PhysicsWorldService::s_RenderInterpolationAlpha = 1.0f;
std::uint64_t PhysicsWorldService::s_WorldGeneration = 0;
float32 PhysicsWorldService::s_LastStepMs = 0.0f;
uint32 PhysicsWorldService::s_LastBodiesComposed = 0;
uint32 PhysicsWorldService::s_LastCharactersComposed = 0;

void PhysicsWorldService::Initialize(const Physics::PhysicsWorldSettings& settings)
{
    if (s_World)
        return;
    s_Settings = settings;
    s_RenderInterpolationAlpha = 1.0f;
    s_LastStepMs = 0.0f;
    s_LastBodiesComposed = 0;
    s_LastCharactersComposed = 0;
    ++s_WorldGeneration;
    s_World = std::make_unique<Physics::PhysicsWorld>(settings);
    Logger::Log::Info("[PhysicsECS] PhysicsWorld initialized");
}

void PhysicsWorldService::Shutdown()
{
    s_World.reset();
    s_RenderInterpolationAlpha = 1.0f;
    s_LastStepMs = 0.0f;
    s_LastBodiesComposed = 0;
    s_LastCharactersComposed = 0;
    ++s_WorldGeneration;
    Logger::Log::Info("[PhysicsECS] PhysicsWorld shutdown");
}

Physics::PhysicsWorld& PhysicsWorldService::Get()
{
    return *s_World;
}

Physics::PhysicsWorld* PhysicsWorldService::TryGet()
{
    return s_World.get();
}

bool PhysicsWorldService::IsInitialized()
{
    return s_World != nullptr;
}

const Physics::PhysicsWorldSettings& PhysicsWorldService::GetSettings()
{
    return s_Settings;
}

float32 PhysicsWorldService::GetRenderInterpolationAlpha()
{
    return s_RenderInterpolationAlpha;
}

void PhysicsWorldService::SetRenderInterpolationAlpha(float32 a)
{
    if (a < 0.0f)
        a = 0.0f;
    if (a > 1.0f)
        a = 1.0f;
    s_RenderInterpolationAlpha = a;
}

std::uint64_t PhysicsWorldService::GetWorldGeneration()
{
    return s_WorldGeneration;
}

float32 PhysicsWorldService::GetLastStepMs()
{
    return s_LastStepMs;
}

void PhysicsWorldService::SetLastStepMs(float32 ms)
{
    s_LastStepMs = ms;
}

uint32 PhysicsWorldService::GetLastBodiesComposed()
{
    return s_LastBodiesComposed;
}

void PhysicsWorldService::SetLastBodiesComposed(uint32 count)
{
    s_LastBodiesComposed = count;
}

uint32 PhysicsWorldService::GetLastCharactersComposed()
{
    return s_LastCharactersComposed;
}

void PhysicsWorldService::SetLastCharactersComposed(uint32 count)
{
    s_LastCharactersComposed = count;
}

} // namespace GameEngine::PhysicsECS

