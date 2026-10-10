// The curve's physical reference line is redrawn only when what it is drawn from changes. Two inputs
// read from the same sky compare equal however their padding bytes happen to differ, so a refresh of an
// unchanged sky redraws nothing.

#include <gtest/gtest.h>

#include <cstring>
#include <new>

#include "Components/Rendering/SkyEnvironment.h"
#include "Inspectors/SkySunCurveReference.h"

using namespace GameEngine;

namespace
{
// `source`'s fields written one by one into an object built over `storage`, so the padding between them
// keeps whatever `storage` held.
SkySunCurveReferenceInputs* PlaceFields(unsigned char* storage, const SkySunCurveReferenceInputs& source)
{
    auto* placed = new (storage) SkySunCurveReferenceInputs;
    placed->Latitude = source.Latitude;
    placed->DayOfYear = source.DayOfYear;
    placed->SunPath = source.SunPath;
    placed->CustomAxisAltitude = source.CustomAxisAltitude;
    placed->CustomNoonHeight = source.CustomNoonHeight;
    placed->KeyTimes = source.KeyTimes;
    placed->SunTint = source.SunTint;
    return placed;
}
} // namespace

TEST(SkySunCurveReference, TheSameSkyReadsAsTheSameInputs)
{
    Components::SkyEnvironment sky{};
    sky.Latitude = 51.5f;
    sky.DayOfYear = 172;

    alignas(SkySunCurveReferenceInputs) unsigned char first[sizeof(SkySunCurveReferenceInputs)];
    alignas(SkySunCurveReferenceInputs) unsigned char second[sizeof(SkySunCurveReferenceInputs)];
    std::memset(first, 0x00, sizeof(first));
    std::memset(second, 0xA5, sizeof(second));
    const SkySunCurveReferenceInputs read = ReadSkySunCurveReferenceInputs(sky);
    const SkySunCurveReferenceInputs* a = PlaceFields(first, read);
    const SkySunCurveReferenceInputs* b = PlaceFields(second, read);
    EXPECT_TRUE(*a == *b) << "an unchanged sky would redraw its reference line at every refresh";

    sky.DayOfYear = 173;
    EXPECT_FALSE(*a == ReadSkySunCurveReferenceInputs(sky)) << "a new day must redraw it";
}
