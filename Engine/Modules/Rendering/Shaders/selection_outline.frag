#version 450

// Selection outline composite pass.
//
// Reads a single-channel R8 selection mask (1.0 inside selected entities,
// 0.0 elsewhere) and runs a 3x3 Sobel edge detector. Where the gradient
// magnitude exceeds a threshold, outputs the configured outline color with
// full alpha. Elsewhere outputs alpha 0 so the surrounding scene is
// preserved by alpha-blending.
//
// A debug "smoke tint" push constant (uSmokeTint) optionally adds a uniform
// alpha tint of the outline color over the whole framebuffer — used during
// initial pass wiring to confirm the composite is actually executing without
// requiring a real selection mask to be populated.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uMask;

layout(push_constant) uniform Push
{
    vec4  uOutlineColor;     // rgb = color, a = max alpha at edge centre
    vec2  uTexelSize;        // 1 / mask resolution
    float uEdgeThreshold;    // gradient magnitude required to register an edge
    float uSmokeTint;        // 0..1, debug uniform tint for pass-wiring smoke test
    float uOutlineRadius;    // outline thickness in pixels
    // Pads the block to a multiple of its largest alignment (vec4: 16), the
    // size the MSL struct rounds up to, so Metal reads exactly what is pushed.
    float uPad0;
    float uPad1;
    float uPad2;
} pc;

void main()
{
    float center = texture(uMask, vUV).r;

    // Dilation-based outline: search outward up to `radius` pixels for any
    // selected texel. If the current texel is outside the mask but a selected
    // texel exists within that radius, draw the outline. The minimum radius
    // is 2 px so a slider value of 1.0 still produces a visible outline.
    float radius = max(pc.uOutlineRadius + 1.0, 2.0);
    float radiusSq = radius * radius;
    int   steps  = int(min(ceil(radius), 16.0));

    // Track squared distance throughout the search so we avoid sqrt() per
    // candidate texel. Final sqrt happens once at the smoothstep.
    float closestSq = radiusSq + 4.0;
    for (int y = -steps; y <= steps; ++y)
    {
        for (int x = -steps; x <= steps; ++x)
        {
            vec2 offs = vec2(float(x), float(y));
            float dSq = dot(offs, offs);
            if (dSq > radiusSq) continue;   // round corner of square kernel
            float s = texture(uMask, vUV + pc.uTexelSize * offs).r;
            if (s > 0.5 && dSq < closestSq)
                closestSq = dSq;
        }
    }

    // Inside selected region: no outline (the mesh occupies the pixel).
    // Outside but within radius: full alpha, with a 1px feathered outer edge.
    float edge = 0.0;
    if (center < 0.5 && closestSq <= radiusSq)
    {
        float closest = sqrt(closestSq);
        edge = 1.0 - smoothstep(radius - 1.0, radius, closest);
    }

    vec3 rgb = pc.uOutlineColor.rgb;
    float a  = edge * pc.uOutlineColor.a;

    // Smoke tint: uniform low-alpha overlay used to verify the pass runs
    // before the mask pass is implemented. Ignored in production once the
    // push constant is set to zero.
    a = max(a, pc.uSmokeTint);

    oColor = vec4(rgb, a);
}
