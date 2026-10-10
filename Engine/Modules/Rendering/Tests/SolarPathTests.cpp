#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"
#include "Rendering/Sky/SolarPath.h"

using namespace GameEngine::Rendering;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesPerRadian = 180.0 / kPi;

// The Spencer (1971) Fourier series for the declination, in radians: the reference the model is held
// to. Good to about 0.04 degrees against the ephemeris.
double SpencerDeclinationRadians(int dayOfYear)
{
    const double g = 2.0 * kPi * (dayOfYear - 1) / 365.0;
    return 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2.0 * g) +
           0.000907 * std::sin(2.0 * g) - 0.002697 * std::cos(3.0 * g) + 0.00148 * std::sin(3.0 * g);
}

// Textbook geometric noon elevation and day length (disc centre, no refraction), in double.
double ReferenceNoonElevationDegrees(double latitudeDegrees, double declinationRadians)
{
    return 90.0 - std::abs(latitudeDegrees - declinationRadians * kDegreesPerRadian);
}

double ReferenceDayLengthHours(double latitudeDegrees, double declinationRadians)
{
    const double latitude = latitudeDegrees / kDegreesPerRadian;
    const double cosHalfDay = std::clamp(-std::tan(latitude) * std::tan(declinationRadians), -1.0, 1.0);
    return 24.0 / kPi * std::acos(cosHalfDay);
}

float DegreesFromUp(const float direction[3])
{
    return static_cast<float>(std::asin(std::clamp(direction[1], -1.0f, 1.0f)) * kDegreesPerRadian);
}

void SunAtHour(const SolarFrame& frame, float hours, float out[3])
{
    SkySettings settings{};
    settings.timeOfDayHours = hours;
    SkySystemState state{};
    ComputeSimpleSunMoon(settings, SkySystemConfig{}, frame, state);
    std::memcpy(out, state.sunDirWS, sizeof(state.sunDirWS));
}

uint32_t Bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// The sun and moon placement the sky used before it had a latitude or a date: one great circle
// through the zenith, noon straight up, east = south x up with south = -Z. It is the golden for the
// default site, so it is kept here verbatim now that production no longer has it.
struct GreatCircle
{
    float Sun[3];
    float Moon[3];
};

GreatCircle PreviousGreatCircle(float timeOfDayHours, float moonArcPosition)
{
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    const float noon[3] = {0.0f, 1.0f, 0.0f};
    const float south[3] = {0.0f, 0.0f, -1.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float east[3] = {south[1] * up[2] - south[2] * up[1], south[2] * up[0] - south[0] * up[2],
                           south[0] * up[1] - south[1] * up[0]};

    float t = timeOfDayHours / 24.0f;
    t = t - std::floor(t);
    const float theta = (t - 0.25f) * kTwoPi;
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    float cycle = moonArcPosition;
    cycle = cycle - std::floor(cycle);
    const float moonTheta = theta + cycle * kTwoPi;
    const float mc = std::cos(moonTheta);
    const float ms = std::sin(moonTheta);

    GreatCircle result{};
    for (int i = 0; i < 3; ++i)
    {
        result.Sun[i] = c * east[i] + s * noon[i];
        result.Moon[i] = mc * east[i] + ms * noon[i];
    }
    return result;
}

// The previous follow-mode inverse at the default site: the direction's angle on that great circle.
float PreviousHourFromDirection(const float direction[3])
{
    constexpr float kTwoPi = 2.0f * 3.14159265f;
    const float east[3] = {1.0f, -0.0f, 0.0f};
    const float noon[3] = {0.0f, 1.0f, 0.0f};
    const float c = direction[0] * east[0] + direction[1] * east[1] + direction[2] * east[2];
    const float s = direction[0] * noon[0] + direction[1] * noon[1] + direction[2] * noon[2];
    float t = std::atan2(s, c) / kTwoPi + 0.25f;
    t = t - std::floor(t);
    return t * 24.0f;
}

// Bitwise equal, except that a -0 Z in the previous vector may be +0 now: the new frame's last
// add is of an exact +0, which turns every signed zero positive. That happens only while both the
// cosine and the sine of the angle are negative, 18:00 to 24:00 (both ends included) for the sun.
::testing::AssertionResult SameDirection(const float previous[3], const float current[3])
{
    for (int i = 0; i < 3; ++i)
    {
        if (Bits(previous[i]) == Bits(current[i]))
            continue;
        const bool zeroSignOnZ = i == 2 && previous[i] == 0.0f && current[i] == 0.0f &&
                                 std::signbit(previous[i]) && !std::signbit(current[i]);
        if (!zeroSignOnZ)
            return ::testing::AssertionFailure()
                   << "component " << i << ": previous " << previous[i] << " (0x" << std::hex << Bits(previous[i])
                   << "), current " << current[i] << " (0x" << Bits(current[i]) << ")";
    }
    return ::testing::AssertionSuccess();
}

} // namespace

