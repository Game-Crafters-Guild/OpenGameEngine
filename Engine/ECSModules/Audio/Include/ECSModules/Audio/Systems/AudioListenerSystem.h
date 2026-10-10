#pragma once

#include "Audio/AudioTypes.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine { namespace Audio { class AudioSystem; } }
namespace GameEngine::Components { struct WorldTransform; }

// Explicit imports from the Audio module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Audio
{
using ::GameEngine::Audio::AudioSystem;
} // namespace GameEngine::Engine::Audio

namespace GameEngine { namespace Engine::Audio {

// Copies translation, world-up (Y column) and local +Z (Z column) out of a
// column-major WorldTransform. This engine is left-handed with +Z forward
// (MakeLookAtLH, C# Transform.Forward), so identity reports (0,0,1).
void FillListenerStateFromWorldTransform(const Components::WorldTransform& wt,
                                         ::GameEngine::Audio::ListenerState& out);

class AudioListenerSystem : public ECS::ISystem
{
public:
    explicit AudioListenerSystem(Audio::AudioSystem* audioSystem) : m_Audio(audioSystem) {}
    const char* GetName() const override { return "AudioListenerSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Audio::AudioSystem* m_Audio = nullptr; // not owned
};

} } // namespace GameEngine::Engine::Audio



