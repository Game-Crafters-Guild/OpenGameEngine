// Procedural caustic web for the glass focused-light dapple (Phase 2 of the glass
// translucent-shadow feature). Self-contained — no texture, no descriptor binding, no
// ocean dependency. The web is anchored to the plane perpendicular to the light (so it
// registers with the glass that casts it and doesn't collapse on vertical receivers), and
// static (glass doesn't flow like water).
//
// THE DAPPLE IS CURRENTLY OFF (kGlassCausticStrength = 0). Only the additive focused web is
// disabled: the translucent-shadow tint is the RGB of the same transmittance sample, reaches the
// receiver on its own path (ge_lastShadowTint), and is untouched here. Note what that tint does
// and does not carry — it is transmissionColor x transmissionWeight, never the Beer-Lambert
// volume colour, so a glass tinted only by attenuationColor casts a colourless shadow (see the
// GE_GLASS_SHADOW_COLOR write in adapter_forward.glsl). The focus term is the dapple's only
// geometric bound and it does not bound amplitude, which is why the web is off rather than scaled.

#ifndef GE_CAUSTICS_GLSL
#define GE_CAUSTICS_GLSL

#include "compat_profile.glsl"

// Glass-caustic tuning: world-space cell density of the web + brightness of the focused
// dapple. Scale 2.5 -> ~0.4 m caustic cells (a few across a typical glass footprint).
const float kGlassCausticScale = 2.5;
// Peak brightness of a filament, as a MULTIPLE OF THE RECEIVER'S OWN DIFFUSE RESPONSE to the
// same light (see GE_GlassCaustic) — not a radiance scale.
//
// Held at 0: the dapple has exactly one geometric bound, GE_GlassFocus, and that measurement
// cannot bound it. Across a glass sphere it saturates at kGlassCausticFocusMax over the lens's
// whole disc, so every texel of the entire shadow footprint decodes to a full-strength dapple;
// raise the clamp and it collapses instead to a thin aliased ring at the silhouette — the
// derivative singularity of a grazing refraction, not a focal spot. A lens concentrates light
// in the MIDDLE of its shadow, and neither shape does that, so what survives to the screen is
// the procedural web itself, spanning metres of floor and reading as a water-caustic texture.
// Restoring it needs the focus model re-derived around the lens-to-receiver distance along the
// light, which the receiver has no channel for (the RGBA8 transmittance cascade spends RGB on
// the tint and A on the focus). Measurements and probe captures are in the investigation linked
// above; a nonzero value here is what turns the term back on.
const float kGlassCausticStrength = 0.0;
const float kGlassCausticInvPi = 0.3183098862; // 1/pi, folded into the Lambert normalisation

// Web appearance constants (governs the filament shape; retune these for the look):
const float kCausticWarp = 1.8;        // domain-warp magnitude — how much the filaments curve
const float kCausticRidgeWidth = 2.2;  // larger = thinner bright filaments

// C1.5 focusing: convergence of light refracted through the glass surface. The glass-tint
// cascade pass writes this (normalized) into the transmittance alpha; the receiver decodes
// and multiplies the web by it so the dapple brightens where the lens converges light.
const float kGlassCausticFocusMax = 2.5;    // clamp on the convergence multiplier (1 = baseline web; gentle by default)
const float kCausticFocusPlaneDist = 0.6;   // virtual focal-plane distance — kept short (< a glass
                                            // sphere's focal length) so convergence registers before
                                            // the refracted rays cross and re-diverge

float GE_CausticHash(vec2 p)
{
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.5);
    return fract(p.x * p.y);
}

// Smooth value noise.
float GE_CausticNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = GE_CausticHash(i);
    float b = GE_CausticHash(i + vec2(1.0, 0.0));
    float c = GE_CausticHash(i + vec2(0.0, 1.0));
    float d = GE_CausticHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Caustic web at a 2D position (in the light-perpendicular plane): two domain-warped noise
// fields with bright thin filaments where they cross (the classic caustic line network).
// Returns >= 0, peaking ~1 on the bright lines and ~0 between (so it ADDS light only on the web).
float GE_CausticWeb(vec2 planePos, float scale)
{
    vec2 p = planePos * scale;
    vec2 warp = vec2(GE_CausticNoise(p + vec2(1.7, 9.2)),
                     GE_CausticNoise(p + vec2(8.3, 2.8)));
    p += (warp - 0.5) * kCausticWarp;
    float n1 = GE_CausticNoise(p);
    float n2 = GE_CausticNoise(p * 1.7 + vec2(4.0, 1.5));
    float ridge = max(1.0 - abs(n1 - n2) * kCausticRidgeWidth, 0.0); // bright where the fields meet
    return ridge * ridge * ridge * ridge;                           // sharpen into thin filaments
}

// Orthonormal 2D basis (t, bt) spanning the plane perpendicular to `dir`, with an up-vector
// fallback when `dir` is near-vertical. Shared by the focus encoder (GE_GlassFocus) and the
// receiver web (GE_GlassCaustic) so both project onto one construction.
void GE_LightPlaneBasis(vec3 dir, out vec3 t, out vec3 bt)
{
    t  = normalize(abs(dir.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), dir) : vec3(1.0, 0.0, 0.0));
    bt = cross(dir, t);
}