// The model is a circular orbit plus the equation of centre, measured from the March equinox. It is
// held to Spencer within 0.1 degrees every day, and must be exactly 0 on the equinox day so the
// default sky is today's to the bit. Dropping the equation of centre fails near day 270 (1.58
// degrees); a perihelion constant far from 4 January, or a sign slip, fails too.
TEST(SolarPath, DeclinationTracksTheSpencerSeries)
{
    constexpr double kToleranceDegrees = 0.1;
    for (int day = 1; day <= kDaysInCalendarYear; ++day)
    {
        const double model = SolarDeclinationRadians(day);
        const double reference = SpencerDeclinationRadians(day);
        EXPECT_NEAR(model * kDegreesPerRadian, reference * kDegreesPerRadian, kToleranceDegrees) << "day " << day;
    }

    const float equinox = SolarDeclinationRadians(kMarchEquinoxDay);
    EXPECT_EQ(equinox, 0.0f);
    EXPECT_FALSE(std::signbit(equinox));
}

// London (51.5 N) and Cape Town (33.9 S) at the June and December solstices, days 172 and 355.
//
// The references are geometric (the centre of the disc, no refraction), from the Spencer
// declination and the textbook sunrise equation, computed here. Published calculators are not used
// as references because they add refraction and the disc radius: NOAA's sunrise table for 51.5 N,
// 0 E, 2026 (gml.noaa.gov/grad/solcalc/table.php, checked by hand on 2026-09-25) gives 04:43 to
// 21:21 BST on 21 June (16.63 h) and 08:03 to 15:53 on 21 December (7.83 h). The same sunrise
// equation with the sun's centre at -0.833 degrees reproduces those to within a minute (16.64 h,
// 7.83 h); geometrically the days are 16.41 h and 7.60 h, which is what this path models.
//
// The tolerances cover the model's declination against Spencer's on these days (at most 0.014
// degrees, which moves the noon elevation by as much and the day length by 0.004 h). They catch a
// swapped hemisphere, a latitude with the wrong sign and an hour angle off by 180 degrees, each of
// which moves these values by degrees or hours. The elevation is also measured on the direction
// the sky renders, at noon and at the computed sunrise.
TEST(SolarPath, NoonElevationAndDayLengthAtTwoLatitudesAndTwoDates)
{
    constexpr double kElevationToleranceDegrees = 0.02;
    constexpr double kDayLengthToleranceHours = 0.01;
    // At noon the sun crosses the meridian; float pi puts theta a few 1e-8 rad off it.
    constexpr float kAzimuthToleranceDegrees = 1e-3f;
    // The rendered sun at the path's own sunrise hour sits on the horizon; float rounding of the hour
    // and the direction leaves a few thousandths of a degree.
    constexpr double kSunriseElevationToleranceDegrees = 0.01;

    struct Case
    {
        float Latitude;
        int32_t Day;
    };
    for (const Case place : {Case{51.5f, 172}, Case{51.5f, 355}, Case{-33.9f, 172}, Case{-33.9f, 355}})
    {
        const double declination = SpencerDeclinationRadians(place.Day);
        const double noonReference = ReferenceNoonElevationDegrees(place.Latitude, declination);
        const double dayLengthReference = ReferenceDayLengthHours(place.Latitude, declination);

        EXPECT_NEAR(SolarNoonElevationDegrees(EarthPathAngles({place.Latitude, place.Day, 0.0f})), noonReference, kElevationToleranceDegrees)
            << place.Latitude << " day " << place.Day;
        EXPECT_NEAR(SolarDayLengthHours(EarthPathAngles({place.Latitude, place.Day, 0.0f})), dayLengthReference, kDayLengthToleranceHours)
            << place.Latitude << " day " << place.Day;

        const SolarFrame frame = MakeSolarFrame(EarthPathAngles({place.Latitude, place.Day, 0.0f}));
        float noon[3];
        SunAtHour(frame, 12.0f, noon);
        EXPECT_NEAR(DegreesFromUp(noon), noonReference, kElevationToleranceDegrees)
            << place.Latitude << " day " << place.Day << ": the rendered noon sun";
        // The noon sun leans toward the equator: south (-Z) in the north, north (+Z) in the south,
        // unless it is past the zenith (never, at these two places).
        if (place.Latitude > 0.0f)
            EXPECT_LT(noon[2], 0.0f);
        else
            EXPECT_GT(noon[2], 0.0f);

        // The same noon as an observer reads it: the elevation, and due south (180) in the north
        // or due north (0) in the south.
        const SolarPosition noonPosition = SolarPositionAtHour(frame, 12.0f);
        EXPECT_NEAR(noonPosition.ElevationDegrees, noonReference, kElevationToleranceDegrees);
        const float bearingFromEquator = place.Latitude > 0.0f
                                             ? std::abs(noonPosition.AzimuthDegrees - 180.0f)
                                             : std::min(noonPosition.AzimuthDegrees, 360.0f - noonPosition.AzimuthDegrees);
        EXPECT_LT(bearingFromEquator, kAzimuthToleranceDegrees) << place.Latitude << " day " << place.Day;

        float sunrise[3];
        SunAtHour(frame, 12.0f - 0.5f * SolarDayLengthHours(EarthPathAngles({place.Latitude, place.Day, 0.0f})), sunrise);
        EXPECT_NEAR(DegreesFromUp(sunrise), 0.0, kSunriseElevationToleranceDegrees)
            << place.Latitude << " day " << place.Day << ": the rendered sun at the path's sunrise";
        EXPECT_GT(sunrise[0], 0.0f) << "the sun rises in the east (+X at north 0)";
    }
}

