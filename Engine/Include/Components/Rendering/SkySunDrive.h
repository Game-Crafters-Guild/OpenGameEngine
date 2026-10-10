#pragma once

#include "Mathematics/Curve.h" // Math::Curve, Math::CurveKey

#include <vector>

// What a sky drives into its sun light beyond the direction: the illuminance, either the light's own
// (the author's clear-noon value, dimmed through the colour) or a curve in lux over the day, and the
// moonlight that owns the light at night. The sky system writes through these, and the sky inspector
// draws and seeds the curve from them, so what an author sees is what the frame does.
//
// Free functions only: this directory is scanned for components, and every struct here would
// register as one.
namespace GameEngine::Components
{
struct SkyEnvironment;

namespace SkySunDrive
{

// The smallest illuminance the curve distinguishes from darkness, in lux. Keys are interpolated in
// log2 of their value (Math::EvaluateCurveKeysLogarithmic), so a key at or below this floor fades its
// segment out instead of reaching minus infinity, and anything that evaluates at or below it reads 0.
inline constexpr float kCurveFloorLux = 0.001f;

// The sun illuminance curve at `hours` of local solar time, in lux: its keys interpolated in the log
// domain over a 24-hour day that wraps from the last key to the first.
float CurveIlluminanceLux(const Math::Curve& curve, float hours);

// What the physical model delivers to a surface facing the sun at `hours` on the sky's path, in lux,
// for a clear, overhead sun of `sunLux`: the sun alone, with no handover to the moon (P(t)).
float PhysicalIlluminanceLux(const SkyEnvironment& sky, float sunLux, float hours);

// The hours of the sky's day when its light is the sun's alone, with no handover to the moon: the sun
// above 1.15 degrees, from `outStart` to `outEnd`. False when the sun never climbs that high (the whole
// day hands over); true with 0 and 24 when it never drops below it.
bool SunOnlyHours(const SkyEnvironment& sky, float& outStart, float& outEnd);

// The physical curve sampled every 15 minutes over the day, 97 samples from 00:00 to 24:00, for a
// read-only preview drawn behind the authored curve.
std::vector<Math::CurveKey> PhysicalCurveSamples(const SkyEnvironment& sky, float sunLux);

// A curve that follows the physical model for the sky's path and a clear, overhead sun of `sunLux`:
// keys where the sun crosses -6, -4, -2, -1, 0, 1, 2, 5, 10, 20, 35 and 55 degrees on either side of
// noon (those it reaches), plus noon and midnight, then refined, up to the curve's 32 keys, until it
// is within half a stop of the model wherever the model gives more than 0.01 lx. Hourly keys on a day
// whose sun never rises or never sets. Linear segments below 5 degrees, smooth ones above.
Math::Curve PhysicalSeedCurve(const SkyEnvironment& sky, float sunLux);

// The curve a sky holds until an author edits it: the physical curve of the default sky (the equator
// on the March equinox) for a 100 000 lx sun, stored as literal keys so that constructing a sky never
// runs the model.
Math::Curve DefaultSunIlluminanceCurve();

// True when `curve` is, key for key and bit for bit, DefaultSunIlluminanceCurve: a curve no one has
// authored, which the first switch to the curve may replace with the physical seed.
bool IsDefaultSunIlluminanceCurve(const Math::Curve& curve);

// The first switch to the curve: while the sky's curve is still DefaultSunIlluminanceCurve, replace
// it with PhysicalSeedCurve for the sky's path and `sunLux`, so the switch looks the same as the
// light at every hour. A curve anyone has authored is kept. True when it seeded.
bool SeedCurveIfUnauthored(SkyEnvironment& sky, float sunLux);

// The sky's brightness reference while the curve drives the light, in lux (R): the curve's value at
// solar noon with the model's noon extinction undone, so the sky's own sun delivers the curve's noon.
// The extinction is floored at its value for a sun 1.15 degrees up, where the handover to the moon
// begins, which keeps the reference continuous as the noon sun sinks toward polar night.
float NoonReferenceLux(const SkyEnvironment& sky);

// The sky's own sun while the curve drives the light, in lux, at `hours`: a clear sun
// (kClearNoonSunIlluminanceLux) scaled by the curve over the physical model at that hour, so the
// dome delivers what the curve delivers now, at every hour of a curve of any shape. Outside the
// hours the sun alone holds the light (SunOnlyHours) the ratio is held at the nearer of their ends,
// where the model still gives light; a day with no such hours takes NoonReferenceLux.
float SkySourceLux(const SkyEnvironment& sky, float hours);

// The illuminance the moon delivers to a surface facing it, in lux (M(t)), when its direction has
// `moonUpDot` as its up component: the sky's Moonlight, extinguished by the air at the moon's
// elevation. 0 while the moon is hidden.
float MoonlightLux(const SkyEnvironment& sky, float moonUpDot);

// The same from the moon's ground colour already evaluated for the frame (Rendering::SkyBodyGroundColors::Moon,
// SkySystemConfig::moonColor extinguished at the moon's elevation), so the frame runs the moon's
// extinction once.
float MoonlightLuxFromGround(const SkyEnvironment& sky, const float moonGround[3]);

// The sky's Moonlight as a multiple of the default moonlight: exactly 1 at the default.
float MoonlightRatio(const SkyEnvironment& sky);

// The sky's Moonlight sanitised: a non-finite value is the default, then clamped to
// [0, kMoonlightIlluminanceMaxLux].
float SanitisedMoonlightLux(float lux);

} // namespace SkySunDrive
} // namespace GameEngine::Components
