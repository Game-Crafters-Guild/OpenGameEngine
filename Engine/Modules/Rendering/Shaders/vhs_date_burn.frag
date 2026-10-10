#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform VhsDateBurnPC {
    float vhsDateBurnEnabled;
    float vhsDateBurnColorR;
    float vhsDateBurnColorG;
    float vhsDateBurnColorB;
    float vhsOverlayOpacity;
    float vhsDateBurnSize;
    float vhsDateBurnPositionX;
    float vhsDateBurnPositionY;
    float vhsDateBurnYear;
    float vhsDateBurnMonth;
    float vhsDateBurnDay;
    float vhsDateBurnHour;
    float vhsDateBurnMinute;
    float shaderAnimationTime;
} pc;

uvec2 DigitBits(int digit)
{
    if (digit == 0) return uvec2(2738546222u, 3u);
    if (digit == 1) return uvec2(2286031044u, 3u);
    if (digit == 2) return uvec2(3292807726u, 7u);
    if (digit == 3) return uvec2(3775349263u, 3u);
    if (digit == 4) return uvec2(301246856u, 2u);
    if (digit == 5) return uvec2(3775366207u, 3u);
    if (digit == 6) return uvec2(2736227374u, 3u);
    if (digit == 7) return uvec2(2216829471u, 0u);
    if (digit == 8) return uvec2(2736211502u, 3u);
    return uvec2(2702132782u, 3u);
}

uvec2 GlyphBits(int code)
{
    if (code >= 48 && code <= 57) return DigitBits(code - 48);
    if (code == 65) return uvec2(1663026734u, 4u);
    if (code == 66) return uvec2(3809986095u, 3u);
    if (code == 67) return uvec2(2182120510u, 7u);
    if (code == 68) return uvec2(3810051631u, 3u);
    if (code == 69) return uvec2(3256321087u, 7u);
    if (code == 70) return uvec2(1108837439u, 0u);
    if (code == 71) return uvec2(2736686142u, 7u);
    if (code == 74) return uvec2(2459181340u, 1u);
    if (code == 76) return uvec2(3255862305u, 7u);
    if (code == 77) return uvec2(1662703473u, 4u);
    if (code == 78) return uvec2(1662834289u, 4u);
    if (code == 79) return uvec2(2736309806u, 3u);
    if (code == 80) return uvec2(1108854319u, 0u);
    if (code == 82) return uvec2(1381484079u, 4u);
    if (code == 83) return uvec2(3775333438u, 3u);
    if (code == 84) return uvec2(138547359u, 1u);
    if (code == 85) return uvec2(2736309809u, 3u);
    if (code == 86) return uvec2(353945137u, 1u);
    if (code == 89) return uvec2(138553905u, 1u);
    if (code == 58) return uvec2(138416256u, 0u);
    if (code == 46) return uvec2(402653184u, 3u);
    return uvec2(0u);
}

float GlyphMask(vec2 fragmentPx, vec2 originPx, float cellPx, uvec2 bits)
{
    vec2 local = (fragmentPx - originPx) / cellPx;
    if (local.x < 0.0 || local.y < 0.0 || local.x >= 5.0 || local.y >= 7.0)
        return 0.0;

    ivec2 cell = ivec2(floor(local));
    int bitIndex = cell.y * 5 + cell.x;
    float enabled = bitIndex < 32
        ? float((bits.x >> uint(bitIndex)) & 1u)
        : float((bits.y >> uint(bitIndex - 32)) & 1u);
    vec2 within = abs(fract(local) - 0.5);
    float block = 1.0 - smoothstep(0.40, 0.52, max(within.x, within.y));
    return enabled * block;
}

int MonthCharacter(int month, int index)
{
    if (month == 1) return index == 0 ? 74 : (index == 1 ? 65 : 78);
    if (month == 2) return index == 0 ? 70 : (index == 1 ? 69 : 66);
    if (month == 3) return index == 0 ? 77 : (index == 1 ? 65 : 82);
    if (month == 4) return index == 0 ? 65 : (index == 1 ? 80 : 82);
    if (month == 5) return index == 0 ? 77 : (index == 1 ? 65 : 89);
    if (month == 6) return index == 0 ? 74 : (index == 1 ? 85 : 78);
    if (month == 7) return index == 0 ? 74 : (index == 1 ? 85 : 76);
    if (month == 8) return index == 0 ? 65 : (index == 1 ? 85 : 71);
    if (month == 9) return index == 0 ? 83 : (index == 1 ? 69 : 80);
    if (month == 10) return index == 0 ? 79 : (index == 1 ? 67 : 84);
    if (month == 11) return index == 0 ? 78 : (index == 1 ? 79 : 86);
    return index == 0 ? 68 : (index == 1 ? 69 : 67);
}

