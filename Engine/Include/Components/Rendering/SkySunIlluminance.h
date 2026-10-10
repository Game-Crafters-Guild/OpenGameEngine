#pragma once

#include "Components/Rendering/Light.h" // LightUnit
#include "ECS/ECS.h"                    // EntityHandle

namespace GameEngine::ECS
{
class World;
}

// Which light a sky drives, and which of its fields. By default the sun's illuminance is one value,
// in lux, stored on the sky's linked directional light: the sky reads it at every hour and never
// writes it, drives the light's direction and colour, and carries the night's dimming in the colour.
// A sky whose illuminance comes from its own curve writes the light's Intensity as well.
//
// Free functions only: this directory is scanned for components, and every struct here would
// register as one.
namespace GameEngine::Components
{
struct Light;
struct SkyEnvironment;

namespace SkySunIlluminance
{

// A light the sky can drive: a directional light with a Transform and no parent, because the drive
// writes its local rotation.
bool IsDrivableSunLight(const ECS::World& world, ECS::EntityHandle light);

// The sky the sky system renders this frame: the first enabled Sky Environment on an active entity,
// in the system's own query order. Invalid when there is none, or when an enabled Skybox with an
// HDRI is present: the system then renders the skybox and no Sky Environment at all.
ECS::EntityHandle RenderedSky(ECS::World& world);

// The light the sky system drives this frame: the rendered sky's linked light while its Time of Day
// drives it and the light is drivable. Invalid otherwise. The system drives exactly this light, and
// every reader that asks "is this light driven" asks here.
ECS::EntityHandle DrivenSunLight(ECS::World& world);

// The sky that drives `light` this frame: RenderedSky when DrivenSunLight is `light`. Invalid when
// no sky drives it.
ECS::EntityHandle SkyDrivingLight(ECS::World& world, ECS::EntityHandle light);

// The light `sky` would drive if it were the rendered sky: its linked light while its Time of Day
// drives it and the light is drivable. Invalid otherwise.
ECS::EntityHandle LightSkyWouldDrive(const ECS::World& world, const SkyEnvironment& sky);

// The light the Sun illuminance row edits: the linked directional light, and only it, so the
// target cannot change under an edit. Invalid when nothing, or no directional light, is linked.
ECS::EntityHandle EditableSunLight(const ECS::World& world, const SkyEnvironment& sky);

// The light the sky takes the sun's brightness from: the linked directional light even when a
// brighter one exists, else the scene's brightest emitting directional light. Invalid when the scene
// has no sun.
ECS::EntityHandle ResolvedSunLight(ECS::World& world, const SkyEnvironment& sky);

// The fields of a light the sky drives, as bits of DrivenLightFields' result.
inline constexpr uint8_t kDrivesDirection = 1u << 0;
inline constexpr uint8_t kDrivesColor = 1u << 1;
inline constexpr uint8_t kDrivesIntensity = 1u << 2;

// The fields `sky` writes into the light it drives: the direction always; the colour while the
// illuminance comes from the light (it carries the sky's extinction and the night) or while
// DriveSunColor is on; the Intensity while the illuminance comes from the sky's curve.
uint8_t FieldsSkyDrives(const SkyEnvironment& sky);

// The fields of `light` the sky system drives this frame: FieldsSkyDrives of the sky that drives it
// (SkyDrivingLight), or 0 when no sky does. The light inspector disables exactly these, and the
// sky system writes and hands back exactly these.
uint8_t DrivenLightFields(ECS::World& world, ECS::EntityHandle light);

// The clear, overhead sun the sky's day is described with, in lux: the illuminance of the light the
// sky reads (ResolvedSunLight) while that light holds it, or the curve's noon reference
// (SkySunDrive::NoonReferenceLux) under the sky's curve, where the light holds what the sky wrote.
// 0 when the scene has no sun. The inspector's previews, its readout and the curve's seed read it.
float ClearSunLux(ECS::World& world, const SkyEnvironment& sky);

// A light's intensity in lux, and a lux value in a light's unit. Conversions go through the
// unitless 203-nit scale, so they are exact inverses up to float rounding for every unit, and a
// Lux light's value passes through unchanged, bit for bit.
float LuxFromLightIntensity(float intensity, LightUnit unit);
float LightIntensityFromLux(float lux, LightUnit unit);

// What a surface facing the light receives now, in lux: the intensity times the luminance of the
// colour the light is resolved to, which for a driven light carries the sky's extinction and the
// sun-to-moon handover.
float DeliveredLux(const Light& light);

// Hand a light's colour back to its author when the sky stops driving it: it returns to white. The
// colour of a driven light is the sky's derived value, not something the author set.
// SkyEnvironmentSystem calls this for every colour drive that ends, however it ends.
void ReleaseSunLightColor(ECS::World& world, ECS::EntityHandle light);

// Hand a light's Intensity back to its author when the sky stops writing it: `lux` in the light's
// current unit. SkyEnvironmentSystem calls this for every intensity drive that ends.
void ReleaseSunLightIntensity(ECS::World& world, ECS::EntityHandle light, float lux);

} // namespace SkySunIlluminance
} // namespace GameEngine::Components
