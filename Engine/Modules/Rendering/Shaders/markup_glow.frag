#version 450

// A world mark-up's glow (MarkupRenderFeature): a translucent body in the mark-up's color,
// alpha-over, with a rim in the same color. A region's display (markup_glow_mesh.vert) lights its
// rim along its top edge and its lid's outline from the rim coordinate its vertices carry, blended
// over as a box's is. A sphere's rim is a fresnel term that brightens
// toward grazing angles, added over the body: it adds at most the color times the rim budget. A
// box's rim follows its edges (a flat face has one n.v) and blends over as coverage: the rim
// raises the body's coverage toward the rim budget, so a box face keeps its hue and never
// brightens past the status color. Where the body meets scene geometry (within
// MarkupGlowContactBandWidth: 5 cm of view depth, or 2 pixels of screen) a contact line blends the
// status color over it at the full rim budget, so the line where a volume enters the ground
// reads. Written premultiplied (color by coverage) into the glow target, which composites One /
// OneMinusSrcAlpha.
//
// The scene depth is the pass's read-only depth attachment, sampled at the fragment's pixel:
// sampler2D, or sampler2DMS (sample 0) in the GE_MARKUP_GLOW_MS_DEPTH build (markup_glow_ms)
// when the pane is multisampled.

#include "Includes/markup_glow_rim.glsl"

layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} cam;

#ifdef GE_MARKUP_GLOW_MS_DEPTH
layout(set = 0, binding = 1) uniform sampler2DMS uSceneDepth;
#else
layout(set = 0, binding = 1) uniform sampler2D uSceneDepth;
#endif

layout(push_constant) uniform PC {
    mat4 uModel;
    vec4 uColor; // rgb = the mark-up's color (linear), a = body alpha
    vec4 uRim;   // x = rim power, y = rim gain, z = rim budget, w = shape (0 box, 1 sphere, 2 mesh)
} pc;

layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec3 vPositionWS;
layout(location = 2) in vec2 vFacePoint;               // a box face's point in [-1, 1]; a mesh's rim in x
layout(location = 3) flat in vec2 vFaceHalfExtents;    // the face's half extents (meters)

layout(location = 0) out vec4 oColor;

// View depth (meters along +Z) of a reverse-Z device depth: d = P22 + P32 / z in perspective,
// d = P22 * z + P32 in orthographic. The far plane (or sky, d = 0) maps far away.
float ViewDepth(float deviceDepth)
{
    if (cam.uP[3][3] > 0.5)
        return (deviceDepth - cam.uP[3][2]) / cam.uP[2][2];
    return cam.uP[3][2] / max(deviceDepth - cam.uP[2][2], 1e-7);
}

// The body's own color at this point, premultiplied: the alpha-over body and its rim.
vec4 BodyColor(float alpha)
{
    if (pc.uRim.w > 1.5)
    {
        float rim = MarkupGlowRim(clamp(vFacePoint.x, 0.0, 1.0), pc.uRim.x, pc.uRim.y);
        float coverage = alpha + (1.0 - alpha) * pc.uRim.z * rim;
        return vec4(pc.uColor.rgb * coverage, coverage);
    }
    if (pc.uRim.w > 0.5)
    {
        // Toward the camera: from the point for a perspective view, against the view's forward
        // (row 2 of the view matrix, +Z forward) for an orthographic one.
        bool orthographic = cam.uP[3][3] > 0.5;
        vec3 toCamera = orthographic ? -vec3(cam.uV[0][2], cam.uV[1][2], cam.uV[2][2])
                                     : cam.uCameraPos.xyz - vPositionWS;
        float nDotV = dot(normalize(vNormalWS), normalize(toCamera));
        float rim = MarkupGlowRim(nDotV, pc.uRim.x, pc.uRim.y);
        return vec4(pc.uColor.rgb * (alpha + pc.uRim.z * rim), alpha);
    }
    float facing = MarkupGlowBoxEdgeFacing(vFacePoint.x, vFacePoint.y, vFaceHalfExtents.x, vFaceHalfExtents.y);
    float rim = MarkupGlowRim(facing, pc.uRim.x, pc.uRim.y);
    float coverage = alpha + (1.0 - alpha) * pc.uRim.z * rim;
    return vec4(pc.uColor.rgb * coverage, coverage);
}

void main()
{
    // The contact gap and its rate per pixel, taken before the discard so the quad's
    // derivatives are defined.
    float bodyDepth = ViewDepth(gl_FragCoord.z);
    float gap = ViewDepth(texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r) - bodyDepth;
    float bandWidth = MarkupGlowContactBandWidth(fwidth(gap), bodyDepth);
    // The far side of the convex volume: markup_glow.vert winds every triangle front-facing
    // from outside, so the body is one layer deep wherever it is seen.
    if (!gl_FrontFacing)
        discard;
    vec4 body = BodyColor(pc.uColor.a);
    float contact = pc.uRim.z * MarkupGlowContactBand(gap, bandWidth);
    oColor = vec4(body.rgb * (1.0 - contact) + pc.uColor.rgb * contact, body.a * (1.0 - contact) + contact);
}
