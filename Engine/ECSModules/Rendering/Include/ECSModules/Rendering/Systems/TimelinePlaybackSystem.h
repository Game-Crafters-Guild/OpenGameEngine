#pragma once

#include "ECS/Systems.h"

namespace GameEngine
{
namespace Audio
{
class AudioSystem;
}
namespace Engine::Renderer
{

// Advances Animator timeline playback, evaluates .timeline assets each frame,
// and applies animation clips, transform/property samples, audio events, and
// registered method callbacks.
class TimelinePlaybackSystem : public ECS::ISystem
{
public:
    explicit TimelinePlaybackSystem(Audio::AudioSystem* audioSystem = nullptr);
    const char* GetName() const override { return "TimelinePlaybackSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Audio::AudioSystem* m_AudioSystem = nullptr;
};

} // namespace Engine::Renderer
} // namespace GameEngine
