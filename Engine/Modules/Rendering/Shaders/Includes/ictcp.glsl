// ITU-R BT.2100-3 ICtCp helpers for PQ display-referred processing.
//
// ICtCp is a temporary perceptual processing space here, never an output
// encoding. Callers provide linear BT.2020 values in absolute nits, modify
// I/Ct/Cp, then convert back to linear RGB before the terminal HDR10 encode.

float GE_IctcpPqEncode(float normalizedLinear)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(clamp(normalizedLinear, 0.0, 1.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float GE_IctcpPqDecode(float pq)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(clamp(pq, 0.0, 1.0), 1.0 / m2);
    return pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-7), 1.0 / m1);
}

vec3 GE_Bt2020NitsToICtCp(vec3 bt2020Nits)
{
    // BT.2100 RGB -> LMS, transcribed column-major for GLSL.
    const mat3 rgbToLms = mat3(
        1688.0 / 4096.0,  683.0 / 4096.0,   99.0 / 4096.0,
        2146.0 / 4096.0, 2951.0 / 4096.0,  309.0 / 4096.0,
         262.0 / 4096.0,  462.0 / 4096.0, 3688.0 / 4096.0);
    vec3 lms = rgbToLms * clamp(bt2020Nits / 10000.0, vec3(0.0), vec3(1.0));
    vec3 lmsPq = vec3(GE_IctcpPqEncode(lms.r),
                      GE_IctcpPqEncode(lms.g),
                      GE_IctcpPqEncode(lms.b));

    // PQ LMS -> ICtCp, transcribed column-major for GLSL.
    const mat3 lmsToICtCp = mat3(
         2048.0 / 4096.0,   6610.0 / 4096.0,  17933.0 / 4096.0,
         2048.0 / 4096.0, -13613.0 / 4096.0, -17390.0 / 4096.0,
            0.0,             7003.0 / 4096.0,   -543.0 / 4096.0);
    return lmsToICtCp * lmsPq;
}

vec3 GE_ICtCpToBt2020Nits(vec3 ictcp)
{
    // Inverses of the BT.2100 matrices above, transcribed column-major.
    const mat3 ictcpToLms = mat3(
        1.0, 1.0, 1.0,
         0.008609037038, -0.008609037038,  0.560031335711,
         0.111029625003, -0.111029625003, -0.320627174987);
    vec3 lmsPq = ictcpToLms * ictcp;
    vec3 lms = vec3(GE_IctcpPqDecode(lmsPq.r),
                    GE_IctcpPqDecode(lmsPq.g),
                    GE_IctcpPqDecode(lmsPq.b));

    const mat3 lmsToRgb = mat3(
         3.436606694333, -0.791329555599, -0.025949899691,
        -2.506452118656,  1.983600451792, -0.098913714712,
         0.069845424323, -0.192270896193,  1.124863614402);
    return max(lmsToRgb * lms, vec3(0.0)) * 10000.0;
}

// Rolls highlight chroma off in ICtCp and returns to the caller's gamut.
// Intensity is untouched, so luminance and neutral greys are stable; only
// saturated values above paper white desaturate, reaching one third of their
// chroma at full strength and full display intensity.
//
// Input and output are display-linear Rec.709 where 1.0 == paperWhiteNits.
// strength 0 is an exact pass-through. Output range policy (clamping to the
// encoding's max linear value) belongs to the caller.
vec3 GE_IctcpCompressHighlightChroma(vec3 rec709Linear,
                                     float strength,
                                     float paperWhiteNits,
                                     float maxOutputNits)
{
    strength = clamp(strength, 0.0, 1.0);
    if (strength <= 0.0)
        return rec709Linear;

    // ICtCp is defined from BT.2020 RGB and absolute luminance, so enter those
    // domains only for this operation and hand Rec.709 back.
    const mat3 rec709ToBt2020 = mat3(
        0.6274040, 0.0690970, 0.0163916,
        0.3292820, 0.9195400, 0.0880132,
        0.0433136, 0.0113612, 0.8955950);
    const mat3 bt2020ToRec709 = mat3(
         1.6604910, -0.1245505, -0.0181508,
        -0.5876411,  1.1328999, -0.1005789,
        -0.0728499, -0.0083494,  1.1187297);

    float paperWhite = max(paperWhiteNits, 1e-3);
    float maxOutput = max(maxOutputNits, paperWhite);
    vec3 bt2020Nits = max(rec709ToBt2020 * max(rec709Linear, vec3(0.0)), vec3(0.0)) * paperWhite;
    vec3 ictcp = GE_Bt2020NitsToICtCp(bt2020Nits);

    float paperWhiteI = GE_IctcpPqEncode(paperWhite / 10000.0);
    float maxOutputI = GE_IctcpPqEncode(clamp(maxOutput / 10000.0, 0.0, 1.0));
    float highlight = smoothstep(paperWhiteI, max(maxOutputI, paperWhiteI + 1e-5), ictcp.x);
    ictcp.yz *= 1.0 / (1.0 + 2.0 * strength * highlight);

    return max(bt2020ToRec709 * (GE_ICtCpToBt2020Nits(ictcp) / paperWhite), vec3(0.0));
}
