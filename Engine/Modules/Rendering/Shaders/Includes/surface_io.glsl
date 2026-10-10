// Shared surface shader IO for the composition pipeline.
//
// Surface shaders implement: `SurfaceOutput EvaluateSurface(SurfaceInput sIn);`
// Surface shaders should NOT declare descriptor sets; they only read material
// params via `Mat` (the shared MaterialData row) declared by the adapter.
//
// This struct is the contract between adapter shaders and user surface shaders.
// Fields are populated by the adapter from varyings; user code reads what it needs.

#ifndef GE_SURFACE_IO_GLSL
#define GE_SURFACE_IO_GLSL

// Emission authoring anchor: a surface emitting this many nits maps to scene-linear 1.0
// (BT.2408 reference white). Fixed so a material's authored nits read the same on SDR and
// HDR; the terminal tonemap/encode places scene-linear 1.0 at the actual display paperwhite.
const float GE_EMISSION_PAPERWHITE_NITS = 203.0;

// The relief march (Includes/parallax_occlusion.glsl) runs in the shading shapes of a Parallax
// material and in its camera prepass shape (GE_DEPTH_ONLY_FRAGMENT with GE_PARALLAX_DEPTH_OFFSET),
// which marches only to write the hit's depth. The other depth-only shapes, the motion and the
// glass-tint shapes decide coverage (or tint) on the polygon: a light cascade carries no camera view
// to march along. Coverage stays on the polygon in every shape, so under an alpha test each reads it
// at the undisplaced UV.
#if defined(GE_PARALLAX_ENABLED) && !defined(GE_MOTION_VECTORS) && !defined(GE_GLASS_SHADOW_COLOR) \
    && (!defined(GE_DEPTH_ONLY_FRAGMENT) || defined(GE_PARALLAX_DEPTH_OFFSET))
#define GE_PARALLAX_MARCH
#endif

// The relief's depth. Pass keyword ParallaxDepthOffset: a marching variant writes the depth of the
// relief hit along the view ray as its fragment depth. Pass keyword ParallaxDepthFromPrepass: the
// colour variant after a prepass that wrote it does not march; it rebuilds the hit from that depth.
#if defined(GE_PARALLAX_MARCH) && defined(GE_PARALLAX_DEPTH_OFFSET)
#define GE_PARALLAX_WRITES_DEPTH
#endif
#if defined(GE_PARALLAX_MARCH) && defined(GE_PARALLAX_DEPTH_FROM_PREPASS) && !defined(GE_DEPTH_ONLY_FRAGMENT)
#define GE_PARALLAX_READS_DEPTH
#endif
#if defined(GE_PARALLAX_WRITES_DEPTH) || defined(GE_PARALLAX_READS_DEPTH)
#define GE_PARALLAX_RELIEF_DEPTH
#endif

// The Parallax steps debug view (pass keyword ParallaxStepsView): a marching variant draws the
// height samples its march took as the hue of its lit colour.
#if defined(GE_PARALLAX_MARCH) && defined(GE_PARALLAX_STEPS_VIEW_ENABLED)
#define GE_PARALLAX_STEPS_VIEW
#endif

struct SurfaceInput
{
    vec2 uv0;           // primary UV
    vec3 normalWS;      // world-space normal (from vertex or normal map)
    vec3 positionWS;    // world-space position
    // Render-origin-relative world position (Earth-scale precision). Identical to
    // positionWS while the render origin is inactive (near-origin scenes); when active
    // it is the PRECISE small-magnitude position — the one coordinate world-anchored
    // texture tiling / procedural detail can derive from without fp32 ULP(|world|)
    // quantization (0.5 m at Earth radius) shredding UVs and their derivatives.
    vec3 positionRelWS;
    vec3 viewDirWS;     // from surface point toward camera

    // Extended fields (populated when the corresponding vertex attributes are present).
    // Adapters set these; surface shaders can read them conditionally.
    // When the attribute is missing, the adapter provides sensible defaults.
    vec2 uv1;           // secondary UV (default: vec2(0))
    vec2 uv2;
    vec2 uv3;
    vec2 uv4;
    vec2 uv5;
    vec2 uv6;
    vec2 uv7;
    vec4 vertexColor;   // vertex color (default: vec4(1))
    vec4 tangentWS;     // world-space tangent (xyz) + handedness (w) (default: vec4(0))
    mat3 TBN;           // tangent-bitangent-normal matrix (default: identity-like)
    vec2 screenUV;      // screen-space UV (default: vec2(0))
    float linearDepth;  // linear depth (default: 0)
    vec4 custom0;       // user-defined data from vertex/instance adapter (default: vec4(1))
#ifdef GE_USER_PARTICLE_BUFFER
    vec4 particleAnimation;
    mat3 particleBasis;
    float particleColorScale; // unlit colour and emission multiplier, 1/exposure (default: 1)
    float particleNearFade;   // a sprite's near-camera fade, also in custom0.a (default: 1)
#endif