// Convergence of light refracted through the glass surface at one glass-tint fragment,
// measured in the light's frame: refract the incident ray through the smooth (interpolated)
// glass normal, land it on a virtual plane kCausticFocusPlaneDist ahead, and compare the
// screen-quad's footprint area there vs where it started. srcArea/landArea > 1 = the lens is
// converging the light here (bright caustic core); < 1 = diverging (dim). Returns ~1 for flat
// glass (parity with the un-focused Phase-2 dapple). Fragment-only (uses derivatives).
float GE_GlassFocus(vec3 posWS, vec3 normalWS, vec3 lightTravelDir, float ior)
{
    if (dot(lightTravelDir, lightTravelDir) < 1e-8)
        return 1.0; // no light direction supplied -> flat (neutral)
    vec3 I = normalize(lightTravelDir);
    vec3 N = normalize(normalWS);
    if (dot(N, I) > 0.0) N = -N;                  // face the incident ray (lit side)
    float eta = 1.0 / max(ior, 1.0001);           // air -> glass
    vec3 R = refract(I, N, eta);
    if (dot(R, R) < 1e-6) R = I;                  // total internal reflection -> passthrough
    // Project the source point and the refracted landing onto the plane perpendicular to the
    // incident ray; the screen-quad area ratio between them is the lens convergence.
    vec3 t, bt;
    GE_LightPlaneBasis(I, t, bt);
    vec3 landing = posWS + R * kCausticFocusPlaneDist;
    vec2 lp = vec2(dot(landing, t), dot(landing, bt));
    vec2 sp = vec2(dot(posWS, t),   dot(posWS, bt));
    float landArea = abs(GE_DFDX(lp).x * GE_DFDY(lp).y - GE_DFDX(lp).y * GE_DFDY(lp).x);
    float srcArea  = abs(GE_DFDX(sp).x * GE_DFDY(sp).y - GE_DFDX(sp).y * GE_DFDY(sp).x);
    // Floor at 1.0: convergence brightens the web; divergence stays at the baseline dapple
    // (never dimmer than the un-focused C2), so C1.5 only ever ADDS a focused core.
    return clamp(srcArea / (landArea + 1e-8), 1.0, kGlassCausticFocusMax);
}

// The full glass-caustic light contribution at a receiver fragment. Projects the world
// position onto the plane perpendicular to the light (so the web registers with the glass
// and doesn't streak on vertical/sloped receivers), gates on glass presence + NdotL (no
// caustic on back-facing or grazing surfaces), tints by the glass colour, and scales by the
// directional light and shadow factor. Additive on top of the Phase 1 tinted base (energy
// not conserved — this is the faked C2).
//
// `diffuseAlbedo` is the receiver's own diffuse reflectance. The dapple is EXTRA IRRADIANCE
// the lens concentrates onto the receiver, so it is returned through that receiver's Lambert
// response (albedo/pi * NdotL) exactly like any other light — never as raw radiance. A dapple
// that skips the BRDF is a multiple of the light's full intensity: on a sun-lit surface a
// filament then outruns the surface's own direct diffuse by ~50x and clips to white, and a
// black floor gets the same white web as a white wall.
vec3 GE_GlassCaustic(vec3 worldPos, vec3 normalWS, vec3 lightDir, float presence,
                     vec3 tint, vec3 diffuseAlbedo, vec3 lightCol, float intensity, float atten)
{
#ifdef GE_TRANSMISSION_ENABLED
    // A refractive surface is the LENS, not the receiver. It sits inside its own glass-tint
    // cascade, so without this it paints its own focused web across itself — the pattern is
    // projected in world space, so it reads as a flat scribble pasted over the glass rather
    // than anything the geometry could produce.
    return vec3(0.0);
#else
    // Constant-folded: at strength 0 the noise field and the focus decode below cost nothing.
    if (kGlassCausticStrength <= 0.0)
        return vec3(0.0);
    float ndotl = max(dot(normalWS, lightDir), 0.0);
    if (presence <= 0.0 || ndotl <= 0.0)
        return vec3(0.0);
    // `presence` carries the encoded refraction convergence (focus/kMax, MAX-blended into the
    // transmittance alpha; 0 = no glass). Decode to the focus multiplier — 1 for flat glass,
    // up to kGlassCausticFocusMax where the lens converges light into a bright core.
    float focus = presence * kGlassCausticFocusMax;
    // Plane perpendicular to the light; project worldPos onto it (world units).
    vec3 t, bt;
    GE_LightPlaneBasis(lightDir, t, bt);
    vec2 planePos = vec2(dot(worldPos, t), dot(worldPos, bt));
    return GE_CausticWeb(planePos, kGlassCausticScale) * focus * kGlassCausticStrength
         * ndotl * diffuseAlbedo * kGlassCausticInvPi * tint * lightCol * intensity * atten;
#endif
}

#endif // GE_CAUSTICS_GLSL
