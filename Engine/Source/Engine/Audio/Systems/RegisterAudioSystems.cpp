#include "ECSModules/Audio/Systems/RegisterAudioSystems.h"
#include "ECS/SystemScheduling.h"

#include "ECSModules/Audio/Systems/AudioEmitterSystem.h"
#include "ECSModules/Audio/Systems/AudioListenerSystem.h"

namespace GameEngine { namespace Engine::Audio {
using namespace ::GameEngine::Audio;

void AddAudioSystemsToSchedule(ECS::SystemScheduleBuilder& schedule, Audio::AudioSystem* audioSystem)
{
    using namespace ECS;

    constexpr const char* kListener = "AudioListener";
    constexpr const char* kEmitter = "AudioEmitter";

    // Listener first so doppler and relative motion can be computed consistently.
    schedule.Add<AudioListenerSystem>(kListener, SystemPhase::Late, 0, {}, audioSystem);
    schedule.Add<AudioEmitterSystem>(kEmitter, SystemPhase::Late, 1, {kListener}, audioSystem);
}

} } // namespace GameEngine::Engine::Audio



