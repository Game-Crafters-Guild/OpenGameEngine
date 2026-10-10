// Surface shader: Unlit (unified).
// Samples albedoMap through the adapter's slot alias; the declaration below
// routes it through the composer's named-slot resolver (canonical ordinal 0).
// When no texture is assigned, the 1x1 default white texture yields (1,1,1,1)
// so the material baseColor passes through.

// @texture albedoMap srgb

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec4 albedo = texture(albedoMap, sIn.uv0);
    o.baseColor = albedo.rgb * Mat.uBaseColor.rgb;
    o.opacity = albedo.a * Mat.uBaseColor.a;
    o.normalWS = normalize(sIn.normalWS);
    return o;
}
