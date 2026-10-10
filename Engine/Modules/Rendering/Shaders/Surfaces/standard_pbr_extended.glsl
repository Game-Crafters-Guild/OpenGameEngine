// Surface shader: Standard PBR Extended (unified) with AO.
// Includes: albedo, normal, metallic-roughness (combined OR separate), emissive, AO.
// coatNormalMap (binding 6) perturbs the clear-coat lobe only; sampled solely under
// GE_COAT_NORMAL_ENABLED (off by default the flat-normal default leaves the coat unperturbed).
// When no texture is assigned, 1x1 default textures yield identity values.
// Texture names (albedoMap, normalMap, …) are provided by the adapter's
// bindless slot macros.

// The emission's exposure weight (uParams19.w, emissiveExposureWeight): the forward adapter shows the
// emission as emission x exposure^weight, so 1 is physical and 0 shows it at its authored brightness
// whatever the view's exposure (Adapters/adapter_forward.glsl).
#define GE_SURFACE_EMISSIVE_EXPOSURE_WEIGHT Mat.uParams19.w

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Primary UV
    vec2 uv = sIn.uv0;

    vec2 uvAlbedoA, uvAlbedoB, uvMRA, uvMRB, uvRoughA, uvRoughB, uvMetalA, uvMetalB;
    vec2 uvAOA, uvAOB, uvEmissiveA, uvEmissiveB, uvNormalA, uvNormalB;
    float blendAlbedo = 0.0, blendMR = 0.0, blendRough = 0.0, blendMetal = 0.0;
    float blendAO = 0.0, blendEmissive = 0.0, blendNormal = 0.0;
    GE_ComputeHexBlendUV(uv, sIn.textureST[0], sIn.textureST2[0], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvAlbedoA, uvAlbedoB, blendAlbedo);
    GE_ComputeHexBlendUV(uv, sIn.textureST[2], sIn.textureST2[2], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvMRA, uvMRB, blendMR);
    GE_ComputeHexBlendUV(uv, sIn.textureST[6], sIn.textureST2[6], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvRoughA, uvRoughB, blendRough);
    GE_ComputeHexBlendUV(uv, sIn.textureST[7], sIn.textureST2[7], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvMetalA, uvMetalB, blendMetal);
    GE_ComputeHexBlendUV(uv, sIn.textureST[4], sIn.textureST2[4], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvAOA, uvAOB, blendAO);
    GE_ComputeHexBlendUV(uv, sIn.textureST[3], sIn.textureST2[3], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvEmissiveA, uvEmissiveB, blendEmissive);
    GE_ComputeHexBlendUV(uv, sIn.textureST[1], sIn.textureST2[1], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvNormalA, uvNormalB, blendNormal);

    // Albedo: texture * UBO tint
    vec4 albedo = mix(texture(albedoMap, uvAlbedoA), texture(albedoMap, uvAlbedoB), blendAlbedo);
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb;
    o.opacity = albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a;

    // Separate roughness/metallic maps. Defaults are white roughness and
    // black metallic, so unassigned slots preserve the material scalar values.
    o.roughness = mix(texture(roughnessMap, uvRoughA).r, texture(roughnessMap, uvRoughB).r, blendRough);
    o.metallic = mix(texture(metallicMap, uvMetalA).r, texture(metallicMap, uvMetalB).r, blendMetal);

    // Apply material UBO modifiers
    o.roughness *= Mat.uParams0.y;
    o.metallic *= Mat.uParams0.x;

    // OpenPBR base specular control: F0 from IOR, tinted + weighted (always-on;
    // neutral defaults white/1/1.5 reproduce the legacy fixed F0=0.04 dielectric).
    o.specularColor    = Mat.uParams13.rgb;
    o.specularWeight   = Mat.uParams13.w;
    o.specularIor      = Mat.uParams14.x;
    o.diffuseRoughness = Mat.uParams14.z; // OpenPBR diffuse roughness (Oren-Nayar; 0 = Lambert)

    // Ambient Occlusion (R channel)
    o.ao = mix(texture(aoMap, uvAOA).r, texture(aoMap, uvAOB).r, blendAO);

    // Emissive — authored in nits: texture x colour x luminance, scaled into scene-linear by the
    // reference-white anchor (an unbound texture samples white; luminance 0 emits nothing).
    vec3 emissive = mix(texture(emissiveMap, uvEmissiveA).rgb, texture(emissiveMap, uvEmissiveB).rgb, blendEmissive);
    o.emissive = emissive * Mat.uParams18.rgb * (Mat.uParams18.w * (1.0 / GE_EMISSION_PAPERWHITE_NITS));

    // Normal mapping (Z reconstructed — see GE_DecodeTangentNormal)
