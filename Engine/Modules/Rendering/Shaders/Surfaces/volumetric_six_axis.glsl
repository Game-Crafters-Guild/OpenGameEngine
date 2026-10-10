// Surface shader: six-axis (dual half-space) volumetric-style lighting from three 2D maps.
//
// Texture slots (standard material bindings):
//   albedoMap              — baked +X, +Y, +Z (one scalar per axis, RGB).
//   normalMap              — baked −X, −Y, −Z (RGB).
//   metallicRoughnessMap   — R=density/coverage, G=emissive luma, A=baked AO (linear).
//
// Material params:
//   uParams1 — sixAxisScatter, sixAxisTeaOcclusionBlend, sixAxisEmissiveScale, sixAxisDensityScale
//   uParams2 — flipbookColumns, flipbookRows, flipbookFps, flipbookStartFrame (set fps>0 to animate)
//
// uParams0 metallic/roughness are unused here (kept for the shared param layout with PBR materials).
//
// Lighting model: Unlit. Uses LightUBO for sun direction, ambient weighting, and Light.uTimeParams.x
// for flipbook time.
//
// Texture names (albedoMap, normalMap, metallicRoughnessMap) are provided by
// the adapter's bindless slot macros.

vec2 SixAxisFlipbookUv(vec2 uv01, float timeSeconds, vec4 flipbook)
{
    float cols = max(flipbook.x, 1.0);
    float rows = max(flipbook.y, 1.0);
    float fps = flipbook.z;
    float start = flipbook.w;
    float cellCount = cols * rows;
    if (fps <= 0.0 || cellCount <= 1.0)
        return uv01;

    float tsum = floor(timeSeconds * fps) + start;
    float frame = tsum - floor(tsum / cellCount) * cellCount;
    float col = frame - floor(frame / cols) * cols;
    float row = floor(frame / cols);
    vec2 cellSize = vec2(1.0 / cols, 1.0 / rows);
    vec2 origin = vec2(col, row) * cellSize;
    return origin + uv01 * cellSize;
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    vec2 uvLocal = SixAxisFlipbookUv(sIn.uv0, Light.uTimeParams.x, Mat.uParams2);

    vec2 uvPosA, uvPosB, uvNegA, uvNegB, uvTeaA, uvTeaB;
    float blendPos = 0.0, blendNeg = 0.0, blendTea = 0.0;
    GE_ComputeHexBlendUV(uvLocal, sIn.textureST[0], sIn.textureST2[0], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvPosA, uvPosB, blendPos);
    GE_ComputeHexBlendUV(uvLocal, sIn.textureST[1], sIn.textureST2[1], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvNegA, uvNegB, blendNeg);
    GE_ComputeHexBlendUV(uvLocal, sIn.textureST[2], sIn.textureST2[2], Mat.uParams0.z, Mat.uParams0.w, Mat.uParams3.x, uvTeaA, uvTeaB, blendTea);

    vec3 positiveAxes = mix(texture(albedoMap, uvPosA).rgb, texture(albedoMap, uvPosB).rgb, blendPos);
    vec3 negativeAxes = mix(texture(normalMap, uvNegA).rgb, texture(normalMap, uvNegB).rgb, blendNeg);
    vec4 tea = mix(texture(metallicRoughnessMap, uvTeaA), texture(metallicRoughnessMap, uvTeaB), blendTea);

    vec3 L = normalize(-Light.uLightDirWorld.xyz);
    vec3 wPos = max(L, vec3(0.0));
    vec3 wNeg = max(-L, vec3(0.0));
    vec3 axisScatter = positiveAxes * wPos + negativeAxes * wNeg;

    // The sun's delivered light: colour times intensity, as every other light consumer reads it. A
    // sky that drives the sun carries its night in the colour, not in the intensity.
    vec3 directionalLight = Light.uLightColorWorld.rgb * max(Light.uLightDirWorld.w, 0.0);
    vec3 tint = Mat.uBaseColor.rgb * sIn.vertexColor.rgb;

    float scatter = Mat.uParams1.x;
    if (scatter <= 0.0)
        scatter = 1.0;

    o.baseColor = axisScatter * tint * scatter * directionalLight;

    float density = clamp(tea.r, 0.0, 1.0);
    float densityScale = Mat.uParams1.w;
    if (densityScale > 0.0)
        density = clamp(density * densityScale, 0.0, 1.0);
    o.opacity = density * Mat.uBaseColor.a * sIn.vertexColor.a;

    float aoBlend = clamp(Mat.uParams1.y, 0.0, 1.0);
    o.ao = mix(1.0, clamp(tea.a, 0.0, 1.0), aoBlend);

    float emissiveLuma = max(tea.g, 0.0) * max(Mat.uParams1.z, 0.0);
    o.emissive = vec3(emissiveLuma) * tint;

    o.normalWS = normalize(sIn.normalWS);
    return o;
}
