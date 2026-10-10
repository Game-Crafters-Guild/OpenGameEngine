// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.
//
// ACES 2.0 Rec.709 Output Transform with peak-luminance tiers. The transform
// body below is OpenColorIO 2.5.2's official ACES 2 fixed-function GPU code
// (Rec.709 -> ACES2065-1, tonescale/chroma/gamut transform, CIE XYZ D65 ->
// linear Rec.709), with the peak-baked scalars and the three 363-entry tables
// lifted into per-tier data blended at runtime. Tier data lives in the
// Aces2Tables device buffer (set 0, binding 2; block declared in generated
// tonemap_aces2_tables.glsl, payload uploaded once per device by TonemapPass
// from generated TonemapAces2Tables.h) — NOT in shader constants: glslang
// materializes dynamically indexed const arrays into per-invocation scratch
// memory, which at this size faults the device. This file's body is
// hand-maintained and must be re-derived from the generator's emission if the
// OCIO pin changes (procedure: Tools/ShaderGen/README.md).
// Output is display-referred linear spanning [0, outputMax] with 1.0 at paper
// white; the caller routes it display-referred (no highlight extension).
// https://github.com/AcademySoftwareFoundation/OpenColorIO
// See ThirdParty/OpenColorIO/LICENSE.txt (full BSD-3-Clause text) and UPSTREAM.md.

#ifndef GE_TONEMAP_ACES2_GLSL
#define GE_TONEMAP_ACES2_GLSL

#include "tonemap_aces2_tables.glsl"

// Peak-dependent parameters of the transform, derived by Aces2SelectTier from
// the two tiers bracketing the display's headroom: six closed forms in the
// tier coordinate, geometric mixes for the power-law-of-peak quantities, and
// linear mixes for angles and log2-affine quantities (the F1 scheme). Slot
// indices mirror the generator's EMITTED_SLOTS layout. The blend shapes are
// mirrored exactly by tier_at() in Tools/ShaderGen/validate_aces2_tiers.py —
// change both together or gates G3-G6 measure the wrong thing.
struct Aces2TierParams
{
    float tsFScale;       // tonescale forward shoulder scale
    float tsSDiv;         // tonescale forward shoulder divisor
    float limitJMax;      // CAM J of the tier peak luminance
    float midJ;           // focus-J anchor (J of mid grey at the tier peak)
    float focusGainBase;  // gamut-mapper slope-gain base
    float gammaBottomInv; // lower-hull gamma reciprocal
    float inputClampAP1;  // pre-transform AP1 domain clamp
    vec3  mnormCosW;      // chroma-compress Mnorm Fourier cosine weights
    vec3  mnormSinW;      // chroma-compress Mnorm Fourier sine weights
    float mnormOffset;    // chroma-compress Mnorm constant term
    float toeLowScale;    // chroma toe stage-1 snJ coefficient
    float toeK2Bias;      // chroma toe k2 bias
    float toeHighScale;   // chroma toe stage-2 nJ coefficient
    float outClamp;       // display-linear ceiling (tier peak / 100)
};

int aces2_tier_lo = 0;
int aces2_tier_hi = 0;
float aces2_tier_t = 0.0;
Aces2TierParams aces2_p;

// Endpoint-exact geometric mix: returns a exactly at t = 0 (exp2(0) == 1), so
// SDR stays byte-identical to tier 0. Valid only for same-signed nonzero
// pairs — the generator asserts positivity for every geo-blended slot and
// table entry, and constant per-slot signs for the Mnorm weights.
float Aces2MixGeo(float a, float b, float t)
{
    return a * exp2(t * log2(b / a));
}

// Reach M is a power law of peak: geometric per entry.
float Aces2ReachM(int i)
{
    return Aces2MixGeo(aces2_reach_m_tables[aces2_tier_lo][i], aces2_reach_m_tables[aces2_tier_hi][i], aces2_tier_t);
}

// Cusp J and M are power laws of peak (geometric); the upper-hull gamma in .z
// is log2-affine (linear). Entries are vec4 in the buffer (std430 packing);
// .w is unused.
vec3 Aces2GamutCusp(int i)
{
    vec4 lo = aces2_gamut_cusp_tables[aces2_tier_lo][i];
    vec4 hi = aces2_gamut_cusp_tables[aces2_tier_hi][i];
    return vec3(Aces2MixGeo(lo.x, hi.x, aces2_tier_t),
                Aces2MixGeo(lo.y, hi.y, aces2_tier_t),
                mix(lo.z, hi.z, aces2_tier_t));
}

