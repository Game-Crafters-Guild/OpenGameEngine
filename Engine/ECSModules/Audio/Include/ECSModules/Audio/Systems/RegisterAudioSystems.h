#pragma once

namespace GameEngine::ECS { class SystemScheduleBuilder; }

namespace GameEngine { namespace Audio { class AudioSystem; } }

// Explicit imports from the Audio module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Audio
{
using ::GameEngine::Audio::AudioSystem;
} // namespace GameEngine::Engine::Audio

namespace GameEngine { namespace Engine::Audio {

// Adds audio systems to a dependency-aware schedule builder (does not build/register).
void AddAudioSystemsToSchedule(ECS::SystemScheduleBuilder& schedule, Audio::AudioSystem* audioSystem);

} } // namespace GameEngine::Engine::Audio


