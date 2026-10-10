// Image-based lighting consumer (M1). Evaluates the Karis split-sum ambient term
// from an irradiance cube (diffuse) + a prefiltered specular cube + a split-sum
// BRDF LUT. Resources bind on set 0 at b18-b21, declared only under
// GE_IBL_ENABLED so non-IBL variants don't grow the descriptor layout. The cubes
// are always valid — an unbaked scene binds an ambient-seeded fallback so this
// degrades to the flat-ambient look, not black. How the cubes are produced is
// the IEnvironmentSource contract, not this file's.

#ifndef GE_IBL_GLSL
#define GE_IBL_GLSL

#ifdef GE_SSSR_NORMAL_ROUGHNESS
// The base GGX lobe that SSR can replace. Export the actual forward evaluation,
// including authored dielectric/film response and every attenuation above it.
// Coat and fuzz reflections remain separate lobes in the forward color.
vec3 ge_sssrIncidentRadiance = vec3(0.0);
vec3 ge_sssrSpecularWeight = vec3(0.0);
vec3 ge_sssrReflectionDirection = vec3(0.0, 0.0, 1.0);
#endif

#include "compat_profile.glsl"  // GE_TAP_LOD0
#include "standard_pbr.glsl"   // GE_SchlickFresnelRoughness + the F0 = mix(0.04, base, metallic) convention
#include "surface_io.glsl"     // SurfaceOutput

// DDGI is an IBL-diffuse-term REPLACEMENT (see the irr formation below), so
// it implicitly requires GE_IBL_ENABLED — there is no standalone DDGI path.
// The C++ per-view keyword OR-in (WorldRenderNode.cpp) sets IBL whenever it
// sets DDGI to keep this true; declared conditionally like every other
// opt-in binding block here so a non-DDGI variant never grows the
// descriptor layout.
#if defined(GE_DDGI_ENABLED)
// Compat fragment: sample the screen-space resolve textures only, never the
// probe atlases. This define makes ddgi_probes.glsl drop the atlas/gather
// layout (and move the volume UBO off b30, where ge_iblSampler sits here).
#if defined(GE_COMPAT_PROFILE)
#define GE_DDGI_RESOLVE_ONLY
#endif
#include "ddgi_probes.glsl"
#include "ddgi_lobe_mix.glsl"

// Scaled glossy-reflection resolve (Shaders/ddgi_glossy_resolve.comp): the
// probe reflection gather pre-evaluated per view at
// DDGIVolume.GlossyResolveScale of the render extent, one texture per baked
// lobe (rough = roughLobeMix 1, glossy = roughLobeMix 0), each
// coverage-premultiplied radiance in rgb / coverage in a. Every fragment reads
// these; there is no per-fragment gather (see the consume below for why).
// The resolve's reflection direction is the previous frame's SHADING normal
// reprojected (the forward MRT normal slice), falling back to a depth-derived
// geometric normal where no history covers the surface. DDGIVolume.uParams2.w
// says whether the view declared the pass; a view that could not (no 1-sample
// depth, kernel missing) binds the 1x1 black fallback and keeps the flat
// environment sample.
// Declared HERE, not in ddgi_probes.glsl: the resolve kernel itself includes
// that file and must not grow its layout with outputs it never binds.
#if defined(GE_COMPAT_PROFILE)
// Compat declares the three resolve targets as separate texture2D images over
// b30 (the standalone IBL sampler): three sampled-texture slots, no sampler slots.
layout(set = 0, binding = 41) uniform texture2D ge_ddgiResolveRoughTex;
#define ge_ddgiResolveRough sampler2D(ge_ddgiResolveRoughTex, ge_iblSampler)
layout(set = 0, binding = 42) uniform texture2D ge_ddgiResolveGlossyTex;
#define ge_ddgiResolveGlossy sampler2D(ge_ddgiResolveGlossyTex, ge_iblSampler)
layout(set = 0, binding = 43) uniform texture2D ge_ddgiResolveIrradianceTex;
#define ge_ddgiResolveIrradiance sampler2D(ge_ddgiResolveIrradianceTex, ge_iblSampler)
#else
layout(set = 0, binding = 41) uniform sampler2D ge_ddgiResolveRough;
layout(set = 0, binding = 42) uniform sampler2D ge_ddgiResolveGlossy;
// Diffuse irradiance from the same resolve pass, looked up along the same
// normal as the reflection — a per-fragment 8-probe loop it removes entirely.
layout(set = 0, binding = 43) uniform sampler2D ge_ddgiResolveIrradiance;
#endif
#endif

// Global per-frame environment parameters (NOT per-material — the param block
// and the 8 texture slots are full). The reflected resource name is the instance
// "Env", so the C++ binder must register this buffer under "Env" (cf. LightUBO/Light).
layout(set = 0, binding = 21) uniform EnvDataUBO {
    float iblIntensity;     // linear radiance scale on env-SOURCED radiance only, pre-exposure.
                            // Folded inside GE_SampleEnvironmentIrradiance/Prefilter — never on
                            // the GE_EvaluateIBL returns (see the sampler comment below).
    float prefilterMaxMip;  // = mipCount - 1, the highest valid specular mip INDEX
    float localBoxProjection;
    float lowerHemisphereDarkness;
    vec4 localProbePositionWS;
    vec4 localBoxCenterWS;
    vec4 localBoxHalfExtentsWS;
    vec4 localBoxAxisXWS;
    vec4 localBoxAxisYWS;
    vec4 localBoxAxisZWS;
    vec4 ambientTintSky;      // scene-linear RGB in .rgb (.a unused); white = identity
    vec4 ambientTintEquator;
    vec4 ambientTintGround;
    // Additive authored ambient irradiance FLOOR (AmbientLight component). Scene-linear RGB in
    // .rgb, ALREADY premultiplied by intensity/203 on the CPU. ambientFloorParams.x is the mode
    // (0 = off, 1 = flat, 2 = gradient); .y is affectSpecular (0/1). Mode 0 (no component) makes
    // GE_AmbientFloor return exactly vec3(0), so the diffuse term is byte-identical.
    vec4 ambientFloorSky;
    vec4 ambientFloorEquator;
    vec4 ambientFloorGround;
    vec4 ambientFloorParams;
} Env;