    // Per-texture affine UV transform rows.
    vec4 textureST[8];
    vec4 textureST2[8];

#ifdef GE_PARALLAX_MARCH
    // The screen-space footprint the relief march solves its texture-space frame from, taken by
    // the adapter at the top of main() in uniform control flow (real derivatives even under the
    // compat profile, where GE_DFDX reads zero).
    vec4 uvFootprint;        // xy = d(uv0)/dx, zw = d(uv0)/dy: repeats per pixel
    vec3 positionFootprintX; // d(positionRelWS)/dx: metres per pixel
    vec3 positionFootprintY; // d(positionRelWS)/dy: metres per pixel
    // Unit, surface to the primary directional light, world space: the light the lighting shades as
    // its shadowed directional term (Light.uLightDirWorld, which the sky blends sun to moon). Zero
    // when that light delivers nothing, so the relief casts no shadow toward it.
    vec3 primaryLightDirectionWS;
#ifdef GE_PARALLAX_READS_DEPTH
    // Metres along the view ray from the polygon to the depth the camera prepass wrote at this pixel,
    // >= 0: the relief hit, unless something nearer stands inside the relief.
    float prepassDepthOffset;
    // A bound on the error of prepassDepthOffset, metres (GE_ParallaxOffsetError).
    float prepassDepthOffsetError;
#endif
#endif
};

struct SurfaceOutput
{
    // Core PBR
    vec3 baseColor;
    float metallic;
    float roughness;
    vec3 normalWS;
    vec3 coatNormalWS; // clear-coat shading normal; == normalWS unless a coat normal map perturbs it (GE_COAT_NORMAL_ENABLED)
    vec3 emissive;
    float opacity;

    // Extended (used by specific lighting models)
    float ao;
    vec3 bentNormalWS; // GTAO world-space bent normal (diffuse irradiance direction); == normalWS without GTAO
    float clearCoat;
    float clearCoatRoughness;
    float clearCoatIor;
    float coatDarkening; // OpenPBR coat_darkening (0..1): wet-look deepening of the through-coat base
    vec3 coatColor;      // OpenPBR coat_color: tints light passing through the coat medium (white = colourless)
    vec3 subsurfaceColor;
    float thickness;
    float anisotropy;
    float anisotropyRotation; // radians; revolves the tangent frame within the surface plane (brushed-metal grain that ignores the UV direction)
    vec3 sheenColor;
    float sheenRoughness;
    vec3 fuzzColor;       // OpenPBR fuzz: an additive Charlie lobe layered OVER the coat (outermost), unlike the under-coat sheenColor
    float fuzzRoughness;
    vec3 tangentWS;    // world-space tangent for anisotropy (zero = isotropic fallback)
    vec3 bitangentWS;  // world-space bitangent (handedness already folded in)

    // OpenPBR base specular (always-on, not a lobe): F0 from IOR, tinted + weighted.
    // Neutral defaults (weight 1, white, ior 1.5) reproduce the legacy fixed F0=0.04.
    float specularWeight;
    vec3 specularColor;
    float specularIor;

    // OpenPBR diffuse roughness (drives the Oren-Nayar lobe). 0 = smooth Lambert (default);
    // higher flattens the matte response (clay / chalk / unfinished wood). Decoupled from the
    // specular roughness above.
    float diffuseRoughness;

    // OpenPBR transmission (refractive dielectric / glass). transmissionWeight splits energy
    // out of the diffuse budget into a refracted environment sample; transmissionColor tints it.
    float transmissionWeight;
    vec3 transmissionColor;
    float refractionDistance; // world units; path length for the thick (two-surface) refraction lens

    // Thick-glass volume absorption (Beer–Lambert). Separate from the surface transmissionColor tint:
    // light deepens toward attenuationColor over attenuationDistance of in-glass travel, so a thick
    // core looks darker/more saturated than a thin edge. White colour or distance 0 = inert (no-op).
    vec3 attenuationColor;     // colour light becomes after attenuationDistance through the medium
    float attenuationDistance; // world units; reference distance for the absorption (0 = absorption off)

