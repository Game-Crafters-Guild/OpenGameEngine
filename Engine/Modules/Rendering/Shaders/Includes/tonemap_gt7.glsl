// Gran Turismo 7 tone mapping operator, ported to GLSL from Polyphony Digital's
// reference implementation published with "Driving Toward Reality: Physically
// Based Tone Mapping and Perceptual Fidelity in Gran Turismo 7" (SIGGRAPH 2025).
// https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp
//
// MIT License
// Copyright (c) 2025 Polyphony Digital Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef GE_TONEMAP_GT7_GLSL
#define GE_TONEMAP_GT7_GLSL

// The published operator consumes linear Rec.2020. GameEngine's scene-linear
// working space is Rec.709, so convert at the boundary and return Rec.709.
const mat3 kGT7Rec709ToRec2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950);
const mat3 kGT7Rec2020ToRec709 = mat3(
     1.6604910, -0.1245500, -0.0181510,
    -0.5876410,  1.1329000, -0.1005790,
    -0.0728500, -0.0083490,  1.1187300);

float GT7InverseEotfSt2084(float value)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    // The reference convention is framebuffer 1.0 == 100 cd/m^2.
    float y = max(value, 0.0) * 0.01;
    float ym = pow(y, m1);
    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0 + c3 * ym)));
}

float GT7EotfSt2084(float signal)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float np = pow(clamp(signal, 0.0, 1.0), 1.0 / m2);
    float luminance = pow(max(np - c1, 0.0) / max(c2 - c3 * np, 1e-7), 1.0 / m1);
    return luminance * 100.0;
}

vec3 GT7RgbToICtCp(vec3 rgb)
{
    vec3 lms = vec3(
        dot(rgb, vec3(1688.0, 2146.0, 262.0)) / 4096.0,
        dot(rgb, vec3(683.0, 2951.0, 462.0)) / 4096.0,
        dot(rgb, vec3(99.0, 309.0, 3688.0)) / 4096.0);
    vec3 pq = vec3(GT7InverseEotfSt2084(lms.x),
                   GT7InverseEotfSt2084(lms.y),
                   GT7InverseEotfSt2084(lms.z));
    return vec3(
        dot(pq, vec3(2048.0, 2048.0, 0.0)) / 4096.0,
        dot(pq, vec3(6610.0, -13613.0, 7003.0)) / 4096.0,
        dot(pq, vec3(17933.0, -17390.0, -543.0)) / 4096.0);
}

vec3 GT7ICtCpToRgb(vec3 ictcp)
{
    vec3 pqLms = vec3(
        ictcp.x + 0.00860904 * ictcp.y + 0.11103 * ictcp.z,
        ictcp.x - 0.00860904 * ictcp.y - 0.11103 * ictcp.z,
        ictcp.x + 0.560031 * ictcp.y - 0.320627 * ictcp.z);
    vec3 lms = vec3(GT7EotfSt2084(pqLms.x),
                    GT7EotfSt2084(pqLms.y),
                    GT7EotfSt2084(pqLms.z));
    return max(vec3(
        dot(lms, vec3(3.43661, -2.50645, 0.0698454)),
        dot(lms, vec3(-0.79133, 1.9836, -0.192271)),
        dot(lms, vec3(-0.0259499, -0.0989137, 1.12486))), vec3(0.0));
}

float GT7Curve(float x)
{
    // SDR initialization from the reference: a 250 nit target, with 1.0 scene
    // linear representing 100 nits. These are the precomputed curve constants.
    const float peak = 2.5;
    const float midpoint = 0.538;
    const float linearSection = 0.444;
    const float toeStrength = 1.280;
    const float kA = 2.963333333333334;
    const float kB = -3.3733512380644313;
    const float kC = -0.539568345323741;
    x = max(x, 0.0);
    float shoulder = kA + kB * exp(x * kC);
    if (x >= linearSection * peak)
        return shoulder;
    float weightLinear = smoothstep(0.0, midpoint, x);
    float toeMapped = midpoint * pow(x / midpoint, toeStrength);
    return mix(toeMapped, x, weightLinear);
}

vec3 TonemapGranTurismo7(vec3 sceneRec709)
{
    vec3 rgb = kGT7Rec709ToRec2020 * max(sceneRec709, vec3(0.0));
    vec3 sourceICtCp = GT7RgbToICtCp(rgb);

    vec3 skewedRgb = vec3(GT7Curve(rgb.r), GT7Curve(rgb.g), GT7Curve(rgb.b));
    vec3 skewedICtCp = GT7RgbToICtCp(skewedRgb);

    // ICtCp intensity for neutral 2.5 (the 250 nit SDR target).
    const float targetIntensity = 0.6025591549907509;
    float chromaScale = 1.0 - smoothstep(0.98, 1.16,
                                        sourceICtCp.x / targetIntensity);
    vec3 scaledICtCp = vec3(skewedICtCp.x,
                            sourceICtCp.yz * chromaScale);
    vec3 scaledRgb = GT7ICtCpToRgb(scaledICtCp);

    // Reference blend ratio is 0.6. The 0.4 correction converts its 250 nit
    // SDR convention back to display-linear [0,1] for the terminal sRGB OETF.
    vec3 mappedRec2020 = min(mix(skewedRgb, scaledRgb, 0.6), vec3(2.5)) * 0.4;
    return clamp(kGT7Rec2020ToRec709 * mappedRec2020, 0.0, 1.0);
}

#endif