#if defined(GE_COMPAT_PROFILE)
// WebGPU counts one sampler per sampled texture per stage, and the IBL trio's
// three were what pushed the forward variant past the 16-per-stage cap. Under
// the compat profile the trio is declared as separate images over ONE
// standalone sampler (binding 30 — no image shares that slot, so the WGSL
// cook's combined-split renumbering leaves it alone). The macros rebuild the
// combined-sampler names at every use site, so the sampling code below is one
// arm, not two. Bound by name: ge_irradianceCubeTex / ge_prefilterCubeTex /
// ge_brdfLUTTex / ge_iblSampler (RenderServicesWorldPass registers both
// spellings).
layout(set = 0, binding = 18) uniform textureCube ge_irradianceCubeTex;
layout(set = 0, binding = 19) uniform textureCube ge_prefilterCubeTex;
layout(set = 0, binding = 20) uniform texture2D   ge_brdfLUTTex;
layout(set = 0, binding = 30) uniform sampler     ge_iblSampler;
#define ge_irradianceCube samplerCube(ge_irradianceCubeTex, ge_iblSampler)
#define ge_prefilterCube  samplerCube(ge_prefilterCubeTex, ge_iblSampler)
#define ge_brdfLUT        sampler2D(ge_brdfLUTTex, ge_iblSampler)
// The irradiance cube and the BRDF LUT are single-mip, so their taps are
// GE_TAP_LOD0: explicit LOD 0 under compat, texture() on desktop, the same texel
// either way. Implicit-derivative sampling is illegal in WGSL after a non-uniform
// branch, and the cascade shadow filters (PCSS's early-outs) run before the IBL.
#else
layout(set = 0, binding = 18) uniform samplerCube ge_irradianceCube;
layout(set = 0, binding = 19) uniform samplerCube ge_prefilterCube;
layout(set = 0, binding = 20) uniform sampler2D   ge_brdfLUT;
#endif

// Apply ground occlusion at consumption time instead of baking black radiance
// into the shared environment. This preserves a lossless environment for
// surfaces that own their horizon model while retaining the authored control on
// ordinary materials. The ramp matches the former sky-capture implementation.
float GE_EnvironmentGroundVisibility(vec3 directionWS, float participation)
{
    vec3 dir = normalize(directionWS);
    float lowerHemisphere = smoothstep(0.0, 0.25, -dir.y);
    float darkness = clamp(Env.lowerHemisphereDarkness, 0.0, 1.0)
                   * clamp(participation, 0.0, 1.0);
    return 1.0 - darkness * lowerHemisphere;
}

// Env.iblIntensity is folded HERE, not on the GE_EvaluateIBL returns, so only env-SOURCED
// radiance scales with the sky's intensity: the engine forces the intensity to 0 whenever no
// environment source has active content, and a tail multiply would zero the authored
// AmbientLight floor in exactly the no-sky scenes it exists for (it also wrongly scaled the
// screen-space refraction grab by the sky slider). Pinned by IblShaderContractTests.
vec3 GE_SampleEnvironmentIrradiance(vec3 directionWS, float groundParticipation)
{
    return GE_TAP_LOD0(ge_irradianceCube, normalize(directionWS)).rgb
         * GE_EnvironmentGroundVisibility(directionWS, groundParticipation)
         * Env.iblIntensity;
}

vec3 GE_SampleEnvironmentPrefilter(vec3 directionWS, float lod, float groundParticipation)
{
    return textureLod(ge_prefilterCube, normalize(directionWS), lod).rgb
         * GE_EnvironmentGroundVisibility(directionWS, groundParticipation)
         * Env.iblIntensity;
}

// Three-color ambient gradient tint blended by world-normal.y: up (+Y) -> sky, horizontal ->
// equator, down (-Y) -> ground. Branchless. Applied
// to the diffuse irradiance ONLY (specular stays physical). White colors -> vec3(1) so the
// default is a byte-exact no-op.
vec3 GE_AmbientGradientTint(vec3 normalWS)
{
    float up   = max(normalWS.y, 0.0);
    float down = max(-normalWS.y, 0.0);
    float eq   = 1.0 - up - down;
    return Env.ambientTintSky.rgb * up
         + Env.ambientTintEquator.rgb * eq
         + Env.ambientTintGround.rgb * down;
}

