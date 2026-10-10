// Surface shader: Textured PBR.
// Samples albedoMap, normalMap, and metallicRoughnessMap.
// Multiplies texture values by MaterialUBO parameters for artist control.

// The adapter defines texture aliases for the material texture table.
#ifndef GE_MATERIAL_TEXTURE_DEFINED
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessMap;
#endif

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Albedo
    vec4 albedo = texture(albedoMap, sIn.uv0);
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb * sIn.vertexColor.rgb;
    o.opacity = albedo.a * Mat.uBaseColor.a * sIn.vertexColor.a;

    // Metallic-roughness (glTF convention: G=roughness, B=metallic)
    vec4 mr = texture(metallicRoughnessMap, sIn.uv0);
    o.roughness = mr.g * Mat.uParams0.y;
    o.metallic = mr.b * Mat.uParams0.x;

    // Normal mapping (Z reconstructed — see GE_DecodeTangentNormal)
#ifdef HAS_TANGENT
    vec3 tn = GE_DecodeTangentNormal(texture(normalMap, sIn.uv0));
    o.normalWS = normalize(sIn.TBN * tn);
#else
    o.normalWS = normalize(sIn.normalWS);
#endif

    return o;
}
