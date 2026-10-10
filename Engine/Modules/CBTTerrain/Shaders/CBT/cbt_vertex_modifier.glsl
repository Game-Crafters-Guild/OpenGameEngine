// CBT vertex output modifier (graphics side of the CBT/LEB terrain renderer).
// Included by adapter_vertex.glsl via GE_VERTEX_MODIFIER_PATH when the material's
// customVertexShader=true (which the clamp turns into HAS_VERTEX_OUTPUT_MODIFIER +
// CUSTOM_VERTEX_SHADER). There is NO vertex buffer: geometry is decoded entirely
// from the CBT SSBOs the compute update wrote this frame.
//
// Draw contract (plan §8 C3/C6): the camera forward pass issues DrawIndexedIndirectCount
// over the VISIBLE bisector stream (Classify frustum-culls into it) with an identity
// index buffer, so gl_VertexIndex walks 0..(3*visibleBisectors-1). Each run of three
// consecutive vertex ids is one bisector triangle:
//   triangle = gl_VertexIndex / 3     -> bisector slot via IndicesVisible[triangle]
//   corner   = gl_VertexIndex % 3     -> which of the LEB corners (0,1,2)
//
// C4: the compute VertexEval already displaced each corner to its real WORLD
// position (height sampled from the SAME terrain heightmap CDLOD samples) and stored
// the terrain UV alongside. We read those positions directly — no per-draw params on
// the graphics side (plan §8 C4). We TRUST VertexEval (do not re-derive the LEB or
// re-sample height here).
//
// Set 2 (the DrawBindings route terrain uses): CBT_DESCRIPTOR_SET=2 puts the two
// SSBOs on set 2, resolved by the MaterialBinder from DrawBindings by instance
// name (gIdxVis / gVertex). CBT_GRAPHICS_INCLUDE keeps the include int64-free and
// strips every compute-only write/atomic helper (plan §9 silent-null-variant trap).
#define CBT_GRAPHICS_INCLUDE
#define CBT_DESCRIPTOR_SET 2
#include "cbt_layout.glsl"

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    uint vid = uint(gl_VertexIndex);
    uint triangle = vid / 3u;
    uint corner = vid - triangle * 3u;

    uint slot = gIdxVis.IndicesVisible[triangle];
    CBTVertexData vd = gVertex.Vertex[slot];

    // This vertex's position + terrain UV (corner.w = uv.x, meta[k] = uv.y). The compute
    // VertexEval already displaced the position (height baked in). The per-pixel normal is
    // sampled from the terrain normalmap in the surface (meta.w = its bindless index),
    // matching CDLOD — smooth + crack-free (plan §8 C4).
    vec3 pos;
    vec2 uv;
    uvec2 packedSector;
    if (corner == 0u)      { pos = vd.corner0.xyz; uv = vec2(vd.corner0.w, vd.meta.x); packedSector = vd.sector0; }
    else if (corner == 1u) { pos = vd.corner1.xyz; uv = vec2(vd.corner1.w, vd.meta.y); packedSector = vd.sector1; }
    else                   { pos = vd.corner2.xyz; uv = vec2(vd.corner2.w, vd.meta.z); packedSector = vd.sector2; }

    // Storage mode rides the per-slot deepTag (terrain S2a). Legacy slots (0): pos is the
    // fp32 WORLD position and the adapter takes the GE_ClipFromWorld path — byte-identical
    // to the pre-S2a draw (dark-ship). Deep slots (1): pos is the SECTOR-LOCAL offset and
    // this corner's integer world sector rides v.sector, so the adapter projects through
    // GE_ClipFromSectorLocal — the exact-integer-delta reconstruction that keeps Earth-
    // magnitude geometry fp32-precise in clip space.
    v.position = pos;
    if (vd.deepTag.x != 0u)
    {
        v.sector = CBT_UnpackCornerSector(packedSector);
        v.useSector = true;
    }
    v.normal = vec3(0.0, 1.0, 0.0);              // placeholder; the surface samples the normalmap
    v.uv0 = uv;                                  // terrain UV: normalmap + future splat lookups
    v.custom0 = vec4(float(slot), 0.0, 0.0, vd.meta.w); // .x = slot (debug), .w = normalmap index
}
