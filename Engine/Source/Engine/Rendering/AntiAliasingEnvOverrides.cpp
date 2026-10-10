#include "Engine/Rendering/AntiAliasingEnvOverrides.h"

#include "Logger/Logger.h"

#include <cstdlib>
#include <cstring>

namespace GameEngine::Engine::Renderer
{

AntiAliasingEnvOverrides AntiAliasingEnvOverrides::Read()
{
    AntiAliasingEnvOverrides overrides;

    if (const char* env = std::getenv("GE_MSAA_SAMPLES"))
    {
        const int value = std::atoi(env);
        if (value == 1 || value == 2 || value == 4 || value == 8)
            overrides.MsaaSamples = static_cast<uint32>(value);
        else
            Logger::Log::Warning("GE_MSAA_SAMPLES '{}' not recognized (1|2|4|8)", env);
    }

    if (const char* env = std::getenv("GE_AA_MODE"))
    {
        if (std::strcmp(env, "off") == 0)
            overrides.Mode = AntiAliasingMode::Off;
        else if (std::strcmp(env, "msaa") == 0)
            overrides.Mode = AntiAliasingMode::MSAA;
        else if (std::strcmp(env, "taa") == 0)
            overrides.Mode = AntiAliasingMode::TAA;
        else if (std::strcmp(env, "fxaa") == 0)
            overrides.Mode = AntiAliasingMode::FXAA;
        else if (std::strcmp(env, "smaa") == 0)
            overrides.Mode = AntiAliasingMode::SMAA;
        else if (std::strcmp(env, "temporalfxaa") == 0)
            overrides.Mode = AntiAliasingMode::TemporalFXAA;
        else
            Logger::Log::Warning(
                "GE_AA_MODE '{}' not recognized (off|msaa|taa|fxaa|smaa|temporalfxaa)", env);
    }

    if (const char* env = std::getenv("GE_TAA_SAMPLES"))
    {
        const int value = std::atoi(env);
        if (value == 8 || value == 16)
            overrides.TaaSequenceLength = static_cast<uint32>(value);
        else
            Logger::Log::Warning("GE_TAA_SAMPLES '{}' not recognized (8|16)", env);
    }

    return overrides;
}

} // namespace GameEngine::Engine::Renderer
