// Surface shader: Waterfall FALLS churn-ribbon FX (the white/cyan churn planes at the falls' lips).
//
// THIS FILE IS PROJECT/EXAMPLE-SIDE, NOT AN ENGINE BUILT-IN — the FALLS sibling of
// water_stylized.glsl (FLOW). Its parameters are the `@property` declarations below (Props.<name>),
// its clock the always-bound per-view time (Light.uTimeParams.y, the bounded scroll clock), and it samples THREE textures resolved by NAME:
// one engine well-known slot (albedoMap) plus two USER-DECLARED slots. That pair is the living
// demo of named-slot resolution: a project surface mints its own texture names in the tag block
// below and samples them via GE_USER_TEXTURE(name); the composer packs each user name into the
// lowest free slot and the .material binds them by the same name in its "textures" object.
// Nothing engine-side knows the word "waterfall". SELF-CONTAINED surface only: user names must
// never be mixed with raw-ordinal texture access (the composer rejects that combination).
//
// Source-graph ground truth (GeneratedFromGraph-Waterfall.shader, SurfaceDescriptionFunction):
//   panUV    = uv0 + 0.01 * _Speed.xy * time             (the graph's own 0.01 damping)
//   churn    = tex(_Texture_01, panUV)                   (panned ribbon color + alpha)
//   detail   = tex(_Texture_02, uv0)                     (static brightness modulation)
//   mask     = tex(_Texture_03, uv0)                     (static fade mask; only .a is read)
//   Emission = _Water_Color * churn * (_Brightness * detail)      -- UNCONDITIONAL
//   Alpha    = churn.a * mask.a
//   BaseColor = linear(0.5 grey), Metallic 0, Smoothness 0.5      -- graph CONSTANTS, not .mat props
//
// Blend: the material authors _SURFACE_TYPE_TRANSPARENT + _ALPHAPREMULTIPLY_ON with
// Blend One OneMinusSrcAlpha (URP preserve-specular premultiply). The port keeps that exact
// structure: the .material authors blend {One, OneMinusSrcAlpha}; this surface premultiplies
// the DIFFUSE base by alpha (URP's AlphaModulate touches only albedo), leaves specular and
// emission un-premultiplied, and writes alpha for the destination attenuation. Where the
// ribbon alpha thins, the emission still ADDS over the falls — the additive-looking churn
// glow — while dense ribbon cores also occlude the background. Exactly the source graph's math.
//
// Emission units: the product is scene-linear against the OpenPBR emission lobe's paperwhite
// anchor (1.0 == GE_EMISSION_PAPERWHITE_NITS == 203 nits), the same convention as
// water_stylized's emissionScale; the authored _Brightness carries 1:1.
//
// Declared properties (authored by NAME in the .material, rendered by the inspector from these lines):
//   panSpeed is EFFECTIVE UV/sec: the converter folds the graph's 0.01 damping into the
//   authored _Speed and negates V (the FBX importer's V flip inverts apparent V motion;
//   U is untouched). FALLS ships _Speed = (20, 0) -> (0.2, 0): a pure U pan along the rim.
// @property vec2  panSpeed   "Pan Speed"   default=0.2,0 tooltip="effective UV per second"
// @property float brightness "Brightness"  default=1 range=0,8 tooltip="_Brightness, scene-linear (1.0 == 203-nit paperwhite)"
// @property color waterColor "Water Color" default=1,1,1 tooltip="_Water_Color, linearized"
//
// Declared texture set. The tag must lead its comment line; these three lines ARE the
// declarations (albedoMap = _Texture_01: the well-known name keeps canonical slot 0 and its
// ST row; churnDetail = _Texture_02 and fadeMask = _Texture_03 are user names — the composer
// assigns their slots, so they are sampled ONLY via GE_USER_TEXTURE, never by ordinal).
// @texture albedoMap   srgb
// @texture churnDetail srgb
// @texture fadeMask    srgb

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    float elapsed    = Light.uTimeParams.y; // bounded scroll clock (wraps per tile period; keeps UV precision on long sessions)
    vec2  panSpeed   = Props.panSpeed;      // effective UV/sec (damping folded by the converter)
    float brightness = Props.brightness;    // _Brightness, scene-linear (1.0 == 203-nit paperwhite)
    vec3  waterColor = Props.waterColor;    // _Water_Color, linearized

    // Panner: slot-0 ST first (authored identity for FALLS), then the graph's offset pan.
    vec2 uvBase = GE_TransformUV(sIn.uv0, sIn.textureST[0], sIn.textureST2[0]);
    vec2 uvPan  = uvBase + panSpeed * elapsed;

    vec4 churn  = texture(albedoMap, uvPan);                      // _Texture_01, panned
    vec4 detail = texture(GE_USER_TEXTURE(churnDetail), sIn.uv0); // _Texture_02 (graph: NoScaleOffset, raw uv0)
    vec4 mask   = texture(GE_USER_TEXTURE(fadeMask), sIn.uv0);    // _Texture_03 (only .a is read)

    float alpha = churn.a * mask.a;

    // Graph constants (deliberately NOT .mat properties — the .mat's _Metallic/_Smoothness
    // floats are stale legacy values the graph never reads): display-sRGB 0.5 grey in linear,
    // dielectric, Smoothness 0.5. The grey is premultiplied by alpha = URP's
    // _ALPHAPREMULTIPLY_ON AlphaModulate (diffuse only; the dielectric F0 does not derive
    // from baseColor, so the specular lobes stay preserved like _BlendModePreserveSpecular=1).
    const vec3 kBaseGreyLinear = vec3(0.2140411);
    o.baseColor = kBaseGreyLinear * alpha;
    o.opacity   = alpha;
    o.metallic  = 0.0;
    o.roughness = 0.5;

    // The graph outputs the identity tangent normal — geometric normal, no normal map. It also
    // never samples vertex color; the converter masks the planes' baked colors with
    // ignoreVertexColor rather than this surface multiplying an identity in.
    o.normalWS = normalize(sIn.normalWS);
    o.coatNormalWS = o.normalWS;

    // The churn glow: UNCONDITIONAL emission, not alpha-scaled (URP adds emission after
    // AlphaModulate), so thin-alpha ribbon edges still add light over the falls.
    o.emissive = waterColor * churn.rgb * (brightness * detail.rgb);

    return o;
}
