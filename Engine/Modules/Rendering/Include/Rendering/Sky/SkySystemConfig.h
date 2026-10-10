#pragma once

namespace GameEngine {
namespace Rendering {

// The constants of the simple sun and moon model the sky system runs (SkySystem.h), on its own so a
// component that derives a default from them does not take in the model's functions.
struct SkySystemConfig
{
    // If true, when the sun is sufficiently below the horizon we switch the primary light to the moon.
    bool useMoonWhenSunBelowHorizon = true;

    // Legacy midpoint for sun/moon handoff; the actual lighting cross-fade spans a band
    // around this value so primary color does not pop at sunset.
    float sunBelowHorizonDotThreshold = -0.05f;

    // Simple parameters for moon lighting when it becomes the primary light.
    float moonIntensityScale = 0.05f;
    // The default moonlight as a fraction of a clear sun's illuminance: with moonColor below it sets
    // SkyEnvironment's default Moonlight (kDefaultMoonlightIlluminanceLux), and the linked light's
    // full night at that default and a clear sun. Real moonlight is ~1/400000 of the sun, which meters
    // to black, so the light runs a stylized ~1/100 instead — against moonColor below, whose own
    // ~1/200 magnitude lands the pair at a few lux: a bright cinematic moonlit night that still sits
    // under the auto-exposure floor. Deliberately smaller than moonIntensityScale: the visible disc is
    // an emitter, the light is its illuminance.
    float moonLightIlluminanceScale = 0.01f;
    // Colour of the primary source at full night, on the same scene-linear scale as the daytime
    // sun's white. Its MAGNITUDE is the stylized moon-to-sun ratio, not a normalized tint: real
    // moonlight is ~1/400000 of sunlight and meters to black, so the sky runs ~1/200 instead — a
    // moonlit night that still sits below the auto-exposure floor and reads as night. The
    // chromaticity is the cool grey of sRGB #0F1012.
    float moonColor[3] = { 0.004776953f, 0.005181517f, 0.006048833f };
};

} // namespace Rendering
} // namespace GameEngine