// The guard for the slice: at latitude 0, day 80 and north 0 the sky's sun and moon are the
// previous great circle's, bit for bit, at every minute of the day, and the follow mode reads the
// same hour back. A reordered float expression, a renormalised vector or a moon moved off the sun's
// circle all fail here. 0.96618974 is the moon arc ddgi_sponza.scene stores.
TEST(SolarPath, TheDefaultIsTodaysGreatCircle)
{
    const SolarFrame frame = MakeSolarFrame(EarthPathAngles({}));
    constexpr int kMinutesPerDay = 24 * 60;
    int zeroSignFlips = 0;
    int daylightMinutes = 0;

    for (const float moonArc : {0.5f, 0.96618974f})
    {
        for (int minute = 0; minute < kMinutesPerDay; ++minute)
        {
            const float hours = static_cast<float>(minute) / 60.0f;
            SkySettings settings{};
            settings.timeOfDayHours = hours;
            settings.moonArcPosition = moonArc;
            SkySystemState state{};
            ComputeSimpleSunMoon(settings, SkySystemConfig{}, frame, state);
            const GreatCircle previous = PreviousGreatCircle(hours, moonArc);

            EXPECT_TRUE(SameDirection(previous.Sun, state.sunDirWS)) << "sun at minute " << minute;
            EXPECT_TRUE(SameDirection(previous.Moon, state.moonDirWS))
                << "moon at minute " << minute << ", arc " << moonArc;
            if (Bits(previous.Sun[2]) != Bits(state.sunDirWS[2]))
            {
                ++zeroSignFlips;
                // 18:00 and 00:00 (= 24:00) themselves are in the window: float pi is slightly over
                // pi, so sin at 18:00 and cos at 00:00 come out as tiny negatives, not zero.
                EXPECT_TRUE(hours >= 18.0f || hours == 0.0f)
                    << "the only allowed difference falls from 18:00 to 24:00, not at " << hours;
            }
            if (state.primaryMoonBlend == 0.0f)
                ++daylightMinutes;

            EXPECT_EQ(Bits(SolarHourFromDirection(frame, state.sunDirWS)), Bits(PreviousHourFromDirection(previous.Sun)))
                << "follow-mode hour at minute " << minute;
        }
    }

    EXPECT_GT(zeroSignFlips, 0) << "the -0 case this test allows for was never exercised";
    // Day is w == 0: 06:04:35 to 17:55:25 at the default site, so 06:05 to 17:55 inclusive.
    EXPECT_EQ(daylightMinutes, 2 * (17 * 60 + 55 - (6 * 60 + 5) + 1));
}