// Hue positions are angles: always linear, never geometric.
float Aces2CuspHue(int i)
{
    return mix(aces2_hues_tables[aces2_tier_lo][i], aces2_hues_tables[aces2_tier_hi][i], aces2_tier_t);
}

float Aces2ScalarLin(int slot)
{
    return mix(aces2_tier_scalars[aces2_tier_lo][slot], aces2_tier_scalars[aces2_tier_hi][slot], aces2_tier_t);
}

float Aces2ScalarGeo(int slot)
{
    return Aces2MixGeo(aces2_tier_scalars[aces2_tier_lo][slot], aces2_tier_scalars[aces2_tier_hi][slot], aces2_tier_t);
}

// Selects the tier pair bracketing the display headroom and derives the
// peak-baked parameters (the F1 blend). The ladder is 100*2^k nit OCIO peaks,
// so idx = log2 of the paper-white-relative headroom is the tier coordinate;
// SDR (outputMax 1.0) lands exactly on tier 0 with t = 0 and every parameter
// equal to the tier-0 table value, and panels beyond 32x headroom saturate on
// the last tier.
//
// Closed forms (generator-asserted against OCIO's emitted per-tier values at
// rel 1e-5; coefficients from the design doc):
//   inputClampAP1 = 8*(128 + 768*log10(peak/100)/2) = 1024 + 924.7641466*idx
//   outClamp      = peak/100                        = exp2(idx)
//   toeK2Bias     = 0.5/peak                        = 0.005*exp2(-idx)
//   toeHighScale  = 2.4000001 + 2.3841577*idx
//   toeLowScale   = max(0.2, 1.29999995 - 0.27002399*idx)
//   focusGainBase = limitJMax * (1.35 + 0.71118337*idx)
// The seven Mnorm Fourier weights share one peak scale factor
// (generator-asserted), blended geometrically once from the mnormOffset slot
// and applied to the lower tier's weights, preserving each weight's sign.
void Aces2SelectTier(float outputMax)
{
    float idx = clamp(log2(max(outputMax, 1.0)), 0.0, 5.0);
    int lo = int(min(idx, 4.0));
    aces2_tier_lo = lo;
    aces2_tier_hi = lo + 1;
    aces2_tier_t = idx - float(lo);
    aces2_p.tsFScale       = Aces2ScalarGeo(0);
    aces2_p.tsSDiv         = Aces2ScalarGeo(1);
    aces2_p.limitJMax      = Aces2ScalarGeo(2);
    aces2_p.midJ           = Aces2ScalarLin(3);
    aces2_p.gammaBottomInv = Aces2ScalarGeo(4);
    float mnormScale = exp2(aces2_tier_t * log2(aces2_tier_scalars[aces2_tier_hi][11] / aces2_tier_scalars[lo][11]));
    aces2_p.mnormCosW      = vec3(aces2_tier_scalars[lo][5], aces2_tier_scalars[lo][6], aces2_tier_scalars[lo][7]) * mnormScale;
    aces2_p.mnormSinW      = vec3(aces2_tier_scalars[lo][8], aces2_tier_scalars[lo][9], aces2_tier_scalars[lo][10]) * mnormScale;
    aces2_p.mnormOffset    = aces2_tier_scalars[lo][11] * mnormScale;
    aces2_p.inputClampAP1  = 1024.0 + 924.7641466 * idx;
    aces2_p.outClamp       = exp2(idx);
    aces2_p.toeK2Bias      = 0.005 * exp2(-idx);
    aces2_p.toeHighScale   = 2.4000001 + 2.3841577 * idx;
    aces2_p.toeLowScale    = max(0.2, 1.29999995 - 0.27002399 * idx);
    aces2_p.focusGainBase  = aces2_p.limitJMax * (1.35 + 0.71118337 * idx);
}

// Declaration of all helper methods

