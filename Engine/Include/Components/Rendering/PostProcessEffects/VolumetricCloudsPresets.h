#pragma once

#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"

namespace GameEngine {
namespace Components {

// Named skies, each modeled on a real cloud type. Only the parameters a given
// sky actually characterizes are set; the rest keep the component's defaults,
// so a preset reads as "this is what makes it that sky".
//
// The shared vocabulary:
//   Altitude/Thickness — where the deck sits and how deep it is. Real decks run
//     from stratus at a few hundred metres to cirrus above 6 km.
//   CloudScale — form size. LOWER is bigger: 0.1 gives kilometre-wide towers,
//     0.6 gives the small tiled cells of a mackerel sky.
//   DensityOffset — coverage. The shape noise must clear -DensityOffset*0.1, so
//     less negative = more sky covered.
//   AbsorptionTowardSun + DarknessThreshold — how dark the undersides go. A
//     thunderhead is mostly this pair.
inline void ApplyVolumetricCloudsPreset(VolumetricClouds& c, VolumetricCloudsPreset preset)
{
    c.Preset = preset;
    switch (preset)
    {
    case VolumetricCloudsPreset::Custom:
        return;

    // Scattered flat-bottomed puffs on a summer afternoon: the default sky.
    case VolumetricCloudsPreset::FairWeatherCumulus:
        c.Altitude = 900.0f;
        c.Thickness = 1400.0f;
        c.CloudScale = 0.18f;
        c.DensityMultiplier = 1.0f;
        c.DensityOffset = -5.2f;
        c.DetailNoiseWeight = 0.1f;
        c.LightAbsorptionThroughCloud = 1.0f;
        c.LightAbsorptionTowardSun = 1.0f;
        c.DarknessThreshold = 0.2f;
        c.BaseBrightness = 0.8f;
        c.BaseSpeed = 0.25f;
        return;

    // Mackerel sky: a high sheet broken into many small cells, thin enough that
    // the sun still reads through it.
    case VolumetricCloudsPreset::Altocumulus:
        c.Altitude = 2200.0f;
        c.Thickness = 500.0f;
        c.CloudScale = 0.55f;
        c.DensityMultiplier = 0.8f;
        c.DensityOffset = -4.4f;
        c.DetailNoiseWeight = 0.2f;
        c.LightAbsorptionThroughCloud = 0.8f;
        c.LightAbsorptionTowardSun = 0.7f;
        c.DarknessThreshold = 0.35f;
        c.BaseBrightness = 0.9f;
        c.BaseSpeed = 0.35f;
        return;

    // A heavy overcast deck: broad, layered banks with thick and thin patches
    // rather than one flat ceiling. The density is deliberately LOW for its
    // coverage — a denser deck saturates every ray to the same value and
    // flattens into a grey slab with no structure left to see. The wide radius
    // pushes the disc rim out past where the deck reads as reaching the horizon.
    case VolumetricCloudsPreset::StratusOvercast:
        c.Radius = 26000.0f;
        c.Altitude = 800.0f;
        c.Thickness = 1100.0f;
        c.CloudScale = 0.075f;
        c.DensityMultiplier = 0.55f;
        c.DensityOffset = -3.4f;
        c.DetailNoiseWeight = 0.5f;
        c.DetailNoiseScale = 12.0f;
        c.LightAbsorptionThroughCloud = 0.8f;
        c.LightAbsorptionTowardSun = 1.2f;
        c.DarknessThreshold = 0.3f;
        c.BaseBrightness = 0.72f;
        c.PhaseFactor = 0.1f;
        c.StepSize = 14.0f;
        c.BaseSpeed = 0.15f;
        return;

    // Cumulonimbus: few, enormous, and deep enough that almost no sunlight
    // reaches the base — the dark underside is the whole read.
    case VolumetricCloudsPreset::Thunderhead:
        c.Altitude = 600.0f;
        c.Thickness = 2600.0f;
        c.CloudScale = 0.09f;
        c.DensityMultiplier = 1.5f;
        c.DensityOffset = -4.6f;
        c.DetailNoiseWeight = 0.15f;
        c.LightAbsorptionThroughCloud = 1.15f;
        c.LightAbsorptionTowardSun = 1.9f;
        // Deep enough to go dark, but a threshold of 0.16 keeps the bases grey
        // rather than crushing them to black.
        c.DarknessThreshold = 0.16f;
        c.ForwardScattering = 0.9f;
        c.BaseBrightness = 0.6f;
        c.StepSize = 18.0f; // the deck is deep; keep the march affordable
        c.BaseSpeed = 0.2f;
        return;

    // Ice-crystal wisps at altitude: barely there, streaked by erosion rather
    // than shaped by coverage.
    case VolumetricCloudsPreset::CirrusVeil:
        c.Altitude = 4200.0f;
        c.Thickness = 300.0f;
        c.CloudScale = 0.3f;
        c.DensityMultiplier = 0.5f;
        c.DensityOffset = -3.2f;
        // Shape barely holds together; the heavy fine-scale erosion is what
        // makes it read as high ice cloud rather than small cumulus.
        c.DetailNoiseWeight = 0.85f;
        c.DetailNoiseScale = 22.0f;
        c.LightAbsorptionThroughCloud = 0.45f;
        c.LightAbsorptionTowardSun = 0.35f;
        c.DarknessThreshold = 0.65f;
        c.BaseBrightness = 1.0f;
        c.StepSize = 14.0f;
        c.BaseSpeed = 0.5f;
        return;

    // A low broken ceiling with big gaps — the sky after weather passes, with
    // heavy shading under the remaining banks.
    case VolumetricCloudsPreset::BrokenCeiling:
        c.Altitude = 700.0f;
        c.Thickness = 1100.0f;
        c.CloudScale = 0.13f;
        c.DensityMultiplier = 1.3f;
        c.DensityOffset = -5.4f;
        c.DetailNoiseWeight = 0.25f;
        c.LightAbsorptionThroughCloud = 1.2f;
        c.LightAbsorptionTowardSun = 1.8f;
        c.DarknessThreshold = 0.14f;
        c.BaseBrightness = 0.7f;
        c.BaseSpeed = 0.3f;
        return;
    }
}

} // namespace Components
} // namespace GameEngine
