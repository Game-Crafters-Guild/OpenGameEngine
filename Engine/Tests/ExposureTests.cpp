#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace GameEngine;
using GameEngine::Components::ExposureMode;

// Fixed mode is the legacy linear multiplier; the default (1.0, no compensation) must resolve to
// EXACTLY 1.0 so the render graph's exposure==1.0 tonemap-skip fast path still fires.
TEST(Exposure, FixedModeIsLegacyLinearIdentity)
{
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 15.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 2.0f, 15.0f, 0.0f), 2.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 0.5f, 7.0f, 0.0f), 0.5f);
}

// Compensation is +/- stops in BOTH modes, and + brightens (the intuitive artist convention).
TEST(Exposure, CompensationStopsBrightenInBothModes)
{
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 0.0f, 1.0f), 2.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 0.0f, -1.0f), 0.5f);
    // Manual: +1 stop on top of the neutral EV also doubles.
    const float neutral = Rendering::kNeutralExposureEV;
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 1.0f), 2.0f, 1e-4f);
}

// Manual EV100 at the neutral anchor (log2(203)) is the identity multiplier — reference white
// (203 nits == scene-linear 1.0) maps to display white.
TEST(Exposure, ManualNeutralEvIsIdentity)
{
    const float neutral = Rendering::kNeutralExposureEV;
    EXPECT_NEAR(neutral, std::log2(Components::kReferenceWhiteNits), 1e-4f);
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 0.0f), 1.0f, 1e-5f);
    // Brighter scene -> higher EV -> smaller multiplier (photographic convention): +1 EV halves.
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral + 1.0f, 0.0f), 0.5f, 1e-5f);
}

// The realistic-mapping check: a physical noon sun (~100000 lux) drives a white Lambertian to
// ~157 scene-linear; exposing at the real-world sunny-16 EV100 of ~15 must bring it to ~display
// white. This is what makes the physical light units authorable with real EV numbers.
TEST(Exposure, SunnySceneAtEv15LandsNearDisplayWhite)
{
    const float sunUnitless = 100000.0f * Components::kUnitlessPerLux; // lux -> unitless (~492)
    const float whiteCardSceneLinear = sunUnitless * (1.0f / 3.14159265f); // Lambertian albedo 1, NdotL 1
    const float exposed = whiteCardSceneLinear * Rendering::EvToLinearExposure(15.0f);
    EXPECT_NEAR(exposed, 1.0f, 0.25f) << "sunny-16 EV100=15 should expose a white card near display white";
}

// Forward/inverse must round-trip so the inspector can display the EV of a resolved linear scale.
TEST(Exposure, EvLinearRoundTrip)
{
    for (float ev : {0.0f, 3.0f, 7.66f, 11.0f, 15.0f})
        EXPECT_NEAR(Rendering::LinearExposureToEv(Rendering::EvToLinearExposure(ev)), ev, 1e-3f);
}

// Physical camera: the sunny-16 rule (f/16, 1/100 s, ISO 100) is EV100 ~ 14.6, and each photographic
// control moves the EV by the expected number of stops.
TEST(Exposure, PhysicalCameraEv100MatchesSunny16AndStops)
{
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.01f, 100.0f), std::log2(25600.0f), 1e-4f); // ~14.64
    const float base = Rendering::PhysicalCameraEv100(16.0f, 0.01f, 100.0f);
    // Open the aperture one stop (area x2 => f /= sqrt(2)) -> 1 stop brighter scene-meter -> EV -1.
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f / std::sqrt(2.0f), 0.01f, 100.0f), base - 1.0f, 1e-4f);
    // Double the shutter time -> EV -1; double the ISO -> EV -1.
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.02f, 100.0f), base - 1.0f, 1e-4f);
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.01f, 200.0f), base - 1.0f, 1e-4f);
}

// Auto-exposure gate + EV-range -> linear clamp inversion. The resolve shader clamps the target
// scale to [minExposure, maxExposure]; minExposure comes from MaxEv (higher EV -> smaller scale).
TEST(Exposure, AutoExposureGateAndClampInversion)
{
    Engine::Renderer::PostProcessSettings s{};
    EXPECT_FALSE(s.IsAutoExposureActive()); // default: off
    s.AutoExposureActive = true;
    EXPECT_TRUE(s.IsAutoExposureActive());

    // The EV range maps to the linear clamp with min < max and both positive.
    const float minClamp = Rendering::EvToLinearExposure(s.AutoExposureMaxEv); // from Max EV
    const float maxClamp = Rendering::EvToLinearExposure(s.AutoExposureMinEv); // from Min EV
    EXPECT_GT(minClamp, 0.0f);
    EXPECT_LT(minClamp, maxClamp);
}