float aces2_reach_m_table_0_sample(float h)
{
  float i_base = floor(h);
  float i_lo = i_base + float(1);
  float i_hi = i_lo + 1.0;
  float lo = Aces2ReachM(int(i_lo));
  float hi = Aces2ReachM(int(i_hi));
  float t = h - i_base;
  return mix(lo, hi, t);
}
float aces2_tonescale_fwd0(float J)
{
  float A = 0.0323680267 * pow(abs(J) * 0.00999999978, 0.879464149);
  float Y = pow(( 27.1299992 * A) / (1.0f - A), 2.3809523809523809);
  float f = aces2_p.tsFScale * pow(Y / (Y + aces2_p.tsSDiv), 1.14999998);
  float Y_ts = max(0.0, f * f / (f + 0.0399999991));
  float F_L_Y = pow(0.79370057210326195 * Y_ts, 0.42);
  float J_ts = 100. * pow((F_L_Y / ( 27.1299992 + F_L_Y)) * 30.8946857, 1.13705599);
  return sign(J) * J_ts;
}
float aces2_toe_fwd0(float x, float limit, float k1_in, float k2_in)
{
  float k2 = max(k2_in, 0.001);
  float k1 = sqrt(k1_in * k1_in + k2 * k2);
  float k3 = (limit + k1) / (limit + k2);
  return (x > limit) ? x : 0.5 * (k3 * x - k1 + sqrt((k3 * x - k1) * (k3 * x - k1) + 4.0 * k2 * k3 * x));
}
vec3 aces2_gamut_cusp_table_0_sample(float h)
{
  int i = int(h) + 1;
  int i_lo = int(max(float(0), float(i + 0)));
  int i_hi = int(min(float(361), float(i + 2)));
  while (i_lo + 1 < i_hi)
  {
    float hcur = Aces2CuspHue(i);
    if (h > hcur)
    {
      i_lo = i;
    }
    else
    {
      i_hi = i;
    }
    i = (i_lo + i_hi) / 2;
  }
  vec3 lo = Aces2GamutCusp(i_hi - 1);
  vec3 hi = Aces2GamutCusp(i_hi);
  float t = (h - Aces2CuspHue(i_hi - 1)) / (Aces2CuspHue(i_hi) - Aces2CuspHue(i_hi - 1));
  return mix(lo, hi, t);
}
float aces2_get_focus_gain0(float J, float cuspJ)
{
  float thr = mix(cuspJ, aces2_p.limitJMax, 0.300000);
  if (J > thr)
  {
    float gain = ( aces2_p.limitJMax - thr) / max(0.0001, aces2_p.limitJMax - J);
    gain = log(gain)/log(10.0);
    return gain * gain + 1.0;
  }
  else
  {
    return 1.0;
  }
}
float aces2_solve_J_intersect0(float J, float M, float focusJ, float slope_gain)
{
  float M_scaled = M / slope_gain;
  float a = M_scaled / focusJ;
  if (J < focusJ)
  {
    float b = 1.0 - M_scaled;
    float c = -J;
    float det =  b * b - 4.f * a * c;
    float root =  sqrt(det);
    return -2.0 * c / (b + root);
  }
  else
  {
    float b = - (1.0 + M_scaled + aces2_p.limitJMax * a);
    float c = aces2_p.limitJMax * M_scaled + J;
    float det =  b * b - 4.f * a * c;
    float root =  sqrt(det);
    return -2.0 * c / (b - root);
  }
}
float aces2_find_gamut_boundary_intersection0(vec2 JM_cusp, float gamma_top_inv, float gamma_bottom_inv, float J_intersect_source, float J_intersect_cusp, float slope)
{
  float M_boundary_lower = J_intersect_cusp * pow(J_intersect_source / J_intersect_cusp, gamma_bottom_inv) / (JM_cusp.r / JM_cusp.g - slope);
  float M_boundary_upper = JM_cusp.g * (aces2_p.limitJMax - J_intersect_cusp) * pow((aces2_p.limitJMax - J_intersect_source) / (aces2_p.limitJMax - J_intersect_cusp), gamma_top_inv) / (slope * JM_cusp.g + aces2_p.limitJMax - JM_cusp.r);
  float smin = 0.0;
  {
    float a = M_boundary_lower;
    float b = M_boundary_upper;
    float s = 0.119999997 * JM_cusp.g;
    float h = max(s - abs(a - b), 0.0) / s;
    smin = min(a, b) - h * h * h * s * 0.16666666666666666;
  }
  return smin;
}
float aces2_remap_M_fwd0(float M, float gamut_boundary_M, float reach_boundary_M)
{
  float boundary_ratio = gamut_boundary_M / reach_boundary_M;
  float proportion = max(boundary_ratio, 0.75);
  float threshold = proportion * gamut_boundary_M;
  if (proportion >= 1.0f || M <= threshold)
  {
    return M;
  }
  float m_offset = M - threshold;
  float gamut_offset = gamut_boundary_M - threshold;
  float reach_offset = reach_boundary_M - threshold;
  float scale = reach_offset / ((reach_offset / gamut_offset) - 1.0f);
  float nd = m_offset / scale;
  return threshold + scale * nd / (1.0f + nd);
}
vec3 aces2_gamut_compress0(vec3 JMh, float Jx, vec3 JMGcusp, float reachMaxM)
{
  float J = JMh.r;
  float M = JMh.g;
  float h = JMh.b;
  if (M <= 0.0 || J > aces2_p.limitJMax)
  {
    return vec3(J, 0.0, h);
  }
  else
  {
    vec2 JMcusp = JMGcusp.rg;
    float focusJ = mix(JMcusp.r, aces2_p.midJ, min(1.0, 1.300000 - (JMcusp.r / aces2_p.limitJMax)));
    float slope_gain = aces2_p.focusGainBase * aces2_get_focus_gain0(Jx, JMcusp.r);
    float J_intersect_source = aces2_solve_J_intersect0(JMh.r, JMh.g, focusJ, slope_gain);
    float gamut_slope = (J_intersect_source < focusJ) ? J_intersect_source : (aces2_p.limitJMax - J_intersect_source);
    gamut_slope = gamut_slope * (J_intersect_source - focusJ) / (focusJ * slope_gain);
    float gamma_top_inv = JMGcusp.b;
    float gamma_bottom_inv = aces2_p.gammaBottomInv;
    float J_intersect_cusp = aces2_solve_J_intersect0(JMcusp.r, JMcusp.g, focusJ, slope_gain);
    float gamutBoundaryM = aces2_find_gamut_boundary_intersection0(JMcusp, gamma_top_inv, gamma_bottom_inv, J_intersect_source, J_intersect_cusp, gamut_slope);
    if (gamutBoundaryM <= 0.0)
    {
      return vec3(J, 0.0, h);
    }
    float reachBoundaryM = aces2_p.limitJMax * pow(J_intersect_source / aces2_p.limitJMax,  0.879464149);
    reachBoundaryM = reachBoundaryM / ((aces2_p.limitJMax / reachMaxM) - gamut_slope);
    float remapped_M = aces2_remap_M_fwd0(M, gamutBoundaryM, reachBoundaryM);
    float remapped_J = J_intersect_source + remapped_M * gamut_slope;
    return vec3(remapped_J, remapped_M, h);
  }
}