    // Thin-film iridescence (wavelength-dependent interference on the specular Fresnel).
    // thinFilmWeight 0 (default) = inert; thickness in nanometres, ior the film index.
    float thinFilmThickness; // nm
    float thinFilmIor;
    float thinFilmWeight;

    // Per-surface participation in the environment's lower-hemisphere ground
    // occlusion. Ordinary materials use 1; surfaces that provide their own
    // horizon/self-occlusion model (the unbounded ocean) use 0.
    float iblGroundDarkening;
#ifdef GE_USER_PARTICLE_LIT
    vec3 particlePositive;
    vec3 particleNegative;
    mat3 particleBasis;
#endif

#ifdef GE_PARALLAX_MARCH
    // How much of the primary directional light reaches the shaded point past the surface's own
    // relief, 0..1 (1 unoccluded). Both lighting paths multiply the primary light's direct term by
    // it, beside its cascade shadow; ambient, local lights and secondary directionals are untouched.
    float primaryLightOcclusion;
#endif
#ifdef GE_PARALLAX_STEPS_VIEW
    int parallaxHeightSamples; // height samples the relief march took (the Parallax steps view)
#endif
#ifdef GE_PARALLAX_WRITES_DEPTH
    // Metres along the view ray from the polygon to the shaded point, >= 0 (the relief is carved
    // below the polygon); the adapter writes the depth there.
    float depthOffset;
#endif
#if defined(GE_PARALLAX_WRITES_DEPTH) && defined(GE_PARALLAX_DEPTH_TOLERANCE)
    // Metres along the view ray one linear step of the march spans: the tolerant colour pass tests a
    // step nearer than its own hit.
    float depthOffsetStep;
#endif
#ifdef GE_PARALLAX_READS_DEPTH
    // The depth the prepass wrote at this pixel is not this surface's relief but something in front of
    // it, inside the relief (a prop sunk into it): the adapter discards the fragment.
    bool reliefHidden;
#endif
};

SurfaceOutput DefaultSurfaceOutput()
{
    SurfaceOutput o;
    o.baseColor = vec3(1.0);
    o.metallic = 0.0;
    o.roughness = 0.5;
    o.normalWS = vec3(0.0, 1.0, 0.0);
    o.coatNormalWS = vec3(0.0, 1.0, 0.0); // mirrors normalWS; the coat lobe reads it only under GE_COAT_NORMAL_ENABLED
    o.emissive = vec3(0.0);
    o.opacity = 1.0;
    o.ao = 1.0;
    o.bentNormalWS = vec3(0.0, 1.0, 0.0);
    o.clearCoat = 0.0;
    o.clearCoatRoughness = 0.0;
    o.clearCoatIor = 1.5;
    o.coatDarkening = 0.0; // inert: no wet-look darkening until the artist dials it
    o.coatColor = vec3(1.0); // colourless coat by default (no tint of the through-coat base)
    o.subsurfaceColor = vec3(0.0);
    o.thickness = 0.0;
    o.anisotropy = 0.0;
    o.anisotropyRotation = 0.0; // no rotation: the guarded rotate is skipped -> raw tangent frame -> byte-identical to the pre-rotation path
    o.sheenColor = vec3(0.0);
    o.sheenRoughness = 0.0;
    o.fuzzColor = vec3(0.0);
    o.fuzzRoughness = 0.0;
    o.tangentWS = vec3(0.0);
    o.bitangentWS = vec3(0.0);
    o.specularWeight = 1.0;
    o.specularColor = vec3(1.0);
    o.specularIor = 1.5;
    o.diffuseRoughness = 0.0;
    o.transmissionWeight = 0.0;
    o.transmissionColor = vec3(1.0);
    o.refractionDistance = 1.0;
    o.attenuationColor = vec3(1.0);  // white -> log(1)=0 -> no absorption
    o.attenuationDistance = 0.0;     // 0 -> the Beer–Lambert guard skips it
    o.thinFilmThickness = 0.0;
    o.thinFilmIor = 1.3;
    o.thinFilmWeight = 0.0;
    o.iblGroundDarkening = 1.0;
#ifdef GE_USER_PARTICLE_LIT
    o.particlePositive = vec3(1.0);
    o.particleNegative = vec3(1.0);
    o.particleBasis = mat3(1.0);
#endif
#ifdef GE_PARALLAX_MARCH
    o.primaryLightOcclusion = 1.0;
#endif
#ifdef GE_PARALLAX_STEPS_VIEW
    o.parallaxHeightSamples = 0;
#endif
#ifdef GE_PARALLAX_WRITES_DEPTH
    o.depthOffset = 0.0;
#endif
#if defined(GE_PARALLAX_WRITES_DEPTH) && defined(GE_PARALLAX_DEPTH_TOLERANCE)
    o.depthOffsetStep = 0.0;
#endif
#ifdef GE_PARALLAX_READS_DEPTH
    o.reliefHidden = false;
#endif
    return o;
}