// Additive authored ambient irradiance floor (AmbientLight component). Mode: 0 off, 1 flat,
// 2 gradient. Off returns EXACTLY vec3(0.0) so a scene without the component adds nothing
// (x + 0.0 == x) and stays byte-identical. Gradient reuses the GE_AmbientGradientTint blend
// (up=sky, horizon=equator, down=ground) but ADDITIVELY; flat returns the constant Sky slot.
// Scene-linear and pre-exposure — summed into the same linear diffuse budget as the sampled
// irradiance, then flowed through kD / AO / albedo like any diffuse ambient.
vec3 GE_AmbientFloor(vec3 normalWS)
{
    float mode = Env.ambientFloorParams.x;
    if (mode < 0.5)
        return vec3(0.0);
    if (mode < 1.5)
        return Env.ambientFloorSky.rgb; // flat: single constant color
    float up   = max(normalWS.y, 0.0);
    float down = max(-normalWS.y, 0.0);
    float eq   = 1.0 - up - down;
    return Env.ambientFloorSky.rgb * up
         + Env.ambientFloorEquator.rgb * eq
         + Env.ambientFloorGround.rgb * down;
}

#ifdef GE_SCENECOLOR_GRAB
// Post-opaque scene-colour grab for screen-space refraction (the dedicated transmissive pass only).
// Declared under the keyword so non-screen-space variants never grow the set-0 layout with b22.
// ge_sceneDepth (b17) backs both depth guards — the adapter's same-pixel occlusion raw-compares it,
// the thick path's cross-pixel exit reject reconstructs eye distance via the reverse-Z helpers — so
// it's declared for every grab variant.
#include "screen_space.glsl"
layout(set = 0, binding = 22) uniform sampler2D ge_sceneColor;
#endif