// Inside the polar circles the sunrise equation leaves [-1, 1]; the day length must clamp to 24 h
// (polar day) or 0 h (polar night), and the directions must stay finite, at the poles too.
TEST(SolarPath, PolarDayAndPolarNightStayFinite)
{
    EXPECT_FLOAT_EQ(SolarDayLengthHours(EarthPathAngles({70.0f, 172, 0.0f})), 24.0f);
    EXPECT_FLOAT_EQ(SolarDayLengthHours(EarthPathAngles({70.0f, 355, 0.0f})), 0.0f);
    EXPECT_LT(SolarNoonElevationDegrees(EarthPathAngles({70.0f, 355, 0.0f})), 0.0f) << "polar night: the noon sun stays down";

    for (const float latitude : {70.0f, 90.0f, -90.0f})
    {
        for (const int32_t day : {int32_t{80}, int32_t{172}, int32_t{355}})
        {
            const float length = SolarDayLengthHours(EarthPathAngles({latitude, day, 0.0f}));
            EXPECT_TRUE(std::isfinite(length));
            EXPECT_GE(length, 0.0f);
            EXPECT_LE(length, 24.0f);
            EXPECT_TRUE(std::isfinite(SolarNoonElevationDegrees(EarthPathAngles({latitude, day, 0.0f}))));

            const SolarFrame frame = MakeSolarFrame(EarthPathAngles({latitude, day, 0.0f}));
            for (float hours = 0.0f; hours < 24.0f; hours += 0.5f)
            {
                float sun[3];
                SunAtHour(frame, hours, sun);
                const float length2 = sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2];
                EXPECT_TRUE(std::isfinite(length2)) << latitude << " day " << day << " hour " << hours;
                EXPECT_NEAR(length2, 1.0f, 1e-5f) << latitude << " day " << day << " hour " << hours;
            }
        }
    }

    // Out-of-range and non-finite input is sanitised, never propagated.
    const SolarFrame nonFinite = MakeSolarFrame(EarthPathAngles({std::nanf(""), 400, std::numeric_limits<float>::infinity()}));
    float sun[3];
    SunAtHour(nonFinite, 9.0f, sun);
    EXPECT_TRUE(std::isfinite(sun[0]) && std::isfinite(sun[1]) && std::isfinite(sun[2]));
}

// The follow mode reads the hour back from a light's direction. It must be the inverse of the
// forward path at a real latitude, date and north, not only on the previous equatorial circle.
TEST(SolarPath, TheHourRoundTripsThroughTheDirection)
{
    const SolarFrame frame = MakeSolarFrame(EarthPathAngles({51.5f, 172, 37.0f}));
    constexpr float kToleranceHours = 1e-3f;
    for (float hours = 0.25f; hours < 24.0f; hours += 0.5f)
    {
        float sun[3];
        SunAtHour(frame, hours, sun);
        const float recovered = SolarHourFromDirection(frame, sun);
        EXPECT_NEAR(recovered, hours, kToleranceHours) << "hour " << hours;
    }
}