// Grey-card calibration anchor for Phase S (sky physical anchoring). Pins the exact scene-linear
// values the runtime grey-card scene is checked against at a NEUTRAL exposure (Manual EV=log2(203),
// scale 1.0): a 203-nit emitter reads display-white 1.0, and an 18% grey Lambertian facing a 203-lux
// sun reads 0.18/pi. The emissive/grey RATIO (pi/0.18 ~ 17.45) is unit-free, so the runtime test can
// compare it even when absolute HDR capture is unavailable. A sky lit grey card must land a plausible
// sub-sun level against THIS sun-grey to be coherent on the 203 scale.
TEST(Exposure, GreyCardCalibrationAnchorAtNeutralExposure)
{
    const float neutral = Rendering::kNeutralExposureEV;
    const float scale = Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 0.0f);
    EXPECT_NEAR(scale, 1.0f, 1e-5f);

    // A 203-nit emitter is scene-linear 1.0 by the emission anchor (surface_io.glsl: nits / 203);
    // exposed at the neutral scale it stays display white. (Folding the runtime scale through keeps
    // these as real value checks, not compile-time-constant comparisons.)
    const float emissiveExposed = scale;
    EXPECT_NEAR(emissiveExposed, 1.0f, 1e-5f);

    // 18% grey Lambertian, normal-facing a 203-lux sun (NdotL 1): albedo/pi * unitless(203 lx == 1.0).
    const float sunUnitless = Components::LightIntensityToUnitless(203.0f, Components::LightUnit::Lux);
    const float greyCardExposed = 0.18f * (1.0f / 3.14159265f) * sunUnitless * scale;
    EXPECT_NEAR(greyCardExposed, 0.0572958f, 1e-4f);

    // The emissive/grey ratio the runtime calibration compares: a sky-lit grey must land a plausible
    // sub-sun fraction of this sun-lit grey to be coherent on the 203 scale.
    EXPECT_NEAR(emissiveExposed / greyCardExposed, 3.14159265f / 0.18f, 1e-2f);
}

// The default auto-metering bias ships 0: reference-photographic metering through the untouched
// Hill fit (AE-ACES content renders ~0.85 stops darker than the pre-flip 1.8-pre-scale look; the
// shipped value is a pending pick, compared live via Exposure Compensation +0.85 — see
// kDefaultAutoExposureBiasEv). Reference relation kept as documentation: a bias of exactly
// log2(1.8) ~ +0.85 commutes with the removed ACES input pre-scale, so THAT bias reproduces the
// established pre-flip brightness. Guard the struct default so any Auto path that never runs the
// extraction stamp still lands on the shipped anchor.
TEST(Exposure, DefaultAutoExposureBiasIsReferencePhotographic)
{
    EXPECT_FLOAT_EQ(Engine::Renderer::kDefaultAutoExposureBiasEv, 0.0f);
    // Provenance of the +0.85 comparison lever: it is the removed 1.8 pre-scale in stops.
    EXPECT_NEAR(0.85f, std::log2(1.8f), 0.01f);
    Engine::Renderer::PostProcessSettings s{};
    EXPECT_FLOAT_EQ(s.AutoExposureBiasEv, Engine::Renderer::kDefaultAutoExposureBiasEv);
}

