// Built-in fallback surface shader (M0).
// Real usage should point GE_SURFACE_SHADER_PATH at a user-authored shader in Assets/.

#ifndef GE_STANDARD_SURFACE_GLSL
#define GE_STANDARD_SURFACE_GLSL

// Requires: Includes/surface_io.glsl (included by the adapter)

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.baseColor = Mat.uBaseColor.rgb;
    o.opacity = Mat.uBaseColor.a;
    o.metallic = Mat.uParams0.x;
    o.roughness = Mat.uParams0.y;
    o.normalWS = normalize(sIn.normalWS);
    return o;
}

#endif // GE_STANDARD_SURFACE_GLSL