// North turns the whole path about +Y in the engine's rotation convention: the direction at North n
// is the direction at North 0 turned by Quaternion::FromAxisAngle(+Y, n), so +90 puts the noon sun
// of a northern site at -X (its south) and sunrise at -Z.
TEST(SolarPath, NorthTurnsTheSunAboutUp)
{
    constexpr float kTolerance = 1e-5f;
    using GameEngine::Mathematics::Quaternion;
    using GameEngine::Mathematics::Vector3;

    for (const float north : {37.0f, 90.0f, -120.0f})
    {
        const SolarFrame reference = MakeSolarFrame(EarthPathAngles({51.5f, 172, 0.0f}));
        const SolarFrame turned = MakeSolarFrame(EarthPathAngles({51.5f, 172, north}));
        const Quaternion yaw =
            Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), north * static_cast<float>(kPi / 180.0));
        for (float hours = 0.0f; hours < 24.0f; hours += 1.5f)
        {
            float before[3];
            float after[3];
            SunAtHour(reference, hours, before);
            SunAtHour(turned, hours, after);
            const Vector3 expected = yaw.Rotate(Vector3(before[0], before[1], before[2]));
            EXPECT_NEAR(after[0], expected.x, kTolerance) << "north " << north << " hour " << hours;
            EXPECT_NEAR(after[1], expected.y, kTolerance) << "north " << north << " hour " << hours;
            EXPECT_NEAR(after[2], expected.z, kTolerance) << "north " << north << " hour " << hours;
        }
    }

    // Elevation and azimuth are read in the site's own frame, so North leaves them unchanged.
    for (float hours = 0.0f; hours < 24.0f; hours += 1.5f)
    {
        const SolarPosition reference = SolarPositionAtHour(MakeSolarFrame(EarthPathAngles({51.5f, 172, 0.0f})), hours);
        const SolarPosition turned = SolarPositionAtHour(MakeSolarFrame(EarthPathAngles({51.5f, 172, 37.0f})), hours);
        EXPECT_NEAR(turned.ElevationDegrees, reference.ElevationDegrees, 1e-3f) << "hour " << hours;
        const float azimuthDifference = std::abs(turned.AzimuthDegrees - reference.AzimuthDegrees);
        EXPECT_LT(std::min(azimuthDifference, 360.0f - azimuthDifference), 1e-3f) << "hour " << hours;
    }

    float noon[3];
    SunAtHour(MakeSolarFrame(EarthPathAngles({51.5f, 172, 90.0f})), 12.0f, noon);
    EXPECT_LT(noon[0], -0.1f) << "with north at +X, a northern noon sun leans to -X";
}

// The Custom path at its defaults (a level axis at heading 0, a noon height of 90) is the default
// Earth path to the bit: switching an untouched sky from Earth to Custom moves nothing.
TEST(SolarPath, ACustomFrameAtTheDefaultsIsTheEarthFrame)
{
    const SolarFrame earth = MakeSolarFrame(EarthPathAngles({}));
    const SolarFrame custom = MakeSolarFrame(CustomPathAngles(0.0f, 0.0f, kOverheadNoonHeightDegrees));
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(Bits(custom.East[i]), Bits(earth.East[i])) << i;
        EXPECT_EQ(Bits(custom.Meridian[i]), Bits(earth.Meridian[i])) << i;
        EXPECT_EQ(Bits(custom.Pole[i]), Bits(earth.Pole[i])) << i;
    }
    EXPECT_EQ(Bits(custom.CosDeclination), Bits(earth.CosDeclination));
    EXPECT_EQ(Bits(custom.SinDeclination), Bits(earth.SinDeclination));
}

