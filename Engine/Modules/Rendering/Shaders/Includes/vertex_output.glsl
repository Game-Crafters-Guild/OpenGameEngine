// Shared struct for the extended vertex modifier contract.
// When HAS_VERTEX_OUTPUT_MODIFIER is defined, the vertex modifier implements:
//   void ModifyVertex(inout VertexOutput v, InstanceData inst);
// instead of the simple:
//   vec3 ModifyVertex(vec3 position, InstanceData inst);
//
// The modifier takes full ownership of vertex inputs and fills position,
// normal, and UV from scratch. Used by terrain, water, and other procedural
// geometry that doesn't use standard mesh vertex attributes.

#ifndef GE_VERTEX_OUTPUT_GLSL
#define GE_VERTEX_OUTPUT_GLSL

struct VertexOutput
{
    vec3 position;   // position (local-space for standard path, world-space for vertex output modifiers; SECTOR-LOCAL when useSector)
    vec3 normal;     // normal (local-space for standard path, world-space for vertex output modifiers)
    vec2 uv0;        // primary UV
    vec4 custom0;    // user-defined data passed to surface shader (e.g., terrain heightmap params)
    // Earth-scale (sector, local) vertex (CBT terrain deep decode). When useSector is
    // true, position is the SECTOR-LOCAL world offset and sector is this VERTEX's integer
    // 1024 m world sector — the adapter then projects via GE_ClipFromSectorLocal(position,
    // sector, ...), the mesh path's exact-integer-delta math with a per-vertex sector
    // instead of a per-instance one. DefaultVertexOutput leaves it off, so every modifier
    // that does not set it (ocean, water, legacy CBT slots) keeps the full-world path.
    ivec3 sector;
    bool useSector;
#ifdef GE_USER_PARTICLE_BUFFER
    vec4 particleAnimation;
    mat3 particleBasis;
    // What an unlit particle's colour and emission are multiplied by: the reciprocal of the view's
    // exposure. 1 for lit particles, which stay scene-linear.
    float particleColorScale;
    // The near-camera fade of a sprite (particle_billboard.glsl), 1 for everything else. Already in
    // custom0.a; the surface also applies it where a blend draws colour regardless of alpha.
    float particleNearFade;
#endif
};

VertexOutput DefaultVertexOutput()
{
    VertexOutput v;
    v.position = vec3(0.0);
    v.normal = vec3(0.0, 1.0, 0.0);
    v.uv0 = vec2(0.0);
    v.custom0 = vec4(1.0);
    v.sector = ivec3(0);
    v.useSector = false;
#ifdef GE_USER_PARTICLE_BUFFER
    v.particleAnimation = vec4(0.0);
    v.particleBasis = mat3(1.0);
    v.particleColorScale = 1.0;
    v.particleNearFade = 1.0;
#endif
    return v;
}

#endif // GE_VERTEX_OUTPUT_GLSL
