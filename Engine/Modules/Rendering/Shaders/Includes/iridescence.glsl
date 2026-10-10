// Thin-film iridescence (Belcour-Barla 2017), practical depolarized form.
// nm-direct parameterization (filmThickness in nanometres, ~0..2000 visible range). Note the
// OpenPBR thin_film_thickness is in MICROMETRES; the MaterialX importer converts it to nm. Output: the RGB spectral
// reflectance R(lambda) of the air/film/base stack, which SUBSTITUTES for the
// specular Fresnel (Schlick/F82) in both the direct GGX lobe and the IBL split-sum.
//
// Requires GE_PI and GE_DielectricF0Scalar — included by standard_pbr.glsl after
// both are defined, so the direct path and (transitively) the IBL path both see it.

#ifndef GE_IRIDESCENCE_GLSL
#define GE_IRIDESCENCE_GLSL

// Films thinner than this (nm) carry no visible interference band; eta2 ramps from 1
// (no film) up to filmIor across this range so the zero-thickness limit has no seam.
const float GE_THIN_FILM_FADE_NM = 30.0;

// CIE-XYZ spectral-sensitivity Fourier fit (Belcour-Barla 2017), matching the OpenPBR
// thin_film parameterization. A closed-form spectral integral, so there is no
// per-wavelength loop. `opd` is in nm, so the phase uses 1e-9 (nm -> m); the
// coefficients are a fixed data-table fit, not a tunable.
vec3 GE_IridescenceSensitivity(float opd, float shift)
{
    float phase = 2.0 * GE_PI * opd * 1.0e-9;
    vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3 xyz = val * sqrt(2.0 * GE_PI * var) * cos(pos * phase + shift) * exp(-var * phase * phase);
    // The X response is bimodal — add its second lobe.
    xyz.x += 9.7470e-14 * sqrt(2.0 * GE_PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift) * exp(-4.5282e+09 * phase * phase);
    return xyz / 1.0685e-7;
}

// Iridescent RGB reflectance of the air/film/base stack (simplified, depolarized:
// no explicit polarization split, base treated through its dielectric/metal F0).
// cosTheta1 = the interface cosine (VdotH for the direct lobe, NdotV for IBL);
// F0base = the SAME base specular F0 the lobe would otherwise feed to Schlick, so a
// tinted metal or coloured dielectric modulates the interference. Returns vec3 in [0,1].
//
// Inert state is driven by thinFilmWeight = 0 at the CALL SITE (the default), where the
// caller's mix() returns the base Fresnel unchanged. The eta2->1 fade here only removes
// the discontinuity at the zero-thickness limit (it does not by itself reproduce the
// grazing F82 ramp, so it is not relied on as the off switch).
vec3 GE_Iridescence(float cosTheta1, float filmIor, float filmThicknessNm, vec3 F0base)
{
    float eta2 = mix(1.0, filmIor, smoothstep(0.0, GE_THIN_FILM_FADE_NM, filmThicknessNm));

    // Refract into the film (Snell). Past the critical angle there is no transmission;
    // the clamp keeps cosTheta2 real and the stack degenerates gracefully to a mirror.
    float sinTheta2Sq = (1.0 / (eta2 * eta2)) * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2 = sqrt(max(1.0 - sinTheta2Sq, 0.0));

    // First interface (air -> film): achromatic dielectric, PI phase jump (low->high index).
    // Uses the raw film IOR; the eta2 fade drives only the Snell + OPD path below.
    float f0_12 = GE_DielectricF0Scalar(filmIor);  // ((filmIor-1)/(filmIor+1))^2
    float R12 = GE_SchlickFresnel(vec3(f0_12), cosTheta1).x;
    float T121 = 1.0 - R12;
    float phi21 = GE_PI;

    // Second interface (film -> base): chromatic, reuses the base F0 (tints the rainbow).
    vec3 R23 = F0base;
    float phi23 = 0.0;

    float opd = 2.0 * filmThicknessNm * eta2 * cosTheta2;  // nm
    float phi2 = phi21 + phi23;

    // Airy series compounded in the frequency domain.
    vec3 R123 = R12 * R23;
    vec3 r123 = sqrt(max(R123, 0.0));
    vec3 Rs = (T121 * T121) * R23 / max(vec3(1.0) - R123, 1e-4);

    // m = 0 (DC term).
    vec3 C0 = vec3(R12) + Rs;
    vec3 I = C0 * GE_IridescenceSensitivity(0.0, 0.0);

    // m = 1..3 (interference harmonics).
    vec3 Cm = Rs - vec3(T121);
    for (int m = 1; m <= 3; ++m)
    {
        Cm *= r123;
        I += Cm * 2.0 * GE_IridescenceSensitivity(float(m) * opd, float(m) * phi2);
    }

    // XYZ -> linear RGB (CIE 1931, neutral E illuminant), clamped to a valid reflectance.
    // GLSL mat3 is column-major, so these args are the transpose of the row-major matrix:
    // each column below is a row of the math matrix (rows sum to 1 -> neutral XYZ stays neutral).
    const mat3 kXyzToRgb = mat3(
         2.3706743, -0.5138850,  0.0052982,
        -0.9000405,  1.4253036, -0.0146949,
        -0.4706338,  0.0885814,  1.0093968);
    return clamp(kXyzToRgb * I, 0.0, 1.0);
}

#endif // GE_IRIDESCENCE_GLSL
