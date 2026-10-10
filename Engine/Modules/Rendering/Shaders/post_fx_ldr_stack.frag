#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler3D uLut3d;
layout(set = 0, binding = 2) uniform sampler2D uLut1d;

layout(push_constant) uniform LdrFxPC {
    float casStrength;
    int casStackOrder;
    int lutStackOrder;
    float lutIntensity;
    int lutInputEncoding;
    int lutSize3d;
    int lutSize1d;
    int lutFlags;
    float lut1dInMin;
    float lut1dInMax;
    float lut3dInMin;
    float lut3dInMax;
    float vignetteIntensity;
    float vignetteSmoothness;
    int vignetteRounded;
    float vignetteColorR;
    float vignetteColorG;
    float vignetteColorB;
    int vignetteStackOrder;
    float shaderAnimationTime;
    float pad0;
} pc;

float Luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ApplyCas(vec3 c, vec2 texel)
{
    float w = clamp(pc.casStrength, 0.0, 1.0);
    if (w <= 1e-5)
        return c;

    vec3 n = texture(uSceneColor, vUV + vec2(0.0, -texel.y)).rgb;
    vec3 s = texture(uSceneColor, vUV + vec2(0.0,  texel.y)).rgb;
    vec3 e = texture(uSceneColor, vUV + vec2( texel.x, 0.0)).rgb;
    vec3 wv = texture(uSceneColor, vUV + vec2(-texel.x, 0.0)).rgb;
    vec3 ne = texture(uSceneColor, vUV + vec2( texel.x, -texel.y)).rgb;
    vec3 nw = texture(uSceneColor, vUV + vec2(-texel.x, -texel.y)).rgb;
    vec3 se = texture(uSceneColor, vUV + vec2( texel.x,  texel.y)).rgb;
    vec3 sw = texture(uSceneColor, vUV + vec2(-texel.x,  texel.y)).rgb;

    vec3 avgCardinal = (n + s + e + wv) * 0.25;
    vec3 avgDiagonal = (ne + nw + se + sw) * 0.25;
    vec3 avg = mix(avgCardinal, avgDiagonal, 0.35);
    vec3 detail = c - avg;

    float lC = Luma(c);
    float lN = Luma(n);
    float lS = Luma(s);
    float lE = Luma(e);
    float lW = Luma(wv);
    float lNE = Luma(ne);
    float lNW = Luma(nw);
    float lSE = Luma(se);
    float lSW = Luma(sw);
    float lMin = min(min(lC, min(min(lN, lS), min(lE, lW))), min(min(lNE, lNW), min(lSE, lSW)));
    float lMax = max(max(lC, max(max(lN, lS), max(lE, lW))), max(max(lNE, lNW), max(lSE, lSW)));
    float contrast = max(lMax - lMin, 1e-4);

    float adaptive = 1.0 - smoothstep(0.08, 0.40, contrast);
    float gain = w * mix(2.8, 0.75, 1.0 - adaptive);
    return max(c + detail * gain, vec3(0.0));
}

float LinearToSRGB1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.0031308) ? (12.92 * x) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float SRGBToLinear1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.04045) ? (x / 12.92) : pow((x + 0.055) / 1.055, 2.4);
}

vec3 LinearToSRGB(vec3 c)
{
    return vec3(LinearToSRGB1(c.r), LinearToSRGB1(c.g), LinearToSRGB1(c.b));
}

vec3 SRGBToLinear(vec3 c)
{
    return vec3(SRGBToLinear1(c.r), SRGBToLinear1(c.g), SRGBToLinear1(c.b));
}

vec3 Rec709ToAWG3(vec3 c)
{
    return vec3(
        dot(vec3(0.631320638, 0.270800406, 0.097878818), c),
        dot(vec3(0.036819917, 0.793037090, 0.170143018), c),
        dot(vec3(0.017369856, 0.148789076, 0.833840896), c));
}

vec3 AWG3ToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.617524436, -0.537286235, -0.080238194), c),
        dot(vec3(-0.070572684, 1.334612859, -0.264040156), c),
        dot(vec3(-0.021102017, -0.226953556, 1.248055903), c));
}

float Log10(float x)
{
    return log(x) / log(10.0);
}

vec3 Rec709ToDWG(vec3 c)
{
    return vec3(
        dot(vec3(0.562767460, 0.323516518, 0.113715973), c),
        dot(vec3(0.077754626, 0.749577398, 0.172668004), c),
        dot(vec3(0.064669185, 0.191998705, 0.743332142), c));
}

vec3 DWGToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.898614861, -0.792176192, -0.106438765), c),
        dot(vec3(-0.168948765, 1.488975755, -0.320026913), c),
        dot(vec3(-0.121539126, -0.315675802, 1.437214990), c));
}

vec3 Rec709ToAP1(vec3 c)
{
    return vec3(
        dot(vec3(0.613117813, 0.339538016, 0.047416696), c),
        dot(vec3(0.070193722, 0.916353879, 0.013452398), c),
        dot(vec3(0.020615592, 0.109569773, 0.869814635), c));
}

