// Minimal StandardPBR lighting model include (M0).
// Goal: one directional light + Cook-Torrance baseline (no IBL, no shadows).

#ifndef GE_LIGHTING_STANDARD_PBR_GLSL
#define GE_LIGHTING_STANDARD_PBR_GLSL

#include "surface_io.glsl" // SurfaceOutput — the shading inputs the BRDF reads

const float GE_PI = 3.14159265359;
const float GE_DIELECTRIC_F0 = 0.04;   // ~4% reflectance (IOR 1.5) — every dielectric interface: base + clear coat
const float GE_MIN_GGX_ALPHA = 0.0025; // alpha floor so a near-mirror lobe stays well-defined
const float GE_MIN_SHEEN_ROUGHNESS = 0.07; // D_Charlie pow(sin,1/a) blows up as a->0; NOT the GGX alpha floor
const float GE_SSS_DISTORTION = 0.2;   // subsurface back-light dir bent by N (DICE/Frostbite); 0.1-0.5
const float GE_SSS_POWER      = 4.0;   // transmission falloff sharpness; 1-12
const float GE_SSS_SCALE      = 1.0;   // overall transmission strength
const float GE_SSS_AMBIENT    = 0.04;  // small uniform translucent wrap floor (DICE subtle range); ungated, so keep it low

// Scalar dielectric F0 from index of refraction (untinted, e.g. the clear coat).
// Neutral ior 1.5 -> 0.04, reproducing the legacy GE_DIELECTRIC_F0: ((0.5/2.5)^2)=0.04.
float GE_DielectricF0Scalar(float ior)
{
    float i = max(ior, 1.0); // IOR < 1 is non-physical for a dielectric; avoid the F0 inversion
    float r = (i - 1.0) / (i + 1.0);
    return r * r;
}

// OpenPBR dielectric F0 from index of refraction, tinted + weighted (the base layer).
// Neutral (weight 1, white, ior 1.5) reproduces GE_DIELECTRIC_F0 exactly.
vec3 GE_DielectricF0(float specularWeight, vec3 specularColor, float specularIor)
{
    return specularWeight * specularColor * GE_DielectricF0Scalar(specularIor);
}

// Internal (dense->rare) hemispherical reflectance of the coat/base interface: the
// per-bounce probability that light leaving the base is reflected BACK DOWN into the
// absorbing base, which drives the multi-internal-reflection coat-darkening series.
// ~0.08 at ior 1.0 (a fit residual at the no-contrast limit, harmless — gated by
// coatWeight*coatDarkening), ~0.61 at the default 1.5 coat, ~0.80 at 2.0.
float GE_CoatInternalReflectance(float ior)
{
    float i = max(ior, 1.0);
    float r = (i - 1.0) / (i + 1.0);
    return 1.0 - (1.0 - r * r) / (i * i) * 0.9181; // fixed internal-reflectance fit coefficient (calibrated at the 1.5 coat)
}

// Coat darkening ("wet look"): light entering a clear coat is partly trapped by internal
// reflection at the coat/base interface and makes extra absorbing passes through the base,
// so the base reads deeper and more saturated. Per-channel trap compounding base'/base =
// (1 - Ri) / (1 - rho*Ri); low-rho (saturated/dark) channels lose more per trapped pass,
// so colour saturates as well as darkens. coatWeight gates presence, coatDarkening (0..1)
// is the artist dial; the result is <= 1 per channel, so it only ever removes energy.
vec3 GE_CoatDarkening(vec3 rho, float coatIor, float coatWeight, float coatDarkening)
{
    float Ri      = GE_CoatInternalReflectance(coatIor);
    vec3  trapped = (1.0 - Ri) / max(1.0 - rho * Ri, vec3(1e-4));
    return mix(vec3(1.0), trapped, coatWeight * coatDarkening);
}