// Worked by hand from declination = noon height + axis altitude - 90. An axis straight up with a
// noon height of 90 puts the declination at 90: the sun sits on the axis, overhead all day. A level
// axis with a noon height of 0 puts it at -90: the sun sits on the horizon, away from the axis
// heading, all day. An axis 30 degrees up with a noon height of 120, over the top, puts it at 60:
// the noon sun is 60 degrees high on the axis side (azimuth 0, toward the heading).
TEST(SolarPath, CustomNoonHeightsWorkedByHand)
{
    constexpr float kToleranceDegrees = 1e-3f;

    const SolarFrame overhead = MakeSolarFrame(CustomPathAngles(90.0f, 0.0f, 90.0f));
    EXPECT_NEAR(SolarPositionAtHour(overhead, 12.0f).ElevationDegrees, 90.0f, kToleranceDegrees);
    EXPECT_NEAR(SolarNoonElevationDegrees(CustomPathAngles(90.0f, 0.0f, 90.0f)), 90.0f, kToleranceDegrees);

    const SolarPathAngles horizonAngles = CustomPathAngles(0.0f, 0.0f, 0.0f);
    const SolarPosition onTheHorizon = SolarPositionAtHour(MakeSolarFrame(horizonAngles), 12.0f);
    EXPECT_NEAR(onTheHorizon.ElevationDegrees, 0.0f, kToleranceDegrees);
    EXPECT_NEAR(onTheHorizon.AzimuthDegrees, 180.0f, kToleranceDegrees) << "away from the axis heading";
    EXPECT_NEAR(SolarNoonElevationDegrees(horizonAngles), 0.0f, kToleranceDegrees);

    const SolarPathAngles overTheTopAngles = CustomPathAngles(30.0f, 0.0f, 120.0f);
    const SolarPosition overTheTop = SolarPositionAtHour(MakeSolarFrame(overTheTopAngles), 12.0f);
    EXPECT_NEAR(overTheTop.ElevationDegrees, 60.0f, kToleranceDegrees);
    const float azimuthFromHeading = std::min(overTheTop.AzimuthDegrees, 360.0f - overTheTop.AzimuthDegrees);
    EXPECT_NEAR(azimuthFromHeading, 0.0f, kToleranceDegrees) << "on the axis side";
    EXPECT_NEAR(SolarNoonElevationDegrees(overTheTopAngles), 60.0f, kToleranceDegrees);
    EXPECT_NEAR(static_cast<float>(overTheTopAngles.DeclinationRadians * kDegreesPerRadian), 60.0f, kToleranceDegrees);
}

// An axis and noon height built from a real place and day reproduce the Earth path: London
// culminates on the equator side (noon height = its noon elevation), Sydney on the pole side in June
// (noon height = 180 - its noon elevation). The branch problem the over-the-top angle solves.
TEST(SolarPath, ACustomFrameReproducesEarth)
{
    constexpr double kDeclinationToleranceDegrees = 1e-4;
    constexpr float kPositionToleranceDegrees = 0.01f;
    struct Place
    {
        float Latitude;
        int32_t Day;
    };
    for (const Place place : {Place{51.5f, 172}, Place{51.5f, 355}, Place{-33.9f, 172}, Place{-33.9f, 355}})
    {
        const SolarPathAngles earth = EarthPathAngles({place.Latitude, place.Day, 0.0f});
        const double declinationDegrees = earth.DeclinationRadians * kDegreesPerRadian;
        // Over the top: the noon sun's angle from the horizon away from the pole's heading.
        const float noonHeight = static_cast<float>(90.0 + declinationDegrees - place.Latitude);
        const SolarPathAngles custom = CustomPathAngles(place.Latitude, 0.0f, noonHeight);
        EXPECT_NEAR(custom.DeclinationRadians * kDegreesPerRadian, declinationDegrees, kDeclinationToleranceDegrees)
            << place.Latitude << " day " << place.Day;

        const SolarFrame earthFrame = MakeSolarFrame(earth);
        const SolarFrame customFrame = MakeSolarFrame(custom);
        for (float hours = 0.0f; hours < 24.0f; hours += 0.75f)
        {
            const SolarPosition expected = SolarPositionAtHour(earthFrame, hours);
            const SolarPosition actual = SolarPositionAtHour(customFrame, hours);
            EXPECT_NEAR(actual.ElevationDegrees, expected.ElevationDegrees, kPositionToleranceDegrees)
                << place.Latitude << " day " << place.Day << " hour " << hours;
        }
        EXPECT_NEAR(SolarDayLengthHours(custom), SolarDayLengthHours(earth), 1e-3f);
    }
}

