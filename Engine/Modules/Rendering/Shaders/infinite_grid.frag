#version 450

// Infinite grid fragment shader: analytically renders anti-aliased grid lines
// on the Y=0 plane with LOD crossfade and distance fade. Outputs premultiplied-
// alpha color for composition via (One, OneMinusSrcAlpha) blending.
//
// All uniforms are in push constants — no descriptor sets needed.

layout(location = 0) in vec2 vNDC;

layout(push_constant) uniform PC {
    mat4 uInvVP;
    vec4 uCameraPos;    // xyz = world position, w unused
    vec4 uGridColor;    // rgb = non-axis line color, a unused (axis colors stay hardcoded)
    float uGridOpacity;
    float uFadeStart;
    float uFadeEnd;
    float _pad0;
} pc;

layout(location = 0) out vec4 oColor;

float GridLine(vec2 coord, float cellSize)
{
    vec2 grid = coord / cellSize;
    vec2 d = abs(fract(grid - 0.5) - 0.5);
    vec2 ddx = fwidth(grid);
    vec2 line = 1.0 - smoothstep(vec2(0.0), ddx * 1.5, d);
    return max(line.x, line.y);
}

void main()
{
    // Per-fragment unproject for precision.
    // Reverse-Z: near plane is at NDC z=1, far plane at NDC z=0.
    vec4 nearW = pc.uInvVP * vec4(vNDC, 1.0, 1.0);
    vec4 farW  = pc.uInvVP * vec4(vNDC, 0.0, 1.0);
    vec3 nearPt = nearW.xyz / nearW.w;
    vec3 farPt  = farW.xyz / farW.w;

    vec3 rayDir = farPt - nearPt;
    float t = -nearPt.y / rayDir.y;

    if (t < 0.0)
        discard;

    // Output correct depth so the grid is occluded by scene geometry.
    // Reverse-Z: depth=1 at the near plane, 0 at the far plane.
    // Perspective-correct interpolation of NDC z between (near z=1, far z=0).
    gl_FragDepth = (1.0 - t) * farW.w / ((1.0 - t) * farW.w + t * nearW.w);

    vec3 worldPos = nearPt + t * rayDir;
    float camDist = length(worldPos - pc.uCameraPos.xyz);

    vec2 coord = worldPos.xz;

    // Two LOD levels: fine (1m) and coarse (10m).
    float fineGrid   = GridLine(coord, 1.0);
    float coarseGrid = GridLine(coord, 10.0);

    // LOD crossfade: fine grid fades out between 15-40m.
    float fineFade = 1.0 - smoothstep(15.0, 40.0, camDist);
    float grid = coarseGrid + fineGrid * fineFade;
    grid = clamp(grid, 0.0, 1.0);

    // Axis coloring: X-axis red, Z-axis blue.
    vec2 axisDdx = fwidth(coord);
    float xAxis = 1.0 - smoothstep(0.0, axisDdx.y * 1.5, abs(coord.y));
    float zAxis = 1.0 - smoothstep(0.0, axisDdx.x * 1.5, abs(coord.x));

    vec3 color = pc.uGridColor.rgb;
    color = mix(color, vec3(0.2, 0.35, 0.95), zAxis);
    color = mix(color, vec3(0.95, 0.2, 0.2), xAxis);

    float axisAlpha = max(xAxis, zAxis);
    float alpha = mix(grid * 0.6, 1.0, axisAlpha);

    // Distance fade.
    float distFade = 1.0 - smoothstep(pc.uFadeStart, pc.uFadeEnd, camDist);
    alpha *= distFade * pc.uGridOpacity;

    // Premultiplied alpha output for (One, OneMinusSrcAlpha) composite.
    oColor = vec4(color * alpha, alpha);
}
