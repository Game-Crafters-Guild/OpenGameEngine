// Shared parameters + spectrum math for the ocean FFT compute chain.
// Faithful port of the reference's FFTSpectrum.compute + OceanWaveSpectrum (MIT):
// per-octave power LUT, Pierson-Moskowitz wind term, deep-water dispersion,
// directional spreading, Gaussian amplitudes, and the per-cascade frequency
// band split. v1 targets full parity; v2 optimizes (resolution/cascade tiers).
//
// 16 cascades, each a tileable patch of worldSize = 0.5 * 2^cascade meters.
// SpectrumInit masks each cascade to a narrow frequency band (the largest
// cascade keeps all low frequencies), so summing the cascades reconstructs the
// full ocean spectrum from 6 cm ripples to km-scale swell.

#ifndef GE_OCEAN_FFT_COMMON_GLSL
#define GE_OCEAN_FFT_COMMON_GLSL

const float GE_FFT_PI  = 3.14159265358979;
const float GE_FFT_PI2 = 6.28318530717959;

// the reference directional-spread constants (verbatim).
const float GE_FFT_INVPI2 = 0.63661977236; // 2/pi   (cos^2 lobe)
const float GE_FFT_PI4    = 0.33661977236; // the reference PI4 (isotropic floor)

// FFT working resolution. Must match the cascade texture size and
// kOceanFFTResolution in C++. 256 -> 8 butterfly passes.
const int GE_FFT_SIZE   = 256;
const int GE_FFT_PASSES = 8; // log2(GE_FFT_SIZE)

// Must match CASCADE_COUNT in C++ and the cascade-array layer count.
const int GE_FFT_CASCADES = 16;

// the reference OceanWaveSpectrum constants.
const int   GE_SPECTRUM_OCTAVES        = 14;
const float GE_SPECTRUM_SMALLEST_WL_P2 = -4.0;
const float GE_WAVE_SAMPLE_FACTOR      = 8.0;

// Per-frame FFT parameters. std140 layout must match OceanFFTParamsGPU in C++.
layout(std140, set = 0, binding = 0) uniform OceanFFTParams
{
    uint  uResolution;   // = GE_FFT_SIZE
    uint  uCascadeCount; // = GE_FFT_CASCADES (active layers)
    float uGravity;      // 9.81
    float uTime;         // sim time * gravityScale (seconds)

    float uWindSpeed;    // m/s
    float uWindDirX;
    float uWindDirZ;
    float uTurbulence;   // the reference _Turbulence / WindTurbulence (0.145)

    float uChop;         // global chop scale (multiplies the per-octave chop)
    float uMultiplier;   // spectrum multiplier (already squared into the LUT)
    float uPeriod;       // loop period (0 = none)
    float uFFTPad0;

    // Per-octave spectrum LUTs (14 octaves used), packed 4-per-vec4. Mirror of
    // OceanFFTParamsGPU's SpectrumPower / ChopScales / GravityScales arrays.
    vec4  uSpectrumPower[4];  // linear power per octave
    vec4  uChopScales[4];     // per-octave horizontal-displacement multiplier
    vec4  uGravityScales[4];  // per-octave dispersion gravity multiplier

    // Per-octave disable bitmask (bit o => octave o muted). 14 bits used.
    uint  uOctaveDisableMask;
    uint  uFFTPad1;
    uint  uFFTPad2;
    uint  uFFTPad3;
};

// Per-cascade tileable patch size (the reference: 0.5 * 2^cascade).
float OceanFFTWorldSize(uint cascade)
{
    return 0.5 * float(1u << cascade);
}

// LUT lookup with linear interpolation + clamp (matches the reference's linear_clamp
// SampleLevel over a 14-texel R32F texture sampled at (octave+0.5)/14).
float OceanSpectrumOctavePower(int octave)
{
    octave = clamp(octave, 0, GE_SPECTRUM_OCTAVES - 1);
    return uSpectrumPower[octave >> 2][octave & 3];
}

float OceanSampleSpectrum(float octaveIndex)
{
    float t = clamp(octaveIndex, 0.0, float(GE_SPECTRUM_OCTAVES - 1));
    int i0 = int(floor(t));
    int i1 = min(i0 + 1, GE_SPECTRUM_OCTAVES - 1);
    return mix(OceanSpectrumOctavePower(i0), OceanSpectrumOctavePower(i1), t - float(i0));
}

