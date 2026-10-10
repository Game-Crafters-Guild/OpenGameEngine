#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"

#include <cmath>

using namespace GameEngine::Components;

namespace
{
constexpr float kPaperWhiteNits = 203.0f; // GE_EMISSION_PAPERWHITE_NITS (surface_io.glsl)
constexpr float kFourPi = 12.56637061f;
}

// The lux/candela anchors must equal 1/203 so a physical light reads coherently with a 203-nit
// emitter. These are the only two calibration constants; a drift here desyncs lights from emission.
TEST(LightPhotometry, AnchorsCoupleToPaperWhite)
{
    EXPECT_FLOAT_EQ(kUnitlessPerLux, 1.0f / kPaperWhiteNits);
    EXPECT_FLOAT_EQ(kUnitlessPerCandela, 1.0f / kPaperWhiteNits);
    EXPECT_FLOAT_EQ(kLightFourPi, kFourPi); // geometry, not a calibration knob

    // 203 lx (or 203 cd at the candela reference) lands at scene-linear ~1.0, where a 203-nit
    // emitter sits; 1 lx is exactly 1/203.
    EXPECT_NEAR(LightIntensityToUnitless(1.0f, LightUnit::Lux), 1.0f / kPaperWhiteNits, 1e-7f);
    EXPECT_NEAR(LightIntensityToUnitless(kPaperWhiteNits, LightUnit::Lux), 1.0f, 1e-5f);
    EXPECT_NEAR(LightIntensityToUnitless(kPaperWhiteNits, LightUnit::Candela), 1.0f, 1e-5f);
}

// Lumen (point/spot flux) resolves via flux -> candela (/4pi) -> /203, so a 4*pi*203 lm isotropic
// point lands at ~1.0 and matches the equivalent candela conversion.
TEST(LightPhotometry, LumenDerivesFromCandela)
{
    const float fluxForUnit = kFourPi * kPaperWhiteNits;
    EXPECT_NEAR(LightIntensityToUnitless(fluxForUnit, LightUnit::Lumen), 1.0f, 1e-4f);
    EXPECT_NEAR(LightIntensityToUnitless(kFourPi * 50.0f, LightUnit::Lumen),
                LightIntensityToUnitless(50.0f, LightUnit::Candela), 1e-6f);
}

// Unitless is anchor-invariant (pass-through) — this is why pre-physical-units scenes are
// unaffected by the anchor flip.
TEST(LightPhotometry, UnitlessIsPassThrough)
{
    EXPECT_FLOAT_EQ(LightIntensityToUnitless(0.0f, LightUnit::Unitless), 0.0f);
    EXPECT_FLOAT_EQ(LightIntensityToUnitless(7.5f, LightUnit::Unitless), 7.5f);
    EXPECT_FLOAT_EQ(LightIntensityToUnitless(2.0f, LightUnit::Unitless), 2.0f); // default directional
}

// Forward/inverse must be a clean matched pair for every unit, across the authoring range.
TEST(LightPhotometry, ForwardInverseRoundTrip)
{
    const LightUnit units[] = {LightUnit::Unitless, LightUnit::Lux, LightUnit::Lumen, LightUnit::Candela};
    const float values[] = {0.0f, 1.0f, 203.0f, 100000.0f};
    for (LightUnit u : units)
        for (float v : values)
        {
            const float back = LightIntensityToUnitless(UnitlessToLightIntensity(v, u), u);
            EXPECT_NEAR(back, v, std::max(1e-3f, v * 1e-5f)) << "unit=" << static_cast<int>(u) << " v=" << v;
        }
}

// Guards the anchor migration ratio so the HTML migration guide's numbers stay truthful: a stored
// Lux value renders 100000/203x brighter than under the old 1/100000 anchor (and candela 127/203x).
TEST(LightPhotometry, MigrationRatioMatchesGuide)
{
    constexpr float kOldPerLux = 1.0f / 100000.0f;
    constexpr float kOldPerCandela = 1.0f / 127.0f;
    EXPECT_NEAR(kUnitlessPerLux / kOldPerLux, 100000.0f / 203.0f, 1.0f);       // ~492.6x
    EXPECT_NEAR(kUnitlessPerCandela / kOldPerCandela, 127.0f / 203.0f, 1e-3f); // ~0.626x
}

// End-to-end resolve: a default light is Unitless intensity 1 -> 1; Kelvin tint is brightness-
// preserving (brightest channel normalized to 1).
TEST(LightPhotometry, ResolveColorIntensityEndToEnd)
{
    Light l{};
    float color[3] = {0, 0, 0};
    float intensity = 0.0f;
    ResolveLightColorIntensity(l, color, intensity);
    EXPECT_FLOAT_EQ(intensity, 1.0f);

    l.UseColorTemperature = true;
    l.ColorTemperature = 6500.0f;
    ResolveLightColorIntensity(l, color, intensity);
    EXPECT_NEAR(std::max(color[0], std::max(color[1], color[2])), 1.0f, 0.02f);
}