vec3 AP1ToRec709(vec3 c)
{
    return vec3(
        dot(vec3(1.704858676, -0.621716022, -0.083299372), c),
        dot(vec3(-0.130076824, 1.140735775, -0.010559802), c),
        dot(vec3(-0.023964073, -0.128975508, 1.153014019), c));
}

float LinearToLogC3_1(float x)
{
    x = max(x, 0.0);
    return (x > 0.010591) ? (0.247190 * Log10(5.555556 * x + 0.052272) + 0.385537)
                          : (5.367655 * x + 0.092809);
}

float LogC3ToLinear_1(float x)
{
    return (x > 0.149658) ? ((pow(10.0, (x - 0.385537) / 0.247190) - 0.052272) / 5.555556)
                          : ((x - 0.092809) / 5.367655);
}

vec3 LinearToLogC3(vec3 c)
{
    return vec3(LinearToLogC3_1(c.r), LinearToLogC3_1(c.g), LinearToLogC3_1(c.b));
}

vec3 LogC3ToLinear(vec3 c)
{
    return vec3(LogC3ToLinear_1(c.r), LogC3ToLinear_1(c.g), LogC3ToLinear_1(c.b));
}

float LinearToDaVinciIntermediate_1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.00262409) ? (x * 10.44426855) : ((log2(x + 0.0075) + 7.0) / 10.0);
}

float DaVinciIntermediateToLinear_1(float x)
{
    return (x <= 0.02740668) ? (x / 10.44426855) : (exp2(x * 10.0 - 7.0) - 0.0075);
}

vec3 LinearToDaVinciIntermediate(vec3 c)
{
    return vec3(LinearToDaVinciIntermediate_1(c.r), LinearToDaVinciIntermediate_1(c.g), LinearToDaVinciIntermediate_1(c.b));
}

vec3 DaVinciIntermediateToLinear(vec3 c)
{
    return vec3(DaVinciIntermediateToLinear_1(c.r), DaVinciIntermediateToLinear_1(c.g), DaVinciIntermediateToLinear_1(c.b));
}

float LinearToACEScct_1(float x)
{
    x = max(x, 0.0);
    return (x <= 0.0078125) ? (10.540237742 * x + 0.072905534)
                            : ((log2(x) + 9.72) / 17.52);
}

float ACEScctToLinear_1(float x)
{
    return (x <= 0.155251142) ? ((x - 0.072905534) / 10.540237742)
                              : exp2(x * 17.52 - 9.72);
}

vec3 LinearToACEScct(vec3 c)
{
    return vec3(LinearToACEScct_1(c.r), LinearToACEScct_1(c.g), LinearToACEScct_1(c.b));
}

vec3 ACEScctToLinear(vec3 c)
{
    return vec3(ACEScctToLinear_1(c.r), ACEScctToLinear_1(c.g), ACEScctToLinear_1(c.b));
}

float LinearToCineon_1(float x)
{
    const float refWhite = 685.0;
    const float refBlack = 95.0;
    const float densityPerCode = 0.002 / 0.6;
    float blackOffset = pow(10.0, (refBlack - refWhite) * densityPerCode);
    float v = max(x, 0.0) * (1.0 - blackOffset) + blackOffset;
    return (Log10(v) / densityPerCode + refWhite) / 1023.0;
}

float CineonToLinear_1(float x)
{
    const float refWhite = 685.0;
    const float refBlack = 95.0;
    const float densityPerCode = 0.002 / 0.6;
    float blackOffset = pow(10.0, (refBlack - refWhite) * densityPerCode);
    float v = pow(10.0, (x * 1023.0 - refWhite) * densityPerCode);
    return (v - blackOffset) / (1.0 - blackOffset);
}

vec3 LinearToCineon(vec3 c)
{
    return vec3(LinearToCineon_1(c.r), LinearToCineon_1(c.g), LinearToCineon_1(c.b));
}

vec3 CineonToLinear(vec3 c)
{
    return vec3(CineonToLinear_1(c.r), CineonToLinear_1(c.g), CineonToLinear_1(c.b));
}

vec3 EncodeLutInput(vec3 linearRec709)
{
    if (pc.lutInputEncoding == 1)
        return LinearToSRGB(linearRec709);
    if (pc.lutInputEncoding == 2)
        return LinearToLogC3(Rec709ToAWG3(linearRec709));
    if (pc.lutInputEncoding == 3)
        return LinearToDaVinciIntermediate(Rec709ToDWG(linearRec709));
    if (pc.lutInputEncoding == 4)
        return LinearToACEScct(Rec709ToAP1(linearRec709));
    if (pc.lutInputEncoding == 5)
        return LinearToCineon(linearRec709);
    return linearRec709;
}