// True when the octave nearest octaveIndex is muted by the disable mask. The
// init pass zeroes the spectrum for muted octaves so they contribute no energy.
bool OceanOctaveDisabled(float octaveIndex)
{
    int o = clamp(int(floor(octaveIndex + 0.5)), 0, GE_SPECTRUM_OCTAVES - 1);
    return (uOctaveDisableMask & (1u << uint(o))) != 0u;
}

// Per-octave chop multiplier (linearly interpolated, like the power LUT). The
// global uChop scales the result. Used in the update pass per-texel.
float OceanSampleChop(float octaveIndex)
{
    float t = clamp(octaveIndex, 0.0, float(GE_SPECTRUM_OCTAVES - 1));
    int i0 = int(floor(t));
    int i1 = min(i0 + 1, GE_SPECTRUM_OCTAVES - 1);
    float c0 = uChopScales[i0 >> 2][i0 & 3];
    float c1 = uChopScales[i1 >> 2][i1 & 3];
    return uChop * mix(c0, c1, t - float(i0));
}

// Per-octave dispersion-gravity multiplier (linearly interpolated). Applied to
// uGravity so a band's waves can be sped up or slowed independently.
float OceanSampleGravityScale(float octaveIndex)
{
    float t = clamp(octaveIndex, 0.0, float(GE_SPECTRUM_OCTAVES - 1));
    int i0 = int(floor(t));
    int i1 = min(i0 + 1, GE_SPECTRUM_OCTAVES - 1);
    float g0 = uGravityScales[i0 >> 2][i0 & 3];
    float g1 = uGravityScales[i1 >> 2][i1 & 3];
    return mix(g0, g1, t - float(i0));
}

// ── Random (the reference: WangHash seed + xorshift + Box-Muller Gaussian) ────────────
uint OceanWangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

uint OceanRandUint(inout uint rngState)
{
    rngState ^= (rngState << 13u);
    rngState ^= (rngState >> 17u);
    rngState ^= (rngState << 5u);
    return rngState;
}

float OceanRandFloat(inout uint rngState)
{
    return float(OceanRandUint(rngState)) / 4294967296.0;
}

float OceanRandGauss(inout uint rngState)
{
    float u1 = OceanRandFloat(rngState);
    float u2 = OceanRandFloat(rngState);
    if (u1 < 1e-6)
        u1 = 1e-6;
    return sqrt(-2.0 * log(u1)) * cos(GE_FFT_PI2 * u2);
}

// ── Dispersion + spectrum (the reference deep-water + Pierson-Moskowitz wind term) ────
// gravity is the effective per-octave gravity (uGravity * the octave's gravity
// scale), so a band's wave speed can be tuned independently. Callers that don't
// need per-octave tuning pass uGravity.
void OceanDeepDispersionG(float k, float gravity, out float w, out float dwdk)
{
    w = sqrt(abs(gravity * k));

    if (uPeriod > 0.0 && w > 1.0e-6)
    {
        float thisPeriod = GE_FFT_PI2 / w;
        float loops = ceil(uPeriod / thisPeriod);
        thisPeriod = uPeriod / loops;
        w = GE_FFT_PI2 / thisPeriod;
    }

    dwdk = gravity / (2.0 * max(w, 1.0e-6));
}

void OceanDeepDispersion(float k, out float w, out float dwdk)
{
    OceanDeepDispersionG(k, uGravity, w, dwdk);
}

float OceanPiersonMoskowitzWindTerm(float w)
{
    if (uWindSpeed <= 0.0 || w <= 0.0)
        return 0.0;

    float wm = 0.87 * uGravity / uWindSpeed;
    return exp(-1.291 * pow(wm / w, 4.0));
}

float OceanDirectionalSpread(float cosTheta)
{
    if (cosTheta > 0.0)
        return mix(GE_FFT_INVPI2 * (cosTheta * cosTheta), GE_FFT_PI4, uTurbulence);
    return GE_FFT_PI4 * uTurbulence;
}

vec2 OceanCMul(vec2 a, vec2 b)
{
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

#endif // GE_OCEAN_FFT_COMMON_GLSL