SurfaceInput DefaultSurfaceInput()
{
    SurfaceInput si;
    si.uv0 = vec2(0.0);
    si.normalWS = vec3(0.0, 1.0, 0.0);
    si.positionWS = vec3(0.0);
    si.positionRelWS = vec3(0.0);
    si.viewDirWS = vec3(0.0, 0.0, 1.0);
    si.uv1 = vec2(0.0);
    si.uv2 = vec2(0.0);
    si.uv3 = vec2(0.0);
    si.uv4 = vec2(0.0);
    si.uv5 = vec2(0.0);
    si.uv6 = vec2(0.0);
    si.uv7 = vec2(0.0);
    si.vertexColor = vec4(1.0);
    si.tangentWS = vec4(0.0);
    si.TBN = mat3(1.0);
    si.screenUV = vec2(0.0);
    si.linearDepth = 0.0;
    si.custom0 = vec4(1.0);
#ifdef GE_USER_PARTICLE_BUFFER
    si.particleAnimation = vec4(0.0);
    si.particleBasis = mat3(1.0);
    si.particleColorScale = 1.0;
    si.particleNearFade = 1.0;
#endif
    for (int i = 0; i < 8; ++i)
    {
        si.textureST[i] = vec4(1.0, 0.0, 0.0, 0.0);
        si.textureST2[i] = vec4(0.0, 1.0, 0.0, 0.0);
    }
#ifdef GE_PARALLAX_MARCH
    si.uvFootprint = vec4(0.0);
    si.positionFootprintX = vec3(0.0);
    si.positionFootprintY = vec3(0.0);
    si.primaryLightDirectionWS = vec3(0.0);
#ifdef GE_PARALLAX_READS_DEPTH
    si.prepassDepthOffset = 0.0;
    si.prepassDepthOffsetError = 0.0;
#endif
#endif
    return si;
}

// Apply per-texture affine UV transform. st packs row 0, st2 packs row 1.
vec2 GE_TransformUV(vec2 uv, vec4 st, vec4 st2)
{
    return vec2(dot(vec3(uv, 1.0), st.xyz), dot(vec3(uv, 1.0), st2.xyz));
}

// Legacy helper for authored shaders that pass only scale/offset.
vec2 GE_TransformUV(vec2 uv, vec4 st) { return uv * st.xy + st.zw; }

float GE_Hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Decode a tangent-space normal-map sample. XY always come from the texture; Z
// is reconstructed from the unit-hemisphere constraint (z = sqrt(1 - x² - y²)),
// which makes the decode identical for RGB(A)8 normal maps (stored Z ignored)
// and the import cook's two-channel BC5 payloads (B samples as 0). Tangent-space
// normals live on the +Z hemisphere, so reconstruction is exact for unit
// normals; maps authored with intentionally non-unit normals (baked-in strength)
// are implicitly renormalized — residual documented in the texture-cook design.
//
// Written component-wise, with `f`-suffixed literals, because this block is lifted verbatim into the
// host tests (ExtractShaderBlock.cmake + GlslShim.h): the shim carries no swizzles and reads an
// unsuffixed literal as a double. Both the mesh and the terrain paths decode through here, so the
// terrain normal-sign tests measure the same expression standard_pbr does.
// GE_SHARED_DECODE_TANGENT_NORMAL_BEGIN
vec3 GE_DecodeTangentNormal(vec4 s)
{
    vec2 xy = vec2(s.x, s.y) * 2.0f - 1.0f;
    return vec3(xy, sqrt(max(1.0f - dot(xy, xy), 0.0f)));
}
// GE_SHARED_DECODE_TANGENT_NORMAL_END

