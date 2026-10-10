#version 450

// 2D infinite grid fragment shader: renders anti-aliased grid lines on the Z=0
// (XY) plane for the editor's 2D scene view mode. Uses zoom-based LOD with
// decade subdivision (1-2-5 sequence) instead of distance-based fading.
//
// Pairs with infinite_grid.vert (fullscreen triangle). Push constants only.

layout(location = 0) in vec2 vNDC;

layout(push_constant) uniform PC {
    mat4 uInvVP;
    vec4 uCameraPos;     // unused in 2D; present for layout parity with 3D grid PC
    vec4 uGridColor;     // rgb = non-axis line color, a unused
    float uGridOpacity;
    float uCamDistance;   // ortho visible height in world units
    float uViewportH;    // viewport height in pixels
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

// Compute the 1-2-5 decade snap value for a given raw spacing.
float DecadeSnap(float raw)
{
    float decade = pow(10.0, floor(log(raw) / log(10.0)));
    float m = raw / decade;
    if      (m < 1.5) return decade;
    else if (m < 3.5) return decade * 2.0;
    else if (m < 7.5) return decade * 5.0;
    else              return decade * 10.0;
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

    // Intersect with Z=0 plane (XY view plane for 2D mode).
    if (abs(rayDir.z) < 1e-7)
        discard;
    float t = -nearPt.z / rayDir.z;
    if (t < 0.0)
        discard;

    // Output correct depth so the grid is occluded by scene geometry.
    // Reverse-Z: depth=1 at near, 0 at far.
    gl_FragDepth = (1.0 - t) * farW.w / ((1.0 - t) * farW.w + t * nearW.w);

    vec3 worldPos = nearPt + t * rayDir;
    vec2 coord = worldPos.xy;

    // Zoom-based LOD: compute fine cell size from visible height using
    // the same 1-2-5 decade sequence as the editor's snap logic.
    const float kTargetLines = 40.0;
    float rawSpacing = pc.uCamDistance / kTargetLines;
    float fineCell = DecadeSnap(rawSpacing);
    float coarseCell = fineCell * 10.0;

    float fineGrid   = GridLine(coord, fineCell);
    float coarseGrid = GridLine(coord, coarseCell);

    // Crossfade: fade fine grid out as zoom approaches the next coarser level.
    // Use the log-space fractional position within the current decade.
    float logRatio = log2(pc.uCamDistance / (fineCell * kTargetLines));
    float fineFade = 1.0 - smoothstep(0.3, 0.9, abs(logRatio));
    float grid = coarseGrid + fineGrid * fineFade;
    grid = clamp(grid, 0.0, 1.0);

    // Axis coloring: X-axis red, Y-axis green (2D convention).
    vec2 axisDdx = fwidth(coord);
    float xAxis = 1.0 - smoothstep(0.0, axisDdx.y * 1.5, abs(coord.y));
    float yAxis = 1.0 - smoothstep(0.0, axisDdx.x * 1.5, abs(coord.x));

    vec3 color = pc.uGridColor.rgb;
    color = mix(color, vec3(0.2, 0.95, 0.2), yAxis);
    color = mix(color, vec3(0.95, 0.2, 0.2), xAxis);

    float axisAlpha = max(xAxis, yAxis);
    float alpha = mix(grid * 0.6, 1.0, axisAlpha);

    // No distance fade in ortho 2D — just apply user opacity.
    alpha *= pc.uGridOpacity;

    // Premultiplied alpha output for (One, OneMinusSrcAlpha) composite.
    oColor = vec4(color * alpha, alpha);
}