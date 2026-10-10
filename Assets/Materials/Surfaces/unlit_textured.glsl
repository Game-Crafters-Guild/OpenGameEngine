// Surface shader: Unlit textured.
// Samples albedoMap and multiplies by MaterialUBO baseColor.
// Use with lightingModel: "unlit" for UI elements, billboards, or debug views.

// The adapter defines texture aliases for the material texture table.
#ifndef GE_MATERIAL_TEXTURE_DEFINED
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
#endif

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec4 tex = texture(albedoMap, sIn.uv0);
    o.baseColor = tex.rgb * Mat.uBaseColor.rgb;
    o.opacity = tex.a * Mat.uBaseColor.a;
    o.normalWS = normalize(sIn.normalWS);
    return o;
}