// Lagarde (Frostbite) specular occlusion. Applying raw diffuse AO to the specular term
// over-occludes broad reflections in cavities (and under-occludes mirror reflections that
// the surface physically blocks). Derive a roughness/view-aware specular AO from the diffuse
// AO instead, so crevices stop showing mirror-bright env reflections ("shiny dirt").
float GE_ComputeSpecularAO(float NdotV, float ao, float roughness)
{
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// Jimenez 2016 multi-bounce occlusion: an albedo-tinted response that adds back the
// energy a single-bounce occlusion term over-darkens (coloured inter-reflection in
// cavities). `visibility` is any surface occlusion — the material's map, the screen-
// space GTAO, or their min — and is required in [0,1] (every producer bounds it: the
// 1x1 white AO fallback, the [0,1] mixes in the surface shaders, and the min() in
// gtao_consume.glsl only lowers it).
// The upper clamp makes full visibility an EXACT no-op: the polynomial fit overshoots
// 1.0 by ~1e-4 at visibility 1 for albedo above 0.8, which this term would otherwise
// spend on the indirect diffuse of every unoccluded material.
vec3 GE_GtaoMultiBounce(float visibility, vec3 albedo)
{
    vec3 a =  2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c =  2.7552 * albedo + 0.6903;
    vec3 fit = ((visibility * a + b) * visibility + c) * visibility;
    return min(max(vec3(visibility), fit), vec3(1.0));
}

// Ambient fuzz proxy layered over an already-shaded ambient stack (`beneath` = base [+coat]).
// The Charlie lobe is broad/retroreflective with no GGX peak, so the diffuse-convolved irradiance
// is the env proxy (matching the ambient sheen below), tinted by fuzzColor and weighted by the
// Charlie directional albedo — the same term the ambient sheen below uses. As the
// OUTERMOST layer it attenuates the stack beneath it by its own reflectance (the env analogue of the
// direct path's over-coat (1 - refl) split), so a white fuzz over a white env cannot exceed the env.
// fuzzColor -> 0 leaves `beneath` byte-exact.
vec3 GE_LayerAmbientFuzzOverCoat(vec3 beneath, vec3 fuzzColor, float fuzzRoughness,
                                 vec3 irradiance, float NdotV, float ao)
{
    float E        = GE_CharlieDirectionalAlbedo(NdotV, fuzzRoughness);
    vec3  fuzzAmb  = fuzzColor * irradiance * E * ao;
    float refl     = clamp(max(max(fuzzColor.r, fuzzColor.g), fuzzColor.b) * E, 0.0, 1.0);
    return beneath * (1.0 - refl) + fuzzAmb;
}

vec3 GE_LocalReflectionDirection(vec3 positionWS, vec3 reflectionDir)
{
    vec3 R = normalize(reflectionDir);
    vec3 halfExtents = Env.localBoxHalfExtentsWS.xyz;
    vec3 axisX = Env.localBoxAxisXWS.xyz;
    vec3 axisY = Env.localBoxAxisYWS.xyz;
    vec3 axisZ = Env.localBoxAxisZWS.xyz;
    bool validBox = Env.localBoxProjection > 0.5 &&
                    all(greaterThan(halfExtents, vec3(1.0e-4))) &&
                    dot(axisX, axisX) > 1.0e-4 &&
                    dot(axisY, axisY) > 1.0e-4 &&
                    dot(axisZ, axisZ) > 1.0e-4;
    if (!validBox)
        return R;

    axisX = normalize(axisX);
    axisY = normalize(axisY);
    axisZ = normalize(axisZ);

    vec3 toPosition = positionWS - Env.localBoxCenterWS.xyz;
    vec3 localPosition = vec3(dot(toPosition, axisX),
                              dot(toPosition, axisY),
                              dot(toPosition, axisZ));
    vec3 localR = vec3(dot(R, axisX), dot(R, axisY), dot(R, axisZ));

    vec3 safeR = localR;
    safeR.x = abs(safeR.x) < 1.0e-5 ? (safeR.x < 0.0 ? -1.0e-5 : 1.0e-5) : safeR.x;
    safeR.y = abs(safeR.y) < 1.0e-5 ? (safeR.y < 0.0 ? -1.0e-5 : 1.0e-5) : safeR.y;
    safeR.z = abs(safeR.z) < 1.0e-5 ? (safeR.z < 0.0 ? -1.0e-5 : 1.0e-5) : safeR.z;

    vec3 boxMin = -halfExtents;
    vec3 boxMax = halfExtents;
    vec3 invR = 1.0 / safeR;
    vec3 t0 = (boxMin - localPosition) * invR;
    vec3 t1 = (boxMax - localPosition) * invR;
    vec3 tNear = min(t0, t1);
    vec3 tFar = max(t0, t1);
    float tEnter = max(max(tNear.x, tNear.y), tNear.z);
    float tExit = min(min(tFar.x, tFar.y), tFar.z);
    if (tExit <= max(tEnter, 0.0))
        return R;

    vec3 localHit = localPosition + localR * tExit;
    vec3 probeLocal = vec3(dot(Env.localProbePositionWS.xyz - Env.localBoxCenterWS.xyz, axisX),
                           dot(Env.localProbePositionWS.xyz - Env.localBoxCenterWS.xyz, axisY),
                           dot(Env.localProbePositionWS.xyz - Env.localBoxCenterWS.xyz, axisZ));
    vec3 correctedLocal = localHit - probeLocal;
    vec3 correctedWS = axisX * correctedLocal.x +
                       axisY * correctedLocal.y +
                       axisZ * correctedLocal.z;
    if (dot(correctedWS, correctedWS) <= 1.0e-8)
        return R;
    return normalize(correctedWS);
}

// Scene-linear HDR ambient contribution (pre-exposure). Sums into finalColor in
// the same linear space as the analytic BRDF and flows through the single
// terminal encoder unchanged — never apply an OETF here.
vec3 GE_EvaluateIBL(SurfaceOutput so, vec3 V, vec3 positionWS)
{
    vec3  N     = so.normalWS;
    float NdotV = max(dot(N, V), 1e-4);
    vec3  R     = reflect(-V, N);
    vec3  Rbase = R; // isotropic reflection preserved for the (isotropic) clear-coat lobe
    vec3  F0    = mix(GE_DielectricF0(so.specularWeight, so.specularColor, so.specularIor), so.baseColor, so.metallic); // matches standard_pbr.glsl

#ifdef GE_ANISOTROPY_ENABLED
    // HDRP/Filament bent-reflection: stretch the env mirror along the groove axis so the
    // reflection smears the same way the punctual highlight does. Signed anisotropy picks
    // the axis (+ -> bitangent, - -> tangent). The prefilter cube is an isotropic
    // convolution, so this only redirects the sample (an approximation, like sheen's IBL).
    if (so.anisotropy != 0.0 && dot(so.tangentWS, so.tangentWS) > 1e-4)
    {
        // Revolve the (T,B) frame in-plane by the authored angle, matching the direct path in
        // standard_pbr.glsl so the env reflection bends along the SAME groove as the punctual
        // highlight. Guarded so rotation 0 leaves the frame untouched (byte-identical no-op).
        vec3 T = so.tangentWS;
        vec3 B = so.bitangentWS;
        if (so.anisotropyRotation != 0.0)
        {
            float ca = cos(so.anisotropyRotation);
            float sa = sin(so.anisotropyRotation);
            vec3 rT =  ca * T + sa * B;
            vec3 rB = -sa * T + ca * B;
            T = rT;
            B = rB;
        }
        vec3 anisoDir   = so.anisotropy >= 0.0 ? B : T;
        vec3 anisoTan   = cross(anisoDir, V);
        vec3 anisoNorm  = normalize(cross(anisoTan, anisoDir));
        // Bend grows with roughness (Filament): a near-mirror reflection is sharp and the
        // redirected sample only stretches once the prefilter cube is blurred enough to show it.
        float bend      = abs(so.anisotropy) * clamp(5.0 * so.roughness, 0.0, 1.0);
        vec3 bentNormal = normalize(mix(N, anisoNorm, bend));
        R = reflect(-V, bentNormal);
    }
#endif

    // Roughness-aware Fresnel — used ONLY for the diffuse energy weight.
    vec3  F     = GE_SchlickFresnelRoughness(F0, NdotV, so.roughness);
    vec3  kD    = (vec3(1.0) - F) * (1.0 - so.metallic);

    // Diffuse: the convolver already yields E/PI — do NOT re-divide by PI.
    // kD is applied exactly once (in the return). Sample the irradiance cube once and reuse
    // it for the optional sheen ambient below.
    // GTAO bent normal as the diffuse irradiance direction (== N without GTAO):
    // it tilts the hemisphere away from occluders so cavities sample less skylight.
    // Ambient gradient tint: multiply the authored 3-color wash into the diffuse irradiance
    // only (specular `pref` below is untouched). Blend by the geometric normal N, not the GTAO
    // bent normal used for the cube sample: the wash is an authored direction-to-colour ramp, so it
    // must follow where the surface actually faces, not where occluders pushed the sample.
    // White = identity.
    vec3  irr     = GE_SampleEnvironmentIrradiance(so.bentNormalWS, so.iblGroundDarkening)
                  * GE_AmbientGradientTint(N);
#if defined(GE_DDGI_ENABLED)
    // DDGI REPLACES (not adds to) the flat-cube diffuse sample when a volume
    // is active for this world: a probe's own miss ray already samples this
    // SAME irradiance cube (Includes/ddgi_hit_shade.glsl's GE_DDGISampleSkyMiss),
    // so adding both would double-count the sky. DDGIVolume.uParams1.x is 0
    // when no volume resolved for this world (the fallback UBO, or a world
    // with no DDGIVolume component) — irr keeps the flat-cube sample then,
    // so a DDGI-keyword variant with no active volume is visually identical
    // to the pre-DDGI look, not a black hole.
    if (DDGIVolume.uParams1.x > 0.5)
        // Shading normal, not the GTAO bent normal. Bent normals tilt the
        // hemisphere away from nearby occluders — correct for sky visibility,
        // but a probe field already stores those occluders' bounced colour in
        // the lobe that faces them. Sampling with the bent vector reads sky /
        // far-wall irradiance on a soffit next to a banner and drops the
        // texture-colour stain. so.ao still carries GTAO below.
    // Deliberately NO per-fragment fallback here. A fallback call costs this
    // shader ~4.5 ms/frame on the Sponza parity scene in register pressure
    // alone, measured, whether or not a single fragment reaches it — the
    // diffuse gather is far too big to keep on standby. A view that cannot
    // declare the resolve (no 1-sample depth) keeps the flat environment
    // sample instead; every pipeline that resolves depth gets probe diffuse.
    if (DDGIVolume.uParams2.w != 0)
    {
        // Bilateral upsample: four point taps around the fragment, each kept
        // only if the depth it was resolved at agrees with this fragment's.
        // A plain bilinear tap of a reduced-resolution term drags irradiance
        // across silhouettes. Falls back to the nearest tap when every
        // neighbour disagrees (a one-pixel sliver), which is still this
        // surface's own value at worst one resolve texel away.
        const float kUpsampleDepthTolerance = 0.02;
        vec2 resolveUV = gl_FragCoord.xy * ge_screenSize.zw;
        vec2 resolveTexel = 1.0 / vec2(textureSize(ge_ddgiResolveIrradiance, 0));
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        for (int tap = 0; tap < 4; ++tap)
        {
            vec2 o = vec2((tap & 1) != 0 ? 0.5 : -0.5, (tap & 2) != 0 ? 0.5 : -0.5);
            vec4 s = textureLod(ge_ddgiResolveIrradiance, resolveUV + o * resolveTexel, 0.0);
            float rel = abs(s.a - gl_FragCoord.z) / max(gl_FragCoord.z, 1.0e-6);
            if (s.a <= 0.0 || rel > kUpsampleDepthTolerance)
                continue;
            acc += s.rgb;
            wsum += 1.0;
        }
        irr = wsum > 0.0 ? acc / wsum
                         : textureLod(ge_ddgiResolveIrradiance, resolveUV, 0.0).rgb;
    }
#endif
    // Add the authored irradiance floor (AmbientLight) to the DIFFUSE budget only — irr stays the
    // pure sampled env so the sheen ambient below reuses it unchanged. Floor is exactly 0 without an
    // AmbientLight component, so (irr + 0.0) * baseColor == irr * baseColor (byte-identical).
    vec3  diffuse = (irr + GE_AmbientFloor(N)) * so.baseColor;

    // Specular split-sum: reconstruct with F0, not the roughness F — the LUT
    // bakes the grazing-angle Fresnel ramp into its bias term.
    float lod      = so.roughness * Env.prefilterMaxMip;        // linear; inverse of the generator mapping
    vec3  prefDir  = GE_LocalReflectionDirection(positionWS, R);
    vec3  pref     = GE_SampleEnvironmentPrefilter(prefDir, lod, so.iblGroundDarkening);
    // Optional (AmbientLight.AffectSpecular): also fill the primary reflection lobe with the floor as
    // a constant-radiance probe fallback, so it flows through the split-sum / multiscatter / horizon
    // / specular-AO path below. Default-off and floor-0 both leave pref untouched -> byte-identical.
    if (Env.ambientFloorParams.y > 0.5)
        pref += GE_AmbientFloor(prefDir);
#if defined(GE_DDGI_ENABLED)
    // Composite DDGI's baked reflection lobes INTO pref (the same "replace
    // the raw env tap before the split-sum BRDF weighting" pattern the
    // diffuse irr hook above uses) — so the existing Fresnel/multiscatter/
    // horizon/specular-AO math below applies to them identically, and
    // whatever composites OVER this pass (SSSR) still runs afterward with its
    // own priority unchanged; this hook only changes the specular value SSSR
    // blends with/over.
    //
    // COMPOSITE, not a blend: the lobes report how much of their solid angle
    // they actually resolved geometry for (coverage, GE_DDGIRayCoverage), and
    // only that fraction displaces the prefiltered cube. Where a probe saw
    // nothing — open sky, or a direction its rays never covered — the real
    // cube survives at full strength instead of being crossfaded toward a
    // probe value that carries no information. A flat mix cannot express
    // that, and dims exactly the reflections it has least to say about.
    if (DDGIVolume.uParams1.z > 0.5)
    {
        float roughLobeMix = GE_DDGIRoughLobeMix(so.roughness);
        vec4 ddgiReflection = vec4(0.0);
        // The scaled resolve is the ONLY source of probe reflections here.
        // Deliberately no per-fragment gather for mirror-like fragments: the
        // 8-probe loop costs this shader its registers whether or not a
        // fragment reaches it (measured ~2 ms/frame on the Sponza parity
        // scene with the branch never taken), and
        // DDGIReflectionParallaxTests pins its absence. GlossyResolveScale
        // Full evaluates the resolve per pixel; its direction is still the
        // previous frame's shading normal, one frame late under motion.
        if (DDGIVolume.uParams2.w != 0)
        {
            // Scaled resolve path: screen taps of the pre-blurred resolve
            // textures replace the 2-cascade x 8-probe inline gather. The
            // per-lobe crossfade is exact (the gather is linear in
            // roughLobeMix). Four bilinear taps at half-texel offsets (a 3x3
            // tent) instead of one: a single magnified bilinear tap keeps the
            // resolve grid's square texel steps visible on curved reflectors.
            vec2 resolveUV = gl_FragCoord.xy * ge_screenSize.zw;
            vec2 resolveTexel = 1.0 / vec2(textureSize(ge_ddgiResolveRough, 0));
            vec4 roughTap = vec4(0.0);
            vec4 glossyTap = vec4(0.0);
            for (int tap = 0; tap < 4; ++tap)
            {
                vec2 o = vec2((tap & 1) != 0 ? 0.5 : -0.5, (tap & 2) != 0 ? 0.5 : -0.5);
                vec2 uvTap = resolveUV + o * resolveTexel;
                roughTap += textureLod(ge_ddgiResolveRough, uvTap, 0.0);
                glossyTap += textureLod(ge_ddgiResolveGlossy, uvTap, 0.0);
            }
            ddgiReflection = mix(glossyTap, roughTap, roughLobeMix) * 0.25;
        }
        pref = GE_DDGICompositeReflection(pref, ddgiReflection, clamp(DDGIVolume.uParams1.w, 0.0, 1.0));
    }
#endif
    vec2  ab       = GE_TAP_LOD0(ge_brdfLUT, vec2(NdotV, so.roughness)).rg;  // CLAMP_TO_EDGE sampler, no mips
    // Thin-film iridescence tints the reflected environment: substitute the iridescent
    // reflectance for F0 in the split-sum scale (the LUT bias term ab.y, baking the grazing
    // ramp, stays on the geometric reconstruction). Evaluated at NdotV, the env analogue of
    // VdotH. Multiscatter compensation below stays on the base F0 (energy add-back, not tint).
    vec3  specF0   = F0;
#ifdef GE_IRIDESCENCE_ENABLED
    specF0 = mix(F0, GE_Iridescence(NdotV, so.thinFilmIor, so.thinFilmThickness, F0), so.thinFilmWeight);
#endif
    vec3 specularWeight = (specF0 * ab.x + ab.y);
    specularWeight *= GE_MultiscatterCompensation(F0, ab.x + ab.y); // add back single-scatter energy loss (Fdez-Aguera)
    // Horizon occlusion (Filament): at grazing angles on normal-mapped surfaces R can dip below
    // the geometric horizon and pull in env radiance the surface physically occludes — fade it.
    float horizon = clamp(1.0 + dot(R, N), 0.0, 1.0);
    specularWeight *= horizon * horizon;
    vec3 specular = pref * specularWeight;

    // --- Optional ambient sheen: the Charlie lobe is broad/retroreflective with no GGX
    //     peak, so the diffuse-convolved IRRADIANCE cube is the right env proxy (NOT the
    //     GGX prefilter cube). Tint by sheenColor, weight by a grazing term so the fuzz
    //     glows at the silhouette. Added to the base before clear-coat attenuation.
    // AO is split: raw diffuse AO darkens the diffuse/sheen, a roughness-aware specular AO
    // darkens the reflections (Lagarde) so cavities stop showing mirror-bright env radiance.
    float specAO = GE_ComputeSpecularAO(NdotV, so.ao, so.roughness);
#ifdef GE_SSSR_NORMAL_ROUGHNESS
    ge_sssrIncidentRadiance = pref;
    ge_sssrSpecularWeight = specularWeight * specAO;
    ge_sssrReflectionDirection = R;
  #ifdef GE_FUZZ_ENABLED
    float fuzzAlbedo = GE_CharlieDirectionalAlbedo(NdotV, so.fuzzRoughness);
    float fuzzReflectance = clamp(max(max(so.fuzzColor.r, so.fuzzColor.g), so.fuzzColor.b) * fuzzAlbedo, 0.0, 1.0);
    ge_sssrSpecularWeight *= 1.0 - fuzzReflectance;
  #endif
#endif

    // Transmission (refractive dielectric / glass): steal energy from the diffuse budget and
    // replace it with a refracted environment sample. kD already carries (1-F)*(1-metallic), so
    // the split takes a SINGLE (1-F) (no double-count) and metals transmit nothing automatically.
    // Phase 0 refracts the prefiltered env cube (roughness drives the blur via the shared `lod`);
    // screen-space refraction of the lit scene behind the surface is a later phase. Folded into
    // baseAmbient BEFORE the clear-coat split below so a coat correctly attenuates it for free.
    float diffuseScale = 1.0;
    vec3  transmission = vec3(0.0);
#ifdef GE_TRANSMISSION_ENABLED
    float wT      = clamp(so.transmissionWeight, 0.0, 1.0);
    float iorT    = max(so.specularIor, 1.0001);
    vec3  refrDir = refract(-V, N, 1.0 / iorT);
    // refract() returns 0 past the critical angle (total internal reflection) -> fall back to the
    // reflection vector so the env-cube sample (and the screen-edge fallback) never reads garbage.
    if (dot(refrDir, refrDir) < 1e-6)
        refrDir = R;
    // Env cube = the always-available background (no-grab variants, thumbnails) AND the
    // screen-edge / foreground-reject fallback for the screen-space path below.
    vec3  background = GE_SampleEnvironmentPrefilter(refrDir, lod, so.iblGroundDarkening)
                     * so.transmissionColor;
  #ifdef GE_SCENECOLOR_GRAB
    // The depth prepass peels transmissive (RenderServices ExecuteDepthOnlyPass), so ge_sceneDepth
    // and the grab are opaque-only: the grab holds the scene BEHIND the glass disk, and the thick
    // path's foreground-reject sees real geometry, never the glass itself.
    //
    // THIN path (single-tap, the default + the universal fallback): offset the scene-colour sample
    // by the view-space front normal, aspect-corrected and scaled by the IOR bend. View +Y is up but
    // the grab's texture V increases downward (negative viewport for Y-up), so the vertical component
    // is negated — same family as shadowUV.y = 1-y and the IBL (1-uv.y) flips.
    const float kThinRefractionStrength = 0.3;  // authored single-tap bend
    const float kScreenEdgeFadeMargin   = 0.04; // screen-border fade back to the env cube
    vec2  ssScreenUV = gl_FragCoord.xy * ge_screenSize.zw;
    vec3  ssNv       = mat3(ge_view) * N;
    vec2  ssOffset   = vec2(ssNv.x, -ssNv.y) * ((iorT - 1.0) * kThinRefractionStrength);
    ssOffset.x      *= ge_screenSize.y * ge_screenSize.z; // aspect (height/width) so x,y bend equally
    vec2  ssUV       = ssScreenUV + ssOffset;
    vec2  ssEdge     = min(ssUV, vec2(1.0) - ssUV);
    float ssEdgeFade = clamp(min(ssEdge.x, ssEdge.y) / kScreenEdgeFadeMargin, 0.0, 1.0);
    vec3  ssBg       = textureLod(ge_sceneColor, clamp(ssUV, 0.0, 1.0), 0.0).rgb * so.transmissionColor;
   #ifdef GE_TRANSMISSION_THICK
    // THICK path (two-surface, sphere-proxy): refract the view ray at the front face, walk the
    // authored thickness through the glass, refract again at an approximate back face, and sample the
    // grab at the PROJECTED exit point — so a glass sphere shows the inverted/magnified scene (the
    // crystal-ball lens). Overwrites the thin ssBg only when the exit tap is valid; on entry-TIR /
    // behind-camera / off-screen / foreground-occluded it leaves the thin tap standing.
    {
        // World-unit path length the refracted ray travels per glass surface (authored, ~the sphere
        // radius). Total chord ~= 2x this. Drives the lens magnitude; physical, not a screen fudge.
        float thick = max(so.refractionDistance, 0.0);
        // Reconstruct this fragment's view-space position. ndc.y uses the negative-viewport
        // convention (V grows downward) so it matches the exit reprojection below — otherwise P0
        // lands vertically mirrored and the lensed image (and the depth guard) flip.
        vec2  ndc0  = vec2(ssScreenUV.x * 2.0 - 1.0, 1.0 - 2.0 * ssScreenUV.y);
        vec4  vp0   = ge_invProj * vec4(ndc0, gl_FragCoord.z, 1.0);
        vec3  P0    = vp0.xyz / max(abs(vp0.w), 1e-5);
        vec3  Iv    = mat3(ge_view) * (-V);                       // camera -> surface, view space (unit: orthonormal view x unit V)
        vec3  Nv    = ssNv;                                       // already view-space + unit length
        vec3  T1    = refract(Iv, Nv, 1.0 / iorT);                // front-face refraction (air -> glass)
        if (dot(T1, T1) >= 1e-6)                                  // skip entry-side TIR -> keep thin
        {
            vec3  Pmid  = P0 + T1 * thick;
            vec3  Nback = normalize(-Nv + 2.0 * dot(Nv, T1) * T1); // proxy back-face normal
            vec3  T2    = refract(T1, Nback, iorT);               // back-face refraction (glass -> air)
            vec3  Pexit = dot(T2, T2) >= 1e-6 ? Pmid + T2 * thick : Pmid;
            vec4  clipE = ge_proj * vec4(Pexit, 1.0); // view -> clip (uploaded ge_proj; no per-fragment inverse)
            if (clipE.w > 1e-4)                                   // exit is in front of the camera
            {
                vec2 ndcE = clipE.xy / clipE.w;
                vec2 uvE  = vec2(0.5 + 0.5 * ndcE.x, 0.5 - 0.5 * ndcE.y); // V grows downward
                if (all(greaterThanEqual(uvE, vec2(0.0))) && all(lessThanEqual(uvE, vec2(1.0))))
                {
                    float fragEye  = abs(P0.z); // == GE_EyeDepthFromRaw(gl_FragCoord.z, ssScreenUV)
                    float sceneEye = GE_EyeDepthFromRaw(texelFetch(ge_sceneDepth, GE_ScreenTexel(uvE), 0).r, uvE);
                    if (sceneEye >= fragEye) // exit tap is behind the glass -> use it; else keep thin
                        ssBg = textureLod(ge_sceneColor, uvE, 0.0).rgb * so.transmissionColor;
                }
            }
            // Beer–Lambert volume absorption: the transmitted background deepens toward
            // attenuationColor over attenuationDistance of in-glass travel, so a thick core reads
            // darker/more saturated than a thin edge (distinct from the flat surface transmissionColor).
            // Only the thick path has a real volume chord; thin/env taps have no depth, so they skip it.
            // Defaults make this a guaranteed no-op: white colour -> log(1)=0 -> absorption 0 -> exp(0)=1.
            if (so.attenuationDistance > 0.0)
            {
                vec3  absorption = -log(clamp(so.attenuationColor, 1e-3, 1.0)) / so.attenuationDistance; // per-channel
                // Exact chord of the sphere proxy this path already assumes: through a sphere of
                // radius `thick`, a ray refracted to T1 at a surface point with normal Nv travels
                // 2 * thick * |cos(T1, Nv)|. That is the whole point of a volume absorption term —
                // it must VARY over the silhouette, so the core reads deep and the rim reads thin.
                // A flat 2 * thick tints every pixel identically and cannot show path length at all.
                // (The lens walk above is a separate two-segment approximation of the same proxy;
                // it steers the sample, this steers how much of it survives.)
                float pathLen    = 2.0 * thick * abs(dot(T1, Nv));
                ssBg            *= exp(-absorption * pathLen);
            }
        }
    }
   #endif
    background       = mix(background, ssBg, ssEdgeFade);
  #endif
    transmission  = kD * wT * background;
    diffuseScale  = 1.0 - wT;
#endif

    // Indirect diffuse always carries the surface occlusion. so.ao is whatever the SURFACE
    // wrote — a sampled occlusion map (ORM.r), or a procedural value such as terrain
    // grass's root-darkening ramp — and the screen-space GTAO min()s into it only in the
    // GE_GTAO_ENABLED variant. The grass colour draw never compiles that variant (its pipeline
    // is the material's base keywords), so the ramp is the blades' only occlusion term. This
    // must not be gated on GE_GTAO_ENABLED: the keyword is set
    // only for views with an active AO volume, and gating here discards every surface's
    // occlusion in all the others. The multi-bounce response is used in BOTH variants so
    // enabling an AO volume cannot jump the surface's contribution wherever GTAO itself
    // finds full visibility. Specular stays on the Lagarde specAO above.
    vec3 baseAmbient = kD * diffuse * GE_GtaoMultiBounce(so.ao, so.baseColor) * diffuseScale
                     + specular * specAO + transmission;
#ifdef GE_SHEEN_ENABLED
    vec3  shIrr = irr; // reuse the diffuse irradiance sample (already E/PI == uniform radiance)
    float shE   = GE_CharlieDirectionalAlbedo(NdotV, so.sheenRoughness);
    baseAmbient += so.sheenColor * shIrr * shE * so.ao;
#endif

#ifdef GE_CLEARCOAT_ENABLED
    // Clear-coat environment reflection: a second split-sum sample at coat roughness
    // with the coat's F0 (from its IOR), reusing the same reflection vector. The coat sits above
    // the ENTIRE base, so it transmits (1 - ccFc) of all base light (diffuse + specular
    // + sheen) — matching the punctual lobe — then adds its own reflection.
    // Under GE_COAT_NORMAL_ENABLED the coat reflects about its OWN normal (coat-only normal map):
    // recompute the reflection vector and grazing cosine from so.coatNormalWS so the env mirror
    // follows the lacquer microstructure. #else reuses the base Rbase/NdotV verbatim.
#ifdef GE_COAT_NORMAL_ENABLED
    vec3  ccNrm   = so.coatNormalWS;
    vec3  ccRefl  = reflect(-V, ccNrm);
    float ccNdotV = max(dot(ccNrm, V), 1e-4);
#else
    vec3  ccRefl  = Rbase;       // coat reflects about true N (isotropic)
    float ccNdotV = NdotV;
#endif
    float ccLod  = so.clearCoatRoughness * Env.prefilterMaxMip;
    vec3  ccPrefDir = GE_LocalReflectionDirection(positionWS, ccRefl);
    vec3  ccPref = GE_SampleEnvironmentPrefilter(ccPrefDir, ccLod, so.iblGroundDarkening);
    vec2  ccAb   = GE_TAP_LOD0(ge_brdfLUT, vec2(ccNdotV, so.clearCoatRoughness)).rg;
    float ccFc   = GE_SchlickFresnel(vec3(GE_DielectricF0Scalar(so.clearCoatIor)), ccNdotV).x * so.clearCoat;
    vec3  ccSpec = ccPref * (GE_DielectricF0Scalar(so.clearCoatIor) * ccAb.x + ccAb.y) * so.clearCoat;
    // Coat darkening on the through-coat base ambient (same helper as the direct lobe so the
    // two paths can't drift); the coat reflection ccSpec stays unscaled.
    vec3 coatDarken = GE_CoatDarkening(so.baseColor, so.clearCoatIor, so.clearCoat, so.coatDarkening);
#ifdef GE_SSSR_NORMAL_ROUGHNESS
    ge_sssrSpecularWeight *= coatDarken * so.coatColor * (1.0 - ccFc);
#endif
    // coatColor tints the through-coat base ambient (coat-medium absorption; white = no tint), matching the direct lobe.
#ifdef GE_FUZZ_ENABLED
    // Over-coat ambient fuzz: layer it over the WHOLE coated ambient stack (mirrors the direct
    // path), unlike the ambient sheen above which is folded into baseAmbient UNDER the coat.
    vec3 coatedAmbient = baseAmbient * coatDarken * so.coatColor * (1.0 - ccFc) + ccSpec * specAO;
    return GE_LayerAmbientFuzzOverCoat(coatedAmbient, so.fuzzColor, so.fuzzRoughness, irr, NdotV, so.ao);
#else
    return baseAmbient * coatDarken * so.coatColor * (1.0 - ccFc) + ccSpec * specAO; // AO already applied per-term above
#endif
#else
  #ifdef GE_FUZZ_ENABLED
    // No coat: fuzz lays over the base ambient (which already carries any under-coat sheen).
    return GE_LayerAmbientFuzzOverCoat(baseAmbient, so.fuzzColor, so.fuzzRoughness, irr, NdotV, so.ao);
  #else
    return baseAmbient; // AO already applied per-term above
  #endif
#endif
}

#endif // GE_IBL_GLSL
