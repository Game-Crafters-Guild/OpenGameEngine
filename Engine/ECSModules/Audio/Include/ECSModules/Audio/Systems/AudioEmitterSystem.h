#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine { namespace Audio { class AudioSystem; } }

// Explicit imports from the Audio module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Audio
{
using ::GameEngine::Audio::AudioSystem;
} // namespace GameEngine::Engine::Audio

namespace GameEngine { namespace Engine::Audio {

class AudioEmitterSystem : public ECS::ISystem
{
public:
    explicit AudioEmitterSystem(Audio::AudioSystem* audioSystem);
    ~AudioEmitterSystem() override;
    const char* GetName() const override { return "AudioEmitterSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    Audio::AudioSystem* m_Audio = nullptr; // not owned
};

} } // namespace GameEngine::Engine::Audio


