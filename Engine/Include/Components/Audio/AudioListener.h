#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Marks an entity as an audio listener.
// The Audio ECS systems will push listener state into the engine-owned AudioSystem each frame.
struct AudioListener
{
    uint8 listenerIndex = 0; // 0..(AudioSystemConfig::maxListeners-1)
    uint16 worldId = 0;      // reserved for future multi-world listener routing
};

static_assert(std::is_trivially_copyable_v<AudioListener>, "AudioListener must be trivially copyable");
static_assert(std::is_standard_layout_v<AudioListener>, "AudioListener must be standard layout");

} // namespace GameEngine::Components