vec3 GE_SchlickFresnel(vec3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// Thin-film iridescence evaluator — needs GE_PI + GE_DielectricF0Scalar (both above).
// Included here so the direct lobe and (via ibl.glsl's include of this file) the IBL
// path share one definition; the call sites are guarded by GE_IRIDESCENCE_ENABLED.
#include "iridescence.glsl"

// Roughness-aware Fresnel (Lagarde) for the IBL diffuse/specular energy split.
// Ramps toward max(1-roughness, F0) at grazing so smooth surfaces don't
// over-brighten. Used only for kD; the specular split-sum reconstructs with F0.
vec3 GE_SchlickFresnelRoughness(vec3 F0, float cosTheta, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// Kulla-Conty F82-tint Fresnel (the OpenPBR / Adobe-Standard-Material metal model). Plain
// Schlick drives every metal's grazing rim to pure white; real metals (gold, copper) keep a
// tinted edge. This adds a single tinted dip at the ~82° peak-deviation angle (cosθ = 1/7):
// F(1/7) = Schlick(1/7) * edgeTint. edgeTint == 1 makes `a` vanish, so it reduces to Schlick
// EXACTLY — a true no-op for untinted metals and all dielectrics.
vec3 GE_FresnelF82(vec3 F0, vec3 edgeTint, float cosTheta)
{
    const float kCos82 = 1.0 / 7.0;                                  // cos(~81.8°), the peak deviation (acos(1/7))
    const float kInvB  = 1.0 / (kCos82 * pow(1.0 - kCos82, 6.0));    // 1 / B(1/7), B(μ)=μ(1-μ)^6
    vec3 schlick   = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
    vec3 schlick82 = F0 + (1.0 - F0) * pow(1.0 - kCos82, 5.0);       // Schlick evaluated at 82°
    vec3 a         = schlick82 * (1.0 - edgeTint) * kInvB;           // per-channel dip magnitude
    return max(schlick - a * (cosTheta * pow(1.0 - cosTheta, 6.0)), 0.0);
}

float GE_D_GGX(float NdotH, float a)
{
    a = max(a, GE_MIN_GGX_ALPHA); // keep a near-mirror lobe well-defined (base + coat callers pass raw alpha)
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (GE_PI * d * d);
}

// Height-correlated isotropic Smith visibility (Heitz 2014). Returns V = G/(4 NdotV NdotL),
// so callers use spec = D*Vis*F with no separate denominator. This is the isotropic form of
// GE_V_SmithGGX_Aniso (identical at at==ab==alpha), so the base lobe and the anisotropy path
// share one masking-shadowing model and toggling anisotropy at slider 0 is a true no-op.
float GE_V_SmithGGX(float NdotV, float NdotL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Anisotropic GGX NDF (Filament D_GGX_Anisotropic, numerically-stable vec3 form).
// at/ab are the per-axis alphas and must be pre-floored by the caller. Reduces to
// GE_D_GGX when at == ab.
float GE_D_GGX_Aniso(float at, float ab, float ToH, float BoH, float NdotH)
{
    float a2 = at * ab;
    vec3  d  = vec3(ab * ToH, at * BoH, a2 * NdotH);
    float d2 = dot(d, d);
    float b2 = a2 / d2;
    return a2 * b2 * b2 * (1.0 / GE_PI);
}

// Height-correlated anisotropic Smith visibility (Heitz 2014). Returns V = G/(4 NdotV NdotL),
// so callers use spec = D*Vis*F with NO separate denominator (mirrors the clear-coat ccV).
float GE_V_SmithGGX_Aniso(float at, float ab, float ToV, float BoV, float ToL, float BoL,
                          float NdotV, float NdotL)
{
    float lambdaV = NdotL * length(vec3(at * ToV, ab * BoV, NdotV));
    float lambdaL = NdotV * length(vec3(at * ToL, ab * BoL, NdotL));
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Isotropic Cook-Torrance specular (the base GGX lobe). Shared by the non-anisotropic
// path and the anisotropy-disabled/untangented fallback so the math lives in one place.
vec3 GE_SpecularIsotropic(float NdotH, float NdotV, float NdotL, float alpha, vec3 F)
{
    float D   = GE_D_GGX(NdotH, alpha);
    float Vis = GE_V_SmithGGX(NdotV, NdotL, alpha);
    return D * Vis * F;
}

// Karis analytic fit of the split-sum DFG (.x=scale, .y=bias). The punctual path has no
// BRDF LUT in scope, so this supplies Ess for multiscatter compensation; the IBL path uses
// the real ge_brdfLUT, and the two agree to within the fit.
vec2 GE_EnvBRDFApprox(float NdotV, float roughness)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Fdez-Aguera 2019 multiscatter energy compensation. Single-scatter GGX integrates to
// Ess = scale+bias < 1 at high roughness, so rough metals/dielectrics darken (the UE
// "plastic" tell). Multiply the single-scatter specular by this to add the lost multi-bounce
// energy back, coloured by F0 (so metals recover their tint). Used by both the direct and IBL paths.
vec3 GE_MultiscatterCompensation(vec3 F0, float Ess)
{
    return 1.0 + F0 * (1.0 / max(Ess, 1e-3) - 1.0);
}

// Per-pixel base multiscatter factor. The compensation depends only on F0/NdotV/roughness
// (per-pixel, NOT per-light), so the lighting accumulator sets this ONCE per fragment via
// GE_ComputeBaseMultiscatter before the light loop, and GE_EvaluateStandardPBR reads it —
// instead of recomputing the DFG fit (incl. an exp2) per light and per area-light sample.
// Defaults to identity so a caller that forgets to set it simply gets no compensation.
vec3 g_BaseMultiscatter = vec3(1.0);

vec3 GE_ComputeBaseMultiscatter(vec3 F0, float NdotV, float roughness)
{
    vec2 dfg = GE_EnvBRDFApprox(NdotV, roughness);
    return GE_MultiscatterCompensation(F0, dfg.x + dfg.y);
}

// Subsurface back-transmission (DICE/Frostbite, GDC 2011), returned SEPARATELY from the
// front-lit BRDF so the caller adds it OUTSIDE the shadow multiply: a thin object lit from
// behind must glow through even where its own front face is in the light's shadow. It is
// NdotL-INDEPENDENT (peaks when L points into the back face). vec3(0) when disabled.
vec3 GE_SubsurfaceTransmission(SurfaceOutput so, vec3 V, vec3 L)
{
#ifdef GE_SUBSURFACE_ENABLED
    vec3  transLightDir = L + so.normalWS * GE_SSS_DISTORTION;
    float tBack = pow(clamp(dot(V, -normalize(transLightDir)), 0.0, 1.0), GE_SSS_POWER) * GE_SSS_SCALE;
    float transAmt = (tBack + GE_SSS_AMBIENT) * clamp(so.thickness, 0.0, 1.0);
    return so.subsurfaceColor * transAmt;
#else
    return vec3(0.0);
#endif
}

// Estevez-Kulla 2017 "Production Friendly Microfacet Sheen BRDF" shadowing-masking — the
// visibility term the glTF KHR_materials_sheen reference uses. The cheap Ashikhmin form it
// replaces ignores the soft velvet terminator; this fitted lambda reproduces it. `alpha` is
// the PERCEPTUAL sheen roughness (not squared), matching the Charlie D above.
float GE_CharlieL(float x, float alpha)
{
    float oneMinusAlpha2 = (1.0 - alpha) * (1.0 - alpha);
    float a = mix(21.5473, 25.3245, oneMinusAlpha2);
    float b = mix(3.82987, 3.32435, oneMinusAlpha2);
    float c = mix(0.19823, 0.16801, oneMinusAlpha2);
    float d = mix(-1.97760, -1.27393, oneMinusAlpha2);
    float e = mix(-4.32054, -4.85967, oneMinusAlpha2);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float GE_CharlieLambda(float cosTheta, float alpha)
{
    return abs(cosTheta) < 0.5
        ? exp(GE_CharlieL(cosTheta, alpha))
        : exp(2.0 * GE_CharlieL(0.5, alpha) - GE_CharlieL(1.0 - cosTheta, alpha));
}

// V = G / (4 NdotV NdotL): the Charlie lobe is D*V*F with no extra denominator, mirroring the
// base/coat lobes.
float GE_VisCharlie(float NdotV, float NdotL, float alpha)
{
    float G = 1.0 / (1.0 + GE_CharlieLambda(NdotV, alpha) + GE_CharlieLambda(NdotL, alpha));
    return G / max(4.0 * NdotV * NdotL, 1e-4);
}

// Charlie (Estevez-Kulla) sheen/fuzz reflected radiance for one light direction, BEFORE the
// shared NdotL. roughness is PERCEPTUAL (floored at GE_MIN_SHEEN_ROUGHNESS by the caller). Shared
// by the under-coat Sheen lobe and the over-coat Fuzz lobe so the two read from one NDF/visibility.
vec3 GE_CharlieLobe(vec3 tint, float roughness, float NdotH, float NdotV, float NdotL)
{
    float invA = 1.0 / roughness;
    float sin2 = max(1.0 - NdotH * NdotH, 0.0);
    float D    = (2.0 + invA) * pow(sin2, invA * 0.5) / (2.0 * GE_PI);
    float V    = GE_VisCharlie(NdotV, NdotL, roughness);
    return tint * (D * V);
}

// Charlie directional albedo E(NdotV, roughness): the fraction of a UNIFORM environment the
// Charlie lobe reflects toward the viewer. It is what the ambient (IBL) sheen/fuzz terms must
// weight the irradiance by, the way the split-sum LUT weights the GGX lobe. Closed-form fit to a
// numeric hemisphere integration of the same D_Charlie * V_Charlie the punctual lobe uses, so the
// direct and ambient sheen cannot drift. Never negative, and it decays smoothly to a small face-on
// value rather than to zero, which is the property a grazing-only ramp gets wrong. Accuracy against
// that integral, measured over NdotV 0.02..1: -8%/+58% at roughness 0.2, -13%/+27% at 0.3,
// -25%/+20% at 0.45, -45%/+9% at 0.9 — a single two-constant exponential cannot hold the whole
// fabric range tighter than that, and the residual is a smooth bias, not a shape error. Tightening
// it means a fitted 2D LUT or a third constant; the win here is the 64x under-read at NdotV 0.9
// that the pow(1 - NdotV, 3) ramp it replaces produced, not the last 20%.
// roughness is PERCEPTUAL; floored internally at GE_MIN_SHEEN_ROUGHNESS, so callers pass it raw.
const float GE_SHEEN_ALBEDO_SCALE = 1.02; // E at NdotV = 0 (silhouette)
const float GE_SHEEN_ALBEDO_K0    = 2.15; // roughness-independent decay
const float GE_SHEEN_ALBEDO_K1    = 0.31; // 1/roughness decay: smoother fuzz collapses faster off-grazing
float GE_CharlieDirectionalAlbedo(float NdotV, float roughness)
{
    float rough = max(roughness, GE_MIN_SHEEN_ROUGHNESS);
    return GE_SHEEN_ALBEDO_SCALE * exp(-(GE_SHEEN_ALBEDO_K0 + GE_SHEEN_ALBEDO_K1 / rough) * NdotV);
}

// Layer the OpenPBR fuzz lobe over an already-shaded stack (`beneath` = base [+coat], pre-NdotL).
// Fuzz is the OUTERMOST layer, so it reflects `fuzzSpec` and the whole stack below it transmits
// (1 - reflectance) of the remaining light — the same energy split the clear-coat applies to the
// base with (1 - Fc), just one layer further out. reflectance is the scalar max channel of the
// fuzz reflectance clamped to [0,1], so the layer can never add net energy or drive `beneath`
// negative; fuzzColor -> 0 makes both terms vanish, leaving `beneath` byte-exact.
vec3 GE_LayerFuzzOverCoat(vec3 beneath, vec3 fuzzColor, float fuzzRoughness,
                          float NdotH, float NdotV, float NdotL)
{
    float rough    = max(fuzzRoughness, GE_MIN_SHEEN_ROUGHNESS);
    vec3  fuzzSpec = GE_CharlieLobe(fuzzColor, rough, NdotH, NdotV, NdotL);
    float refl     = clamp(max(max(fuzzSpec.r, fuzzSpec.g), fuzzSpec.b), 0.0, 1.0);
    return beneath * (1.0 - refl) + fuzzSpec;
}

// Evaluate StandardPBR for a single light. The surface properties travel in `so`
// (baseColor / metallic / roughness / normalWS); V is the view dir and L the
// surface->light dir, both world-space. Passing the whole SurfaceOutput keeps the
// signature stable as later lighting models read more of `so` (clear coat, sheen).
vec3 GE_EvaluateStandardPBR(SurfaceOutput so, vec3 V, vec3 L)
{
    vec3 N = so.normalWS;
    vec3 baseColor = so.baseColor;
    float metallic = so.metallic;
    float roughness = so.roughness;

    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float LdotH = max(dot(L, H), 0.0);

    vec3 F0 = mix(GE_DielectricF0(so.specularWeight, so.specularColor, so.specularIor), baseColor, metallic);
    // F82 metal edge-tint: specularColor tints a metal's ~82° rim so it keeps its hue instead
    // of washing to white; dielectrics keep pure Schlick (edgeTint = 1, a true no-op). A metal
    // with the default white specularColor also stays Schlick-exact, so this never regresses.
    vec3 edgeTint = mix(vec3(1.0), so.specularColor, metallic);
    vec3  F = GE_FresnelF82(F0, edgeTint, VdotH);
#ifdef GE_IRIDESCENCE_ENABLED
    // Thin-film interference replaces the specular Fresnel with the wavelength-dependent
    // Airy reflectance of the air/film/base stack; thinFilmWeight lerps back to F82 so
    // weight 0 (the default) is a true no-op. VdotH is the microfacet-interface cosine.
    F = mix(F, GE_Iridescence(VdotH, so.thinFilmIor, so.thinFilmThickness, F0), so.thinFilmWeight);
#endif
    // GGX alpha = roughness^2 (standard remap); GE_D_GGX squares it again -> roughness^4.
    // Matches the IBL prefilter/BRDF-LUT convention so direct + IBL highlights agree.
    float alpha = roughness * roughness;
#ifdef GE_ANISOTROPY_ENABLED
    // Anisotropy RESHAPES the base lobe (unlike the additive sheen/coat below): split
    // alpha into per-axis at/ab along the surface tangent/bitangent. Floor AFTER the
    // split — at |aniso|->1 one axis hits 0 and the NDF denominator would divide by 0.
    // anisotropy==0 or a zero/degenerate tangent frame degrades to the isotropic path
    // (so enabling the keyword at slider 0 is a true no-op, matching the IBL gate).
    vec3 spec;
    vec3 T = so.tangentWS;
    vec3 B = so.bitangentWS;
    if (so.anisotropy != 0.0 && dot(T, T) > 1e-4)
    {
        // Revolve the (T,B) frame within the surface plane so the groove follows an authored
        // angle instead of the raw geometric tangent (brushed metal that ignores the UV grain).
        // Guarded so rotation 0 leaves T,B untouched -> byte-identical to the unrotated path, and
        // matched by the IBL path so the punctual highlight and the env reflection stay aligned.
        if (so.anisotropyRotation != 0.0)
        {
            float ca = cos(so.anisotropyRotation);
            float sa = sin(so.anisotropyRotation);
            vec3 rT =  ca * T + sa * B;
            vec3 rB = -sa * T + ca * B;
            T = rT;
            B = rB;
        }
        float at  = max(alpha * (1.0 + so.anisotropy), GE_MIN_GGX_ALPHA);
        float ab  = max(alpha * (1.0 - so.anisotropy), GE_MIN_GGX_ALPHA);
        float ToH = dot(T, H), BoH = dot(B, H);
        float ToV = dot(T, V), BoV = dot(B, V);
        float ToL = dot(T, L), BoL = dot(B, L);
        float D   = GE_D_GGX_Aniso(at, ab, ToH, BoH, NdotH);
        float Vis = GE_V_SmithGGX_Aniso(at, ab, ToV, BoV, ToL, BoL, NdotV, NdotL);
        spec = D * Vis * F;
    }
    else
    {
        spec = GE_SpecularIsotropic(NdotH, NdotV, NdotL, alpha, F);
    }
#else
    vec3 spec = GE_SpecularIsotropic(NdotH, NdotV, NdotL, alpha, F);
#endif
    // Multiscatter compensation (Fdez-Aguera) — hoisted to per-pixel via g_BaseMultiscatter,
    // set once by the lighting accumulator before the light loop (see GE_ComputeBaseMultiscatter).
    spec *= g_BaseMultiscatter;

    vec3 kd = (1.0 - F) * (1.0 - metallic);
    // Oren-Nayar diffuse (Fujii's energy-conserving "improved qualitative" form) — OpenPBR's
    // named rough-diffuse model, replacing the Disney/Burley lobe. Microfacet shadowing flattens
    // the response and adds azimuthal retroreflection at grazing, so rough surfaces read as real
    // matte (chalk, clay, unfinished wood) instead of soft Lambert plastic. onA/onB fold in the
    // 1/pi; the NdotL cosine is applied with the rest of the lobes at the end. sigma == 0 ->
    // onB == 0 -> exact Lambert. Driven by the DECOUPLED diffuse roughness (OpenPBR's separate
    // diffuse control, default 0 = neutral Lambert); rough-diffuse surfaces (clay, chalk, skin)
    // dial it up independently of the specular roughness.
    float onSigma = clamp(so.diffuseRoughness, 0.0, 1.0); // keep the Fujii fit in-domain (sigma>1 over-brightens)
    float onDenom = GE_PI + (GE_PI * 0.5 - 2.0 / 3.0) * onSigma;
    float onA     = 1.0 / onDenom;
    float onB     = onSigma / onDenom;
    float onS     = dot(L, V) - NdotL * NdotV;
    float onST    = onS > 0.0 ? onS / max(max(NdotL, NdotV), 1e-4) : onS;
    vec3 diffuse  = kd * baseColor * (onA + onB * onST);
#ifdef GE_TRANSMISSION_ENABLED
    // Transmission takes its energy out of the DIFFUSE budget: light that refracts through the
    // surface leaves on the far side instead of scattering back to the viewer. ibl.glsl applies
    // the same (1 - weight) to the ambient diffuse (`diffuseScale`), and this is the analytic
    // half of that split — without it a weight-1 glass still shades a full opaque Lambert lobe
    // from every punctual light, which under a bright key buries the refracted background (and
    // its Beer-Lambert tint) by orders of magnitude. Gated on the keyword so a material without
    // the lobe is byte-identical.
    diffuse *= 1.0 - clamp(so.transmissionWeight, 0.0, 1.0);
#endif

    // --- Optional additive fabric sheen (Charlie/Estevez-Kulla): a retroreflective fuzz
    //     lobe peaking at grazing (velvet rim-glow), opposite to the GGX peak. Added to
    //     the base BEFORE any clear-coat attenuation so the coat sits over the sheened base.
    vec3 baseSpec = diffuse + spec;
#ifdef GE_SHEEN_ENABLED
    // Shares GE_CharlieLobe with the over-coat fuzz lobe so both read one NDF/visibility.
    baseSpec += GE_CharlieLobe(so.sheenColor, max(so.sheenRoughness, GE_MIN_SHEEN_ROUGHNESS), NdotH, NdotV, NdotL);
#endif

    // Subsurface back-transmission is intentionally NOT added here — it is returned by
    // GE_SubsurfaceTransmission() and summed by each call site OUTSIDE the shadow multiply,
    // so a back-lit thin object glows through its own front-face shadow.
#ifdef GE_CLEARCOAT_ENABLED
    // Additive clear-coat: a thin dielectric GGX lobe over the base (F0 from the coat
    // IOR). The base layer transmits (1 - Fc) of incoming light; the coat reflects the
    // rest. Kelemen visibility already folds in the 1/(4 NdotV NdotL) term, so the coat
    // lobe is D*V*F (no extra denominator). One NdotL applied to the whole result.
    // The coat shades about its OWN normal under GE_COAT_NORMAL_ENABLED (coat-only normal
    // map) so a textured lacquer microstructure reads on the coat without disturbing the
    // base lobes; #else reuses the base cosines verbatim. The final NdotL stays the base's
    // (the coat is one thin layer over the same lit surface; the base normal owns the cosine).
#ifdef GE_COAT_NORMAL_ENABLED
    // The half-vector H and VdotH are normal-independent, so the only cosine the coat normal
    // moves is the NDF's NdotH; Fresnel (VdotH) and Kelemen visibility (VdotH) are unchanged.
    float ccNdotH = max(dot(so.coatNormalWS, H), 0.0);
#else
    float ccNdotH = NdotH;
#endif
    float Fc      = GE_SchlickFresnel(vec3(GE_DielectricF0Scalar(so.clearCoatIor)), VdotH).x * so.clearCoat;
    float ccAlpha = so.clearCoatRoughness * so.clearCoatRoughness;
    float ccD     = GE_D_GGX(ccNdotH, ccAlpha);
    float ccV     = 0.25 / max(VdotH * VdotH, 1e-4); // Kelemen-Szirmay-Kalos
    float ccSpec  = ccD * ccV * Fc;
    // Coat darkening multiplies only the through-coat base (inside the (1-Fc) group); the
    // coat's own reflection ccSpec is never darkened, so no energy is added or double-counted.
    // coatColor tints the same through-coat group (absorption by the coat medium); white = no tint.
    vec3 coatDarken = GE_CoatDarkening(so.baseColor, so.clearCoatIor, so.clearCoat, so.coatDarkening);
#ifdef GE_FUZZ_ENABLED
    // Fuzz is the outermost layer: shade the coated stack first, then lay the fuzz over the
    // WHOLE thing (base + coat) so a coated fabric still shows its over-coat fuzz rim. Distinct
    // from sheen, which is folded into baseSpec ABOVE and therefore sits UNDER the coat.
    vec3 coated = baseSpec * coatDarken * so.coatColor * (1.0 - Fc) + ccSpec;
    return GE_LayerFuzzOverCoat(coated, so.fuzzColor, so.fuzzRoughness, NdotH, NdotV, NdotL) * NdotL;
#else
    return (baseSpec * coatDarken * so.coatColor * (1.0 - Fc) + ccSpec) * NdotL;
#endif
#else
  #ifdef GE_FUZZ_ENABLED
    // No coat: fuzz lays directly over the base (which already carries any under-coat sheen).
    return GE_LayerFuzzOverCoat(baseSpec, so.fuzzColor, so.fuzzRoughness, NdotH, NdotV, NdotL) * NdotL;
  #else
    return baseSpec * NdotL;
  #endif
#endif
}

#endif // GE_LIGHTING_STANDARD_PBR_GLSL