// An axis straight up with a noon height of 30: the sun circles the horizon at 30 degrees all day
// and never sets. A day-length formula still tied to a latitude, or a heading that turns NaN at the
// pole, fails here.
TEST(SolarPath, AVerticalAxisGivesASunThatNeverSets)
{
    constexpr float kToleranceDegrees = 1e-3f;
    const SolarPathAngles angles = CustomPathAngles(90.0f, 0.0f, 30.0f);
    const SolarFrame frame = MakeSolarFrame(angles);
    for (int minute = 0; minute < 24 * 60; ++minute)
    {
        const SolarPosition position = SolarPositionAtHour(frame, static_cast<float>(minute) / 60.0f);
        EXPECT_NEAR(position.ElevationDegrees, 30.0f, kToleranceDegrees) << "minute " << minute;
        EXPECT_TRUE(std::isfinite(position.AzimuthDegrees)) << "minute " << minute;
    }
    EXPECT_FLOAT_EQ(SolarDayLengthHours(angles), 24.0f);
    EXPECT_NEAR(SolarMidnightElevationDegrees(angles), 30.0f, kToleranceDegrees);
}

// The declination stays in [-90, 90]: a noon height the axis cannot reach is clamped to the nearest
// one it can, so the path is still a real circle; the reachable range is what the inspector clamps to.
TEST(SolarPath, TheNoonHeightClampsToTheReachableRange)
{
    constexpr double kToleranceDegrees = 1e-9;
    EXPECT_NEAR(CustomPathAngles(60.0f, 0.0f, 170.0f).DeclinationRadians * kDegreesPerRadian, 90.0, kToleranceDegrees);
    EXPECT_NEAR(CustomPathAngles(-60.0f, 0.0f, 10.0f).DeclinationRadians * kDegreesPerRadian, -90.0, kToleranceDegrees);
    EXPECT_NEAR(CustomPathAngles(0.0f, 0.0f, 400.0f).DeclinationRadians * kDegreesPerRadian, 90.0, kToleranceDegrees);
    EXPECT_NEAR(CustomPathAngles(0.0f, 0.0f, std::nanf("")).DeclinationRadians, 0.0, kToleranceDegrees)
        << "a non-finite noon height reads as overhead";

    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(170.0f, 60.0f), 120.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(10.0f, -60.0f), 60.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(45.0f, 30.0f), 45.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(std::nanf(""), 0.0f), kOverheadNoonHeightDegrees);
    // The reach runs below the horizon: an axis 75 degrees up (a polar site) reaches a noon sun down
    // to 75 degrees under the horizon on the away side; one 30 degrees down reaches 210 on the axis
    // side; a level axis reaches neither side below the horizon.
    EXPECT_FLOAT_EQ(ReachableNoonHeights(75.0f).Lowest, -75.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeights(75.0f).Highest, 105.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(-8.44f, 75.0f), -8.44f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(-80.0f, 75.0f), -75.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(200.0f, -30.0f), 200.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(-30.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(ReachableNoonHeightDegrees(190.0f, 0.0f), 180.0f);
    EXPECT_NEAR(CustomPathAngles(75.0f, 0.0f, -8.44f).DeclinationRadians * kDegreesPerRadian, -23.44, 1e-4);
    EXPECT_NEAR(SolarNoonElevationDegrees(CustomPathAngles(75.0f, 0.0f, -8.44f)), -8.44f, 1e-4f);
    EXPECT_FLOAT_EQ(SolarDayLengthHours(CustomPathAngles(75.0f, 0.0f, -8.44f)), 0.0f);
    EXPECT_NEAR(SolarNoonElevationDegrees(CustomPathAngles(-30.0f, 0.0f, 200.0f)), -20.0f, 1e-4f);
}

// The lowest point of the day: at 70 degrees north in June the midnight sun stays up.
TEST(SolarPath, TheMidnightElevationIsTheLowestOfTheDay)
{
    const SolarPathAngles summer = EarthPathAngles({70.0f, 172, 0.0f});
    const float midnight = SolarMidnightElevationDegrees(summer);
    EXPECT_GT(midnight, 0.0f);
    EXPECT_NEAR(midnight, SolarPositionAtHour(MakeSolarFrame(summer), 0.0f).ElevationDegrees, 1e-3f);
    EXPECT_NEAR(SolarMidnightElevationDegrees(EarthPathAngles({})), -90.0f, 1e-4f);
}