vec3 DecodeLutOutput(vec3 lutRgb)
{
    if (pc.lutInputEncoding == 1)
        return SRGBToLinear(lutRgb);
    if (pc.lutInputEncoding == 2)
        return AWG3ToRec709(LogC3ToLinear(lutRgb));
    if (pc.lutInputEncoding == 3)
        return DWGToRec709(DaVinciIntermediateToLinear(lutRgb));
    if (pc.lutInputEncoding == 4)
        return AP1ToRec709(ACEScctToLinear(lutRgb));
    if (pc.lutInputEncoding == 5)
        return CineonToLinear(lutRgb);
    return lutRgb;
}

vec3 Apply1DStrip(vec3 lutRgb)
{
    int n = max(pc.lutSize1d, 2);
    float invR = 1.0 / max(pc.lut1dInMax - pc.lut1dInMin, 1e-6);
    vec3 t = clamp((lutRgb - vec3(pc.lut1dInMin)) * invR, 0.0, 1.0);
    float nf = float(n);
    vec2 uvR = vec2((t.r * (nf - 1.0) + 0.5) / nf, (0.0 + 0.5) / 3.0);
    vec2 uvG = vec2((t.g * (nf - 1.0) + 0.5) / nf, (1.0 + 0.5) / 3.0);
    vec2 uvB = vec2((t.b * (nf - 1.0) + 0.5) / nf, (2.0 + 0.5) / 3.0);
    float r = texture(uLut1d, uvR).r;
    float g = texture(uLut1d, uvG).r;
    float b = texture(uLut1d, uvB).r;
    return vec3(r, g, b);
}

vec3 Apply3DTable(vec3 lutRgb)
{
    int n = max(pc.lutSize3d, 2);
    float invR = 1.0 / max(pc.lut3dInMax - pc.lut3dInMin, 1e-6);
    vec3 t = clamp((lutRgb - vec3(pc.lut3dInMin)) * invR, 0.0, 1.0);
    vec3 coord = (t * (float(n) - 1.0) + 0.5) / float(n);
    return texture(uLut3d, coord).rgb;
}

vec3 ApplyCubeLut(vec3 linearRgb)
{
    float w = clamp(pc.lutIntensity, 0.0, 1.0);
    if (w <= 1e-5)
        return linearRgb;

    vec3 c = EncodeLutInput(linearRgb);
    int flags = pc.lutFlags;
    bool has1d = (flags & 2) != 0;
    bool has3d = (flags & 1) != 0;
    if (has1d)
        c = Apply1DStrip(c);
    if (has3d)
        c = Apply3DTable(c);
    c = DecodeLutOutput(c);
    return mix(linearRgb, max(c, vec3(0.0)), w);
}

// URP-exact procedural vignette (UberPost.hlsl), applied on display-referred LDR color.
vec3 ApplyVignette(vec3 c)
{
    float w = clamp(pc.vignetteIntensity, 0.0, 1.0);
    if (w <= 1e-5)
        return c;

    vec2 sz = vec2(textureSize(uSceneColor, 0));
    float aspect = sz.x / max(sz.y, 1.0);
    vec2 d = abs(vUV - vec2(0.5)) * (w * 3.0);
    d.x *= (pc.vignetteRounded != 0) ? aspect : 1.0;
    float vf = pow(clamp(1.0 - dot(d, d), 0.0, 1.0), clamp(pc.vignetteSmoothness, 0.0, 1.0) * 5.0);
    vec3 vigColor = vec3(pc.vignetteColorR, pc.vignetteColorG, pc.vignetteColorB);
    return c * mix(vigColor, vec3(1.0), vf);
}

void main()
{
    vec4 src = texture(uSceneColor, vUV);
    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));

    float casW = clamp(pc.casStrength, 0.0, 1.0);
    float lutW = clamp(pc.lutIntensity, 0.0, 1.0);
    float vigW = clamp(pc.vignetteIntensity, 0.0, 1.0);

    // Order the three reorderable LDR ops by stack order (stable insertion sort;
    // ties keep CAS < LUT < Vignette). At defaults (casStackOrder 1, others 0)
    // this yields LUT -> Vignette -> CAS: vignette after the grade, and the CAS/LUT
    // pair identical to the previous casFirst = casStackOrder <= lutStackOrder rule.
    int order[3] = int[](0, 1, 2); // 0 = CAS, 1 = LUT, 2 = Vignette
    int key[3] = int[](pc.casStackOrder, pc.lutStackOrder, pc.vignetteStackOrder);
    for (int i = 1; i < 3; ++i)
    {
        int j = i;
        while (j > 0 && key[order[j - 1]] > key[order[j]])
        {
            int t = order[j - 1];
            order[j - 1] = order[j];
            order[j] = t;
            --j;
        }
    }

    vec3 rgb = src.rgb;
    for (int i = 0; i < 3; ++i)
    {
        int op = order[i];
        if (op == 0 && casW > 1e-5)
            rgb = ApplyCas(rgb, texel);
        else if (op == 1 && lutW > 1e-5)
            rgb = ApplyCubeLut(rgb);
        else if (op == 2 && vigW > 1e-5)
            rgb = ApplyVignette(rgb);
    }

    oColor = vec4(rgb, src.a);
}