// TryWriteField folds the bias into the metering key AND both envelope clamps by the same factor.
// Scaling all three is exactly a post-clamp multiply of the resolved exposure — the UE semantics:
// compensation stays effective even when adaptation is pinned at the MinEv/MaxEv envelope edge
// (the old key-only fold went dead there).
TEST(Exposure, AutoExposureBiasFoldsIntoKeyAndBothClamps)
{
    using GameEngine::Rendering::ShaderMeta;
    using GameEngine::Rendering::NamedPushConstantWriter;

    ShaderMeta meta{};
    GameEngine::Rendering::PushConstantRangeMeta pc{};
    pc.Name = "PC";
    pc.Size = 12;
    pc.Block.Size = 12;
    const char* memberNames[3] = {"exposureKey", "minExposure", "maxExposure"};
    for (uint32_t i = 0; i < 3; ++i)
    {
        GameEngine::Rendering::Member m{};
        m.Name = memberNames[i];
        m.Offset = i * 4;
        m.Size = 4;
        pc.Block.Members.push_back(m);
    }
    meta.PushConstants.push_back(pc);

    Engine::Renderer::PostProcessSettings s{};
    s.AutoExposureActive = true;
    s.AutoExposureBiasEv = 0.85f;

    NamedPushConstantWriter pcw(meta, "PC");
    ASSERT_TRUE(pcw.IsValid());
    for (const char* name : memberNames)
        ASSERT_TRUE(s.TryWriteField(name, pcw)) << name;

    float key = 0.0f, minExp = 0.0f, maxExp = 0.0f;
    std::memcpy(&key, pcw.GetBuffer().data() + 0, 4);
    std::memcpy(&minExp, pcw.GetBuffer().data() + 4, 4);
    std::memcpy(&maxExp, pcw.GetBuffer().data() + 8, 4);

    const float bias = std::exp2(s.AutoExposureBiasEv);
    EXPECT_NEAR(key, Rendering::kAutoExposureMiddleGrey * bias, 1e-6f);
    EXPECT_NEAR(minExp, Rendering::EvToLinearExposure(s.AutoExposureMaxEv) * bias, 1e-8f);
    EXPECT_NEAR(maxExp, Rendering::EvToLinearExposure(s.AutoExposureMinEv) * bias, 1e-4f);

    // The post-clamp identity the fold relies on: with the resolve shader's
    // clamp(key/avg, lo, hi) evaluated on the folded values, a target pinned at either
    // envelope edge still carries the full bias.
    const float lo = Rendering::EvToLinearExposure(s.AutoExposureMaxEv);
    const float hi = Rendering::EvToLinearExposure(s.AutoExposureMinEv);
    const float brightAvg = 1000.0f; // pins at lo
    const float darkAvg = 1e-5f;     // pins at hi
    EXPECT_NEAR(std::clamp(key / brightAvg, minExp, maxExp), lo * bias, lo * bias * 1e-4f);
    EXPECT_NEAR(std::clamp(key / darkAvg, minExp, maxExp), hi * bias, hi * bias * 1e-4f);
}

// The bias participates in the volume blend like the other EV-space exposure fields.
TEST(Exposure, AutoExposureBiasBlendsAcrossVolumes)
{
    Engine::Renderer::PostProcessSettings a{}, b{};
    a.AutoExposureBiasEv = 0.85f;
    b.AutoExposureBiasEv = 2.0f;
    const auto out = Engine::Renderer::BlendPostProcessSettings(
        a, b, 1.0f, Engine::Renderer::SettingsBlendGroupMask::All());
    EXPECT_FLOAT_EQ(out.AutoExposureBiasEv, 2.0f);
    const auto mid = Engine::Renderer::BlendPostProcessSettings(
        a, b, 0.5f, Engine::Renderer::SettingsBlendGroupMask::All());
    EXPECT_NEAR(mid.AutoExposureBiasEv, 0.5f * (0.85f + 2.0f), 1e-5f);
}

// Physical mode routes the camera EV through the same EV-based path as Manual, so a sunny-16 camera
// exposes a physical noon sun to a bright (near display white) value rather than blowing out.
TEST(Exposure, PhysicalModeExposesSunnySceneSanely)
{
    const float ev = Rendering::ExposureEv100ForMode(ExposureMode::Physical, 0.0f, 16.0f, 0.01f, 100.0f);
    const float scale = Rendering::ResolveExposureScale(ExposureMode::Physical, 1.0f, ev, 0.0f);
    const float sunUnitless = 100000.0f * Components::kUnitlessPerLux;
    const float whiteCard = sunUnitless * (1.0f / 3.14159265f) * scale;
    EXPECT_GT(whiteCard, 0.4f);  // not crushed to black
    EXPECT_LT(whiteCard, 4.0f);  // not blown out by orders of magnitude (bright, near/above white)
    // Fixed mode ignores the camera EV entirely (back-compat).
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, ev, 0.0f), 1.0f);
}

// The camera component and the render-side camera exposure must start from the one shared
// default, so a camera switched to Manual exposes exactly like the Scene View it was framed in.
// The editor's Scene View pin reads the same constant but is not observable from an engine test:
// it is a private field on a prefs-backed singleton with no reset API, so a test would read
// whatever the user's preferences hold rather than the default. Nothing here sees that half.
TEST(Exposure, ManualEvDefaultIsSharedAcrossCameraAndViewRegistry)
{
    EXPECT_FLOAT_EQ(Components::Camera{}.ManualExposureEV, Components::kDefaultManualExposureEv);
    EXPECT_FLOAT_EQ(Engine::Renderer::ViewRegistry::CameraExposure{}.ManualExposureEV,
                    Components::kDefaultManualExposureEv);
    // Auto stays the camera's default mode: the shared EV is the fallback a manual camera lands on,
    // not a switch to manual exposure.
    EXPECT_EQ(Components::Camera{}.ExposureControl, ExposureMode::Auto);
}