// Hex-cell randomization support. Computes two rotated UVs and an opacity-style
// blend factor to mix texel samples across hex boundaries without UV stretching.
// hexBlend in [0..1] controls seam softness (0 = crisp, 1 = very blended).
// hexRotation in [0..1] controls random rotation amount (0 = off, 1 = full).
void GE_ComputeHexBlendUV(vec2 uv, vec4 st, vec4 st2, float hexEnabled, float hexBlend, float hexRotation,
                          out vec2 uvA, out vec2 uvB, out float blend)
{
    vec2 transformed = GE_TransformUV(uv, st, st2);
    if (hexEnabled < 0.5)
    {
        uvA = transformed;
        uvB = transformed;
        blend = 0.0;
        return;
    }

    const float kSqrt3 = 1.73205080757;

    // World-to-axial conversion for pointy-top hexes.
    float q = (kSqrt3 / 3.0) * transformed.x - (1.0 / 3.0) * transformed.y;
    float r = (2.0 / 3.0) * transformed.y;
    float s = -q - r;

    // Cube-rounding to nearest hex cell.
    vec3 rounded = floor(vec3(q, r, s) + 0.5);
    vec3 diff = abs(rounded - vec3(q, r, s));
    if (diff.x > diff.y && diff.x > diff.z)
        rounded.x = -rounded.y - rounded.z;
    else if (diff.y > diff.z)
        rounded.y = -rounded.x - rounded.z;
    else
        rounded.z = -rounded.x - rounded.y;

    vec2 baseHex = rounded.xy;

    // Find the two nearest hex centers (base + 6 neighbors), then blend their
    // randomized transforms to avoid hard seams at hex boundaries.
    const vec2 kHexNeighborOffsets[7] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(-1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(0.0, -1.0),
        vec2(1.0, -1.0),
        vec2(-1.0, 1.0)
    );

    vec2 nearestHex = baseHex;
    vec2 secondHex = baseHex;
    vec2 nearestCenter = vec2(0.0);
    vec2 secondCenter = vec2(0.0);
    float nearestDist2 = 1e20;
    float secondDist2 = 1e20;

    for (int i = 0; i < 7; ++i)
    {
        vec2 hex = baseHex + kHexNeighborOffsets[i];
        vec2 center = vec2(kSqrt3 * (hex.x + 0.5 * hex.y), 1.5 * hex.y);
        vec2 delta = transformed - center;
        float d2 = dot(delta, delta);
        if (d2 < nearestDist2)
        {
            secondDist2 = nearestDist2;
            secondCenter = nearestCenter;
            secondHex = nearestHex;
            nearestDist2 = d2;
            nearestCenter = center;
            nearestHex = hex;
        }
        else if (d2 < secondDist2)
        {
            secondDist2 = d2;
            secondCenter = center;
            secondHex = hex;
        }
    }

    float rotationAmount = clamp(hexRotation, 0.0, 1.0);
    float angle0 = GE_Hash12(nearestHex) * 6.28318530718 * rotationAmount;
    float c0 = cos(angle0);
    float s0 = sin(angle0);
    vec2 uv0 = nearestCenter + mat2(c0, -s0, s0, c0) * (transformed - nearestCenter);

    float angle1 = GE_Hash12(secondHex) * 6.28318530718 * rotationAmount;
    float c1 = cos(angle1);
    float s1 = sin(angle1);
    vec2 uv1 = secondCenter + mat2(c1, -s1, s1, c1) * (transformed - secondCenter);

    float d0 = sqrt(nearestDist2);
    float d1 = sqrt(secondDist2);
    float w0 = pow(max(1.0 - d0 / (d0 + d1 + 1e-5), 0.0), 2.0);
    float w1 = pow(max(1.0 - d1 / (d0 + d1 + 1e-5), 0.0), 2.0);
    float blendFactor = w1 / max(w0 + w1, 1e-5);
    float blendSoftness = clamp(hexBlend, 0.0, 1.0);
    if (blendSoftness <= 0.0)
    {
        blend = 0.0;
        uvA = uv0;
        uvB = uv1;
        return;
    }
    float lo = mix(0.45, 0.05, blendSoftness);
    float hi = mix(0.55, 0.95, blendSoftness);
    blend = smoothstep(lo, hi, blendFactor);
    uvA = uv0;
    uvB = uv1;
}

void GE_ComputeHexBlendUV(vec2 uv, vec4 st, float hexEnabled, float hexBlend, float hexRotation,
                          out vec2 uvA, out vec2 uvB, out float blend)
{
    GE_ComputeHexBlendUV(uv, st, vec4(0.0, 1.0, 0.0, 0.0),
                         hexEnabled, hexBlend, hexRotation, uvA, uvB, blend);
}

#endif // GE_SURFACE_IO_GLSL