// Declaration of the OCIO shader function

vec4 TonemapACES2Generated(vec4 inPixel)
{
  vec4 outColor = inPixel;

  // Add Matrix processing

  {
    vec4 res = vec4(outColor.rgb.r, outColor.rgb.g, outColor.rgb.b, outColor.a);
    vec4 tmp = res;
    res = mat4(0.6130974024011876, 0.070193722469582095, 0.020615592882226936, 0., 0.33952314618410551, 0.91635387905734422, 0.10956977293813543, 0., 0.047379451414706564, 0.013452398473074167, 0.86981463417963756, 0., 0., 0., 0., 1.) * tmp;
    outColor.rgb = vec3(res.x, res.y, res.z);
    outColor.a = res.w;
  }

  // Add Range processing

  {
    outColor.rgb = max(vec3(0., 0., 0.), outColor.rgb);
    outColor.rgb = min(vec3(aces2_p.inputClampAP1), outColor.rgb);
  }

  // Add Matrix processing

  {
    vec4 res = vec4(outColor.rgb.r, outColor.rgb.g, outColor.rgb.b, outColor.a);
    vec4 tmp = res;
    res = mat4(0.69545224135745176, 0.044794563372037632, -0.0055258825581135443, 0., 0.14067869647029416, 0.85967111845642163, 0.0040252103059786586, 0., 0.16386906217225403, 0.095534318171540358, 1.0015006722521349, 0., 0., 0., 0., 1.) * tmp;
    outColor.rgb = vec3(res.x, res.y, res.z);
    outColor.a = res.w;
  }

  // Add FixedFunction 'ACES_OutputTransform20 (Forward)' processing

  {

    // Add RGB to JMh

    vec3 JMh;
    vec3 Aab;
    {
      {
        vec3 lms = mat3(0.445181042, 0.123734146, 0.0117007261, 0.34964928, 0.613643706, 0.0280607939, -0.00112973212, 0.0563228019, 0.753939033) * outColor.rgb;
        vec3 F_L_v = pow(abs(lms), vec3(0.419999987, 0.419999987, 0.419999987));
        vec3 rgb_a = (sign(lms) * F_L_v) / ( 27.1299992 + F_L_v);
        Aab = mat3(20.25881, 15480., 1720., 10.129405, -16887.2734, 1720., 0.506470263, 1407.27271, -3440.) * rgb_a.rgb;
      }
      {
        if (Aab.r <= 0.0)
        {
          JMh.rgb = vec3(0., 0., 0.);
        }
        else
        {
          float J = 100. * pow(Aab.r, 1.13705599);
          float M = (J == 0.0) ? 0.0 : sqrt(Aab.g * Aab.g + Aab.b * Aab.b);
          float h = (Aab.g == 0.0) ? 0.0 : atan(Aab.b, Aab.g) * 57.29577951308238;
          h = h - floor(h / 360.0) * 360.0;
          h = (h < 0.0) ? h + 360.0 : h;
          JMh.rgb = vec3(J, M, h);
        }
      }
      outColor.rgb = JMh;
    }
    float h_rad = outColor.b * 0.0174532924;
    float cos_hr = cos(h_rad);
    float sin_hr = sin(h_rad);

    // Add ToneScale and ChromaCompress (fwd)

    float J_ts = aces2_tonescale_fwd0(outColor.r);
    // Sample tables (fwd)
    float reachMaxM = aces2_reach_m_table_0_sample(outColor.b);

    {
      float J = outColor.r;
      float M = outColor.g;
      float h = outColor.b;
      float M_cp = M;
      if (M != 0.0)
      {
        float nJ = J_ts / aces2_p.limitJMax;
        float snJ = max(0.0, 1.0 - nJ);
        float Mnorm;
        {
          float cos_hr2 = 2.0 * cos_hr * cos_hr - 1.0;
          float sin_hr2 = 2.0 * cos_hr * sin_hr;
          float cos_hr3 = 4.0 * cos_hr * cos_hr * cos_hr - 3.0 * cos_hr;
          float sin_hr3 = 3.0 * sin_hr - 4.0 * sin_hr * sin_hr * sin_hr;
          vec3 cosines = vec3(cos_hr, cos_hr2, cos_hr3);
          vec3 cosine_weights = aces2_p.mnormCosW;
          vec3 sines = vec3(sin_hr, sin_hr2, sin_hr3);
          vec3 sine_weights = aces2_p.mnormSinW;
          Mnorm = dot(cosines, cosine_weights) + dot(sines, sine_weights) + aces2_p.mnormOffset;
        }
        float limit = pow(nJ, 0.879464149) * reachMaxM / Mnorm;
        M_cp = M * pow(J_ts / J, 0.879464149);
        M_cp = M_cp / Mnorm;
        M_cp = limit - aces2_toe_fwd0(limit - M_cp, limit - 0.001, snJ * aces2_p.toeLowScale, sqrt(nJ * nJ + aces2_p.toeK2Bias));
        M_cp = aces2_toe_fwd0(M_cp, limit, nJ * aces2_p.toeHighScale, snJ);
        M_cp = M_cp * Mnorm;
      }
      outColor.rgb = vec3(J_ts, M_cp, h);
    }

    // Add GamutCompress (fwd)

    {
      vec3 JMGcusp = aces2_gamut_cusp_table_0_sample(outColor.b);
      outColor.rgb = aces2_gamut_compress0(outColor.rgb, outColor.r, JMGcusp, reachMaxM);
    }

    // Add JMh to RGB

    {
      vec3 JMh = outColor.rgb;
      vec3 Aab;
      {
        Aab.r = pow(JMh.r * 0.00999999978, 0.879464149);
        Aab.g = JMh.g * cos_hr;
        Aab.b = JMh.g * sin_hr;
      }
      {
        vec3 rgb_a = mat3(0.0323680267, 0.0323680267, 0.0323680267, 2.07657631e-05, -4.10250432e-05, -1.01296409e-05, 1.3260621e-05, -1.20174373e-05, -0.000290076074) * Aab.rgb;
        vec3 rgb_a_lim = min( abs(rgb_a), vec3(0.99000001, 0.99000001, 0.99000001) );
        vec3 lms = sign(rgb_a) * pow( 27.1299992 * rgb_a_lim / (1.0f - rgb_a_lim), vec3(2.38095236, 2.38095236, 2.38095236));
        JMh.rgb = mat3(7.45048571, -1.4750675, 0.0106288502, -6.1301837, 3.11835742, -0.31857267, -0.0603808537, -0.383369029, 1.56786489) * lms;
      }
      outColor.rgb = JMh;
    }
  }

  // Add Range processing

  {
    outColor.rgb = max(vec3(0., 0., 0.), outColor.rgb);
    outColor.rgb = min(vec3(aces2_p.outClamp), outColor.rgb);
  }

  return outColor;
}

// outputMax = display peak / paper white (HdrOutputMaxLinearValue() in
// tonemap.frag; 1.0 under SDR).
vec3 TonemapACES2(vec3 sceneLinearRec709, float outputMax)
{
    Aces2SelectTier(outputMax);
    return TonemapACES2Generated(vec4(max(sceneLinearRec709, vec3(0.0)), 1.0)).rgb;
}

#endif
