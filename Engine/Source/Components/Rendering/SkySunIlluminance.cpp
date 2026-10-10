#include "Components/Rendering/SkySunIlluminance.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Types/ColorUtils.h"

namespace GameEngine::Components::SkySunIlluminance
{
namespace
{
const Light* DirectionalLight(const ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.IsValid(entity))
        return nullptr;
    const Light* light = world.GetComponent<Light>(entity);
    return light && light->Type == LightType::Directional ? light : nullptr;
}
} // namespace

bool IsDrivableSunLight(const ECS::World& world, ECS::EntityHandle light)
{
    if (!DirectionalLight(world, light) || !world.GetComponent<Transform>(light))
        return false;
    const Parent* parent = world.GetComponent<Parent>(light);
    return !(parent && world.IsValid(parent->parent));
}

ECS::EntityHandle LightSkyWouldDrive(const ECS::World& world, const SkyEnvironment& sky)
{
    if (!sky.TimeOfDayDrivesSunLight || !IsDrivableSunLight(world, sky.SunLight))
        return {};
    return sky.SunLight;
}

ECS::EntityHandle RenderedSky(ECS::World& world)
{
    // The same two scans, in the same order, as SkyEnvironmentSystem::Update: an HDRI skybox wins
    // outright, else the first enabled Sky Environment the query visits.
    bool hdriSkybox = false;
    world.Query<ECS::Read<Skybox>>().Each([&](const Skybox& box) {
        hdriSkybox = hdriSkybox || box.HDRIAssetGuid[0] != '\0';
    });
    if (hdriSkybox)
        return {};

    ECS::EntityHandle rendered{};
    world.Query<ECS::Read<SkyEnvironment>>().Each([&](ECS::EntityHandle entity, const SkyEnvironment&) {
        if (!rendered.IsValid())
            rendered = entity;
    });
    return rendered;
}

ECS::EntityHandle DrivenSunLight(ECS::World& world)
{
    const ECS::EntityHandle sky = RenderedSky(world);
    const SkyEnvironment* comp = sky.IsValid() ? world.GetComponent<SkyEnvironment>(sky) : nullptr;
    return comp ? LightSkyWouldDrive(world, *comp) : ECS::EntityHandle{};
}

ECS::EntityHandle SkyDrivingLight(ECS::World& world, ECS::EntityHandle light)
{
    if (!light.IsValid() || DrivenSunLight(world) != light)
        return {};
    return RenderedSky(world);
}

uint8_t FieldsSkyDrives(const SkyEnvironment& sky)
{
    const bool curve = sky.SunIlluminanceSource == SkySunIlluminanceSource::Curve;
    uint8_t fields = kDrivesDirection;
    if (!curve || sky.DriveSunColor)
        fields |= kDrivesColor;
    if (curve)
        fields |= kDrivesIntensity;
    return fields;
}

uint8_t DrivenLightFields(ECS::World& world, ECS::EntityHandle light)
{
    const ECS::EntityHandle sky = SkyDrivingLight(world, light);
    const SkyEnvironment* comp = sky.IsValid() ? world.GetComponent<SkyEnvironment>(sky) : nullptr;
    return comp ? FieldsSkyDrives(*comp) : uint8_t{0};
}

ECS::EntityHandle EditableSunLight(const ECS::World& world, const SkyEnvironment& sky)
{
    return DirectionalLight(world, sky.SunLight) ? sky.SunLight : ECS::EntityHandle{};
}

ECS::EntityHandle ResolvedSunLight(ECS::World& world, const SkyEnvironment& sky)
{
    if (DirectionalLight(world, sky.SunLight))
        return sky.SunLight;

    // Scanned from the components, not from RenderServices::GetWorldLights, because the sky reads
    // its sun near the front of the frame, when that list is EMPTY: RenderingLoop::Update clears every
    // world's lights before the first ECS wave and RenderExtractionSystem, its only submitter, runs
    // many waves later. The ocean's sun pick reads ECS order for the same reason (RenderServices.h,
    // above SelectPrimaryDirectional).
    //
    // Brightest by resolved ILLUMINANCE: the finalized list's contribution order weights by range
    // squared, which a directional light does not have (FinalizeWorldLights documents the residual),
    // and the question here is which light is the sun. The first visited wins a tie.
    ECS::EntityHandle brightest{};
    float brightestUnitless = 0.0f;
    world.Query<ECS::Read<Light>>().Each(
        [&](ECS::EntityHandle entity, const Light& light) {
            if (!light.CastsLight || light.Type != LightType::Directional)
                return;
            const float unitless = LightIntensityToUnitless(light.Intensity, light.IntensityUnit);
            if (unitless > brightestUnitless)
            {
                brightestUnitless = unitless;
                brightest = entity;
            }
        });
    return brightest;
}

float ClearSunLux(ECS::World& world, const SkyEnvironment& sky)
{
    if (sky.SunIlluminanceSource == SkySunIlluminanceSource::Curve)
        return SkySunDrive::NoonReferenceLux(sky);
    const ECS::EntityHandle sun = ResolvedSunLight(world, sky);
    const Light* light = sun.IsValid() ? world.GetComponent<Light>(sun) : nullptr;
    return light ? LuxFromLightIntensity(light->Intensity, light->IntensityUnit) : 0.0f;
}

float LuxFromLightIntensity(float intensity, LightUnit unit)
{
    // Through the unitless scale and back is not exact for about one value in eight, so a Lux light
    // skips it: its value is already lux.
    if (unit == LightUnit::Lux)
        return intensity;
    return UnitlessToLightIntensity(LightIntensityToUnitless(intensity, unit), LightUnit::Lux);
}

float LightIntensityFromLux(float lux, LightUnit unit)
{
    if (unit == LightUnit::Lux)
        return lux;
    return UnitlessToLightIntensity(LightIntensityToUnitless(lux, LightUnit::Lux), unit);
}

float DeliveredLux(const Light& light)
{
    float colour[3] = {1.0f, 1.0f, 1.0f};
    float unitless = 0.0f;
    ResolveLightColorIntensity(light, colour, unitless);
    const float luminance = ColorUtils::LinearRec709Luminance(colour);
    return UnitlessToLightIntensity(unitless, LightUnit::Lux) * luminance;
}

void ReleaseSunLightIntensity(ECS::World& world, ECS::EntityHandle light, float lux)
{
    const Light* current = DirectionalLight(world, light);
    if (!current)
        return;
    const float intensity = LightIntensityFromLux(lux, current->IntensityUnit);
    if (current->Intensity == intensity)
        return;
    if (Light* writable = world.GetComponentForWrite<Light>(light))
        writable->Intensity = intensity;
}

void ReleaseSunLightColor(ECS::World& world, ECS::EntityHandle light)
{
    const Light* current = DirectionalLight(world, light);
    if (!current || (current->Color[0] == 1.0f && current->Color[1] == 1.0f && current->Color[2] == 1.0f))
        return;
    if (Light* writable = world.GetComponentForWrite<Light>(light))
    {
        writable->Color[0] = 1.0f;
        writable->Color[1] = 1.0f;
        writable->Color[2] = 1.0f;
    }
}

} // namespace GameEngine::Components::SkySunIlluminance
