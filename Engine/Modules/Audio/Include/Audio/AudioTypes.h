#pragma once

#include <cstdint>

namespace GameEngine::Audio
{
using AudioWorldId = std::uint16_t;
using AudioBusId = std::uint16_t;

enum class AudioLoadPolicy : std::uint8_t
{
    Auto = 0,        // engine decides (default)
    DecodeToPCM,     // fully decode at load
    Stream,          // never fully decode; stream/decode on demand
};

struct AudioRuntimeSettings
{
    AudioLoadPolicy loadPolicy = AudioLoadPolicy::Auto;
    bool allowVirtualization = true;
};

struct AudioSystemConfig
{
    std::uint32_t maxListeners = 4;
    std::uint32_t maxEmitters = 4096;
    std::uint32_t maxVoices = 256;
    std::uint32_t mixSampleRate = 48000;
    std::uint32_t mixBufferSizeFrames = 512;
    bool enable3D = true;
};

struct ListenerState
{
    float position[3] = {0, 0, 0};
    float forward[3] = {0, 0, 1}; // local +Z; matches WorldTransform column 2
    float up[3] = {0, 1, 0};
    float velocity[3] = {0, 0, 0};
};

struct PlayOptions
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatialized = false;
    AudioBusId bus = 0;
};

} // namespace GameEngine::Audio



