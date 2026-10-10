// @category Sampling
// @version 1

// @sgnode SampleTexture2D
// @display "Sample Texture 2D"
// @param tex "Texture" hint=texture2d
// @param uv "UV" hint=uv default=vec2(0)
// @out rgba RGBA
// @out rgb RGB
// @out r R
// @out g G
// @out b B
// @out a A
// @stage fragment
void SG_SampleTexture2D(sampler2D tex, vec2 uv,
                        out vec4 rgba, out vec3 rgb, out float r, out float g, out float b, out float a)
{
    rgba = texture(tex, uv);
    rgb = rgba.rgb;
    r = rgba.r; g = rgba.g; b = rgba.b; a = rgba.a;
}

#ifdef GE_MATERIAL_TEXTURE_DEFINED
// Material-slot overloads: the graph compiler splices slot aliases (albedoMap,
// GE_USER_TEXTURE names) into tex pins, and under the adapter those expand to
// GE_MaterialTexture. Routing through the adapter's texture()/textureLod()
// overloads keeps the per-view TAAU mip bias on graph samples too.
void SG_SampleTexture2D(GE_MaterialTexture tex, vec2 uv,
                        out vec4 rgba, out vec3 rgb, out float r, out float g, out float b, out float a)
{
    rgba = texture(tex, uv);
    rgb = rgba.rgb;
    r = rgba.r; g = rgba.g; b = rgba.b; a = rgba.a;
}
#endif

// @sgnode SampleTexture2DLOD
// @display "Sample Texture 2D LOD"
// @param tex "Texture" hint=texture2d
// @param uv "UV" hint=uv
// @param lod "LOD" default=0
// @out rgb RGB
// @stage any
void SG_SampleTexture2DLOD(sampler2D tex, vec2 uv, float lod, out vec3 rgb)
{
    rgb = textureLod(tex, uv, lod).rgb;
}

#ifdef GE_MATERIAL_TEXTURE_DEFINED
void SG_SampleTexture2DLOD(GE_MaterialTexture tex, vec2 uv, float lod, out vec3 rgb)
{
    rgb = textureLod(tex, uv, lod).rgb;
}
#endif

// @sgnode SampleTextureArray
// @display "Sample Texture Array"
// @param tex "Texture" hint=texture2d
// @param uv "UV" hint=uv
// @param layer "Layer" default=0
// @out color Color
// @stage fragment
vec4 SG_SampleTextureArray(sampler2DArray tex, vec2 uv, float layer)
{
    return texture(tex, vec3(uv, layer));
}

// @sgnode SampleCubemap
// @display "Sample Cubemap"
// @param tex "Texture" hint=texture2d
// @param direction "Direction" hint=normal
// @out color Color
// @stage fragment
vec4 SG_SampleCubemap(samplerCube tex, vec3 direction)
{
    return texture(tex, normalize(direction));
}

// @sgnode TriplanarWeights
// @display "Triplanar Weights"
// @param normalWS "Normal (WS)" hint=normal
// @param sharpness "Sharpness" default=4 range=0,16
// @out weights Weights
// @pure
vec3 SG_TriplanarWeights(vec3 normalWS, float sharpness)
{
    vec3 n = pow(abs(normalWS), vec3(sharpness));
    return n / max(n.x + n.y + n.z, 1e-5);
}

// @sgnode TriplanarSample
// @display "Triplanar Sample"
// @param tex "Texture" hint=texture2d
// @param positionWS "Position (WS)" hint=position
// @param weights "Weights"
// @param tiling "Tiling" default=1
// @out rgb RGB
// @stage fragment
void SG_TriplanarSample(sampler2D tex, vec3 positionWS, vec3 weights, float tiling, out vec3 rgb)
{
    vec2 uvX = positionWS.zy * tiling;
    vec2 uvY = positionWS.xz * tiling;
    vec2 uvZ = positionWS.xy * tiling;
    vec3 cX = texture(tex, uvX).rgb;
    vec3 cY = texture(tex, uvY).rgb;
    vec3 cZ = texture(tex, uvZ).rgb;
    rgb = cX * weights.x + cY * weights.y + cZ * weights.z;
}

#ifdef GE_MATERIAL_TEXTURE_DEFINED
void SG_TriplanarSample(GE_MaterialTexture tex, vec3 positionWS, vec3 weights, float tiling, out vec3 rgb)
{
    vec2 uvX = positionWS.zy * tiling;
    vec2 uvY = positionWS.xz * tiling;
    vec2 uvZ = positionWS.xy * tiling;
    vec3 cX = texture(tex, uvX).rgb;
    vec3 cY = texture(tex, uvY).rgb;
    vec3 cZ = texture(tex, uvZ).rgb;
    rgb = cX * weights.x + cY * weights.y + cZ * weights.z;
}
#endif

// @sgnode CurveTexture
// @display "Curve Texture"
// @param tex "Texture" hint=texture2d
// @param value "Value" default=0
// @param row "Row" default=0.5
// @out color Color
// @stage fragment
vec4 SG_CurveTexture(sampler2D tex, float value, float row)
{
    return texture(tex, vec2(clamp(value, 0.0, 1.0), row));
}

#ifdef GE_MATERIAL_TEXTURE_DEFINED
vec4 SG_CurveTexture(GE_MaterialTexture tex, float value, float row)
{
    return texture(tex, vec2(clamp(value, 0.0, 1.0), row));
}
#endif
