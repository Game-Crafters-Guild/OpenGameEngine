#pragma once

// Developer environment overrides for the engine-wide anti-aliasing defaults.
// Parsing only: the values are reported as-requested, and the caller applies
// them through RenderServices' setters so the device clamps (an Apple GPU caps
// MSAA at 4x) stay in one place.

#include "Engine/Rendering/AntiAliasing.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

// GE_MSAA_SAMPLES  1|2|4|8      — a count > 1 also selects MSAA mode, so the
//                                 legacy variable keeps its meaning under the
//                                 mode enum (GE_AA_MODE still wins). It seeds
//                                 the startup values only: unlike GE_AA_MODE it
//                                 is NOT held back when a project or game.config
//                                 snapshot is applied over it.
// GE_AA_MODE       off|msaa|taa|fxaa|smaa|temporalfxaa (lowercase)
// GE_TAA_SAMPLES   8|16         — TAA Halton cycle length.
//
// An unset variable leaves its field empty; an unrecognized value warns and
// leaves it empty, so the caller keeps whatever default it already had.
struct AntiAliasingEnvOverrides
{
    std::optional<uint32> MsaaSamples;
    std::optional<AntiAliasingMode> Mode;
    std::optional<uint32> TaaSequenceLength;

    static AntiAliasingEnvOverrides Read();
};

} // namespace GameEngine::Engine::Renderer