void main()
{
    vec4 source = texture(uSceneColor, vUV);
    if (pc.vhsDateBurnEnabled <= 0.5)
    {
        oColor = source;
        return;
    }

    vec2 imageSizePx = vec2(textureSize(uSceneColor, 0));
    float cellPx = clamp(floor(imageSizePx.y / 360.0), 2.0, 4.0)
                 * clamp(pc.vhsDateBurnSize, 0.5, 4.0) * 1.08;
    float advance = cellPx * 6.0;
    float dateWidth = cellPx * 65.0;
    float leftOrigin = cellPx * 6.0;
    float rightOrigin = max(leftOrigin, imageSizePx.x - dateWidth - cellPx * 6.0);
    float bottomOrigin = imageSizePx.y - cellPx * 21.0;
    float topOrigin = cellPx * 6.0;
    vec2 origin = vec2(
        mix(leftOrigin, rightOrigin,
            clamp(pc.vhsDateBurnPositionX, 0.0, 1.0)),
        mix(bottomOrigin, topOrigin,
            clamp(pc.vhsDateBurnPositionY, 0.0, 1.0)));
    vec2 fragmentPx = vUV * imageSizePx;

    int startMinutes = clamp(int(pc.vhsDateBurnHour + 0.5), 0, 23) * 60
                     + clamp(int(pc.vhsDateBurnMinute + 0.5), 0, 59);
    int runningMinutes = startMinutes
                       + int(floor(max(pc.shaderAnimationTime, 0.0) / 60.0));
    int hour24 = (runningMinutes / 60) % 24;
    int minute = runningMinutes % 60;
    int hour12 = hour24 % 12;
    if (hour12 == 0)
        hour12 = 12;

    float mask = 0.0;
    for (int index = 0; index < 8; ++index)
    {
        int code = 32;
        if (index == 0) code = hour24 < 12 ? 65 : 80;
        else if (index == 1) code = 77;
        else if (index == 3) code = hour12 >= 10 ? 48 + hour12 / 10 : 32;
        else if (index == 4) code = 48 + hour12 % 10;
        else if (index == 5) code = 58;
        else if (index == 6) code = 48 + minute / 10;
        else if (index == 7) code = 48 + minute % 10;
        mask = max(mask, GlyphMask(
            fragmentPx, origin + vec2(advance * float(index), 0.0),
            cellPx, GlyphBits(code)));
    }

    int month = clamp(int(pc.vhsDateBurnMonth + 0.5), 1, 12);
    int day = clamp(int(pc.vhsDateBurnDay + 0.5), 1, 31);
    int year = clamp(int(pc.vhsDateBurnYear + 0.5), 1900, 2099);
    for (int index = 0; index < 11; ++index)
    {
        int code = 32;
        if (index < 3) code = MonthCharacter(month, index);
        else if (index == 3) code = 46;
        else if (index == 4) code = 48 + day / 10;
        else if (index == 5) code = 48 + day % 10;
        else if (index == 7) code = 48 + (year / 1000) % 10;
        else if (index == 8) code = 48 + (year / 100) % 10;
        else if (index == 9) code = 48 + (year / 10) % 10;
        else if (index == 10) code = 48 + year % 10;
        mask = max(mask, GlyphMask(
            fragmentPx,
            origin + vec2(advance * float(index), cellPx * 8.0),
            cellPx, GlyphBits(code)));
    }

    // A low-amplitude neighboring sample gives the block characters the soft
    // halo of an optical camcorder OSD. The following VHS pass adds its normal
    // YIQ blur, chroma delay, tracking, and temporal field response.
    float halo = mask * 0.72;
    vec3 burnColor = clamp(
        vec3(pc.vhsDateBurnColorR,
             pc.vhsDateBurnColorG,
             pc.vhsDateBurnColorB),
        0.0, 1.0);
    oColor = vec4(mix(
        source.rgb, burnColor,
        halo * clamp(pc.vhsOverlayOpacity, 0.0, 1.0)), source.a);
}
