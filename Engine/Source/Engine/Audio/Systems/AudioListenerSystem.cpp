#include "ECSModules/Audio/Systems/AudioListenerSystem.h"

#include "Audio/AudioSystem.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"

namespace GameEngine { namespace Engine::Audio {
using namespace ::GameEngine::Audio;

using GameEngine::Components::AudioListener;
using GameEngine::Components::WorldTransform;

void FillListenerStateFromWorldTransform(const WorldTransform& wt, Audio::ListenerState& out)
{
    const float32* m = wt.matrix; // column-major

    out.position[0] = m[12];
    out.position[1] = m[13];
    out.position[2] = m[14];

    out.up[0] = m[4];
    out.up[1] = m[5];
    out.up[2] = m[6];

    // Local +Z is forward (LH). Same world-space vector miniaudio uses for
    // emitter positions, so stereo front matches the camera look.
    out.forward[0] = m[8];
    out.forward[1] = m[9];
    out.forward[2] = m[10];
}

void AudioListenerSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_Audio || !m_Audio->IsInitialized())
        return;

    auto q = world.Query<ECS::Read<AudioListener>, ECS::Optional<WorldTransform>>();
    q.Each([&](ECS::EntityHandle /*e*/, const AudioListener& listener, const WorldTransform* wt)
           {
               Audio::ListenerState s{};
               if (wt)
               {
                   FillListenerStateFromWorldTransform(*wt, s);
               }

               m_Audio->SetListener(static_cast<Audio::AudioWorldId>(listener.worldId),
                                    static_cast<uint32>(listener.listenerIndex),
                                    s);
           });
}

} } // namespace GameEngine::Engine::Audio