#ifdef HAS_TANGENT
    vec3 tn = mix(GE_DecodeTangentNormal(texture(normalMap, uvNormalA)),
                  GE_DecodeTangentNormal(texture(normalMap, uvNormalB)), blendNormal);
    o.normalWS = normalize(sIn.TBN * tn);
#else
    o.normalWS = normalize(sIn.normalWS);
#endif

    // Clear-coat normal mapping (slot 5). The coat lobe shades about its own normal so a
    // coat can show orange-peel / brushed-lacquer microstructure independent of the base
    // surface. Without a tangent frame (or the keyword) the coat reuses the base normal, so
    // the swap is a pure no-op there.
#ifdef GE_COAT_NORMAL_ENABLED
  #ifdef HAS_TANGENT
    vec2 uvCoatNormalA, uvCoatNormalB;
    float blendCoatNormal = 0.0;
    GE_ComputeHexBlendUV(uv, sIn.textureST[5], sIn.textureST2[5], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvCoatNormalA, uvCoatNormalB, blendCoatNormal);
    vec3 ctn = mix(GE_DecodeTangentNormal(texture(coatNormalMap, uvCoatNormalA)),
                   GE_DecodeTangentNormal(texture(coatNormalMap, uvCoatNormalB)), blendCoatNormal);
    o.coatNormalWS = normalize(sIn.TBN * ctn);
  #else
    o.coatNormalWS = o.normalWS;
  #endif
#else
    o.coatNormalWS = o.normalWS;
#endif

#ifdef GE_CLEARCOAT_ENABLED
    o.clearCoat = Mat.uParams9.x;
    o.clearCoatRoughness = Mat.uParams9.y; // GE_MIN_GGX_ALPHA floors the lobe; the full 0..1 slider is live
    o.clearCoatIor = Mat.uParams14.y;      // OpenPBR coat IOR; neutral 1.5 -> F0 0.04
    o.coatDarkening = Mat.uParams9.z;      // wet-look darkening weight (0 = inert)
    o.coatColor = Mat.uParams19.rgb;       // OpenPBR coat medium tint (white = colourless)
#endif

#ifdef GE_SHEEN_ENABLED
    o.sheenColor     = Mat.uParams10.rgb;
    o.sheenRoughness = Mat.uParams10.w; // floored in the BRDF (GE_MIN_SHEEN_ROUGHNESS)
#endif

#ifdef GE_ANISOTROPY_ENABLED
    o.anisotropy = Mat.uParams11.x; // signed [-1..1]; 0 = isotropic
    o.anisotropyRotation = Mat.uParams11.y; // radians; revolves the tangent frame in-plane (0 = aligned with the geometric tangent)
    #ifdef HAS_TANGENT
    o.tangentWS   = sIn.TBN[0]; // T (normalized in the adapter)
    o.bitangentWS = sIn.TBN[1]; // B = cross(N,T)*handedness, sign folded in by the adapter
    #endif
#endif

#ifdef GE_SUBSURFACE_ENABLED
    o.subsurfaceColor = Mat.uParams12.rgb;
    o.thickness       = Mat.uParams12.w;
#endif

#ifdef GE_TRANSMISSION_ENABLED
    o.transmissionColor  = Mat.uParams15.rgb;
    o.transmissionWeight = Mat.uParams15.w;
    o.refractionDistance = Mat.uParams16.x; // world-unit path length for the thick (two-surface) lens
    o.attenuationColor    = Mat.uParams21.rgb; // Beer–Lambert volume colour (thick path only)
    o.attenuationDistance = Mat.uParams21.w;   // reference distance in world units (0 = inert)
#endif

#ifdef GE_IRIDESCENCE_ENABLED
    o.thinFilmThickness = Mat.uParams17.x; // nm
    o.thinFilmIor       = Mat.uParams17.y;
    o.thinFilmWeight    = Mat.uParams17.z;
#endif

#ifdef GE_FUZZ_ENABLED
    o.fuzzColor     = Mat.uParams20.rgb;
    o.fuzzRoughness = Mat.uParams20.w; // floored in the BRDF (GE_MIN_SHEEN_ROUGHNESS)
#endif

    return o;
}
