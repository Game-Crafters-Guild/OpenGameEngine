#version 450

// A world mark-up's volume for the glow (MarkupRenderFeature): a box or a sphere generated from
// the vertex index, no vertex buffer. The unit shape spans [-1, 1] on each axis; uModel scales,
// rotates and places it.

layout(set = 0, binding = 5) uniform CameraUBO {
#include "Includes/camera_ubo_fields.glsl"
} cam;

layout(push_constant) uniform PC {
    mat4 uModel; // unit shape -> world: columns = axes * half extents (a sphere's radius), center
    vec4 uColor; // rgb = the mark-up's color (linear), a = body alpha
    vec4 uRim;   // x = rim power, y = rim gain, z = rim budget, w = shape (0 box, 1 sphere)
} pc;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec3 vPositionWS;
layout(location = 2) out vec2 vFacePoint;            // a box face's point in [-1, 1]
layout(location = 3) flat out vec2 vFaceHalfExtents; // the face's half extents (meters)

// The sphere's tessellation: MarkupRenderFeature::kSphereVertexCount restates
// kSphereRings * kSphereSegments * 6, and kBoxVertexCount the box's 36; change them together.
const int kSphereRings = 24;
const int kSphereSegments = 48;
const float kPi = 3.14159265;

// Corner `index` (0 to 5) of a quad's two triangles (0,0) (1,1) (1,0) and (0,0) (0,1) (1,1),
// as (u, v) in {0, 1}; arithmetic rather than an indexed constant array. Each triangle turns
// from +v toward +u.
vec2 QuadCorner(int index)
{
    float u = (index == 1 || index == 2 || index == 5) ? 1.0 : 0.0;
    float v = (index == 1 || index == 4 || index == 5) ? 1.0 : 0.0;
    return vec2(u, v);
}

// The unit vector along axis `axis` (0 x, 1 y, 2 z).
vec3 AxisVector(int axis)
{
    return vec3(axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0);
}

vec3 SpherePoint(float ring, float segment)
{
    float theta = kPi * ring / float(kSphereRings);
    float phi = 2.0 * kPi * segment / float(kSphereSegments);
    return vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// One winding for both shapes: on every face u x v points out of the volume and each triangle
// turns from +v toward +u, which in the engine's left-handed frame is counter-clockwise seen from
// outside, the pipeline's front face; markup_glow.frag tells the near side by gl_FrontFacing.
void main()
{
    int quad = gl_VertexIndex / 6;
    vec2 corner = QuadCorner(gl_VertexIndex % 6);
    vec3 localPosition;
    vec3 localNormal;
    vFacePoint = vec2(0.0);
    vFaceHalfExtents = vec2(0.0);
    if (pc.uRim.w > 0.5)
    {
        // u = segment, v = ring: d/dsegment x d/dring points out of the sphere.
        float ring = float(quad / kSphereSegments) + corner.y;
        float segment = float(quad % kSphereSegments) + corner.x;
        localPosition = SpherePoint(ring, segment);
        localNormal = localPosition;
    }
    else
    {
        // Face `quad` of the box: its normal axis and side, and the two axes spanning it. The
        // tangent (u) flips with the side so tangent x bitangent points out of the box on both.
        int axis = quad / 2;
        float side = (quad % 2 == 0) ? 1.0 : -1.0;
        vec3 normal = AxisVector(axis) * side;
        vec3 tangent = AxisVector((axis + 1) % 3) * side;
        vec3 bitangent = AxisVector((axis + 2) % 3);
        vFacePoint = corner * 2.0 - 1.0;
        localPosition = normal + tangent * vFacePoint.x + bitangent * vFacePoint.y;
        localNormal = normal;
        vFaceHalfExtents = vec2(length(mat3(pc.uModel) * tangent), length(mat3(pc.uModel) * bitangent));
    }
    vec4 world = pc.uModel * vec4(localPosition, 1.0);
    vPositionWS = world.xyz;
    // A box's face normal maps to its axis and a sphere's scale is uniform, so the model's own
    // 3x3 carries the normal; the fragment normalizes.
    vNormalWS = mat3(pc.uModel) * localNormal;
    gl_Position = cam.uVP * world;
}
