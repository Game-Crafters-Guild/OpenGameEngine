// bone_palette.glsl — single source of truth for the bone palette atlas
// storage layout. All compute shaders that WRITE skin matrices and all
// vertex shaders that READ them go through the helpers below. Changing
// the layout (e.g. switching to dual quaternions later) means editing
// this file only.
//
// Storage: each bone occupies 3 consecutive vec4 rows in a `vec4 rows[]`
// SSBO. The 3 rows are the TOP 3 rows of a column-major mat4 (its
// bottom row is always (0,0,0,1) for affine transforms, so we drop it).
//   row 0 = (M[0][0], M[1][0], M[2][0], M[3][0])
//   row 1 = (M[0][1], M[1][1], M[2][1], M[3][1])
//   row 2 = (M[0][2], M[1][2], M[2][2], M[3][2])
// 12 floats / 48 bytes per bone vs 16 floats / 64 bytes for full mat4.
// 25% atlas bandwidth + memory saving, mathematically lossless for
// affine transforms (which is what skinning produces).
//
// Caller contract:
//   - Declare a buffer named `BonePaletteAtlas` with a `vec4 rows[]` field
//     before including this header (compute = restrict writeonly,
//     vertex = readonly, etc.).
//   - For write helpers, define `GE_BONE_PALETTE_WRITABLE` before include.
//   - The "atlas bone slot" parameter is in BONE units (1 bone = 3 vec4
//     rows internally). It matches the historical "mat4 offset" units —
//     callers do not change.

const uint kBonePaletteVec4sPerBone = 3u;

// Row index of a bone within an atlas. The single place the 3-rows-per-bone
// layout becomes an index; every accessor below, over whichever buffer, goes
// through it.
uint ge_BoneRowBase(uint atlasBoneSlot, uint localBoneIdx) {
    return kBonePaletteVec4sPerBone * (atlasBoneSlot + localBoneIdx);
}

// The buffer is declared writeonly OR readonly in the consuming shader.
// Helper functions are gated on the matching capability flag — a shader
// that needs write helpers defines GE_BONE_PALETTE_WRITABLE before
// include; one that needs read helpers defines GE_BONE_PALETTE_READABLE.

#ifdef GE_BONE_PALETTE_WRITABLE
// Pack a column-major mat4 as 3 vec4 rows and write them at the given
// bone slot. The bottom row of `m` is implicitly assumed to be
// (0, 0, 0, 1) — caller is responsible for ensuring m is affine.
void ge_StoreBonePalette(uint atlasBoneSlot, uint localBoneIdx, mat4 m) {
    uint base = ge_BoneRowBase(atlasBoneSlot, localBoneIdx);
    BonePaletteAtlas.rows[base + 0u] = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    BonePaletteAtlas.rows[base + 1u] = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    BonePaletteAtlas.rows[base + 2u] = vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
}
#endif // GE_BONE_PALETTE_WRITABLE

#ifdef GE_BONE_PALETTE_READABLE
void ge_LoadBoneRows(uint atlasBoneSlot, uint localBoneIdx,
                     out vec4 row0, out vec4 row1, out vec4 row2) {
    uint base = ge_BoneRowBase(atlasBoneSlot, localBoneIdx);
    row0 = BonePaletteAtlas.rows[base + 0u];
    row1 = BonePaletteAtlas.rows[base + 1u];
    row2 = BonePaletteAtlas.rows[base + 2u];
}

void ge_BlendBonePaletteRows(uint atlasBoneSlot, uvec4 joints, vec4 weights,
                             out vec4 row0, out vec4 row1, out vec4 row2) {
    vec4 r0a, r1a, r2a, r0b, r1b, r2b, r0c, r1c, r2c, r0d, r1d, r2d;
    ge_LoadBoneRows(atlasBoneSlot, joints.x, r0a, r1a, r2a);
    ge_LoadBoneRows(atlasBoneSlot, joints.y, r0b, r1b, r2b);
    ge_LoadBoneRows(atlasBoneSlot, joints.z, r0c, r1c, r2c);
    ge_LoadBoneRows(atlasBoneSlot, joints.w, r0d, r1d, r2d);

    row0 = weights.x * r0a + weights.y * r0b + weights.z * r0c + weights.w * r0d;
    row1 = weights.x * r1a + weights.y * r1b + weights.z * r1c + weights.w * r1d;
    row2 = weights.x * r2a + weights.y * r2b + weights.z * r2c + weights.w * r2d;
}

// Skinning blend + transform. Equivalent to the canonical:
//   mat4 skin = w.x*M[j.x] + w.y*M[j.y] + w.z*M[j.z] + w.w*M[j.w];
//   pos = (skin * vec4(pos, 1.0)).xyz;
//   norm = mat3(skin) * norm;
// but blends rows directly so we never reconstruct the full mat4.
void ge_ApplyBonePalette(inout vec3 position, inout vec3 normal,
                         uvec4 joints, vec4 weights, uint atlasBoneSlot) {
    vec4 r0, r1, r2;
    ge_BlendBonePaletteRows(atlasBoneSlot, joints, weights, r0, r1, r2);

    vec4 ph = vec4(position, 1.0);
    position = vec3(dot(r0, ph), dot(r1, ph), dot(r2, ph));
    // mat3 of M extracted from rows: column-major mat3 with cols 0..2.
    // Each output normal component = dot(row_i.xyz, normal_in).
    normal = vec3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
}

// Position-only variant (selection mask, depth-only passes).
vec3 ge_ApplyBonePalettePosition(vec3 position, uvec4 joints, vec4 weights,
                                 uint atlasBoneSlot) {
    vec4 r0, r1, r2;
    ge_BlendBonePaletteRows(atlasBoneSlot, joints, weights, r0, r1, r2);

    vec4 ph = vec4(position, 1.0);
    return vec3(dot(r0, ph), dot(r1, ph), dot(r2, ph));
}
#endif // GE_BONE_PALETTE_READABLE

#ifdef GE_BONE_PALETTE_PREV_READABLE
// A second read view over a DIFFERENT buffer holding the PREVIOUS frame's
// palettes. The TAA skinned motion-vector pass needs both pose endpoints in
// one draw, and GLSL cannot pass an SSBO as a parameter, so the accessor is
// duplicated rather than parameterised — the layout itself stays shared via
// ge_BoneRowBase.
//
// Caller contract: declare a buffer named `PrevBonePaletteAtlas` with a
// `vec4 rows[]` field (readonly) before including this header, and define
// GE_BONE_PALETTE_PREV_READABLE.
vec3 ge_ApplyPrevBonePalettePosition(vec3 position, uvec4 joints, vec4 weights,
                                     uint atlasBoneSlot) {
    uint b0 = ge_BoneRowBase(atlasBoneSlot, joints.x);
    uint b1 = ge_BoneRowBase(atlasBoneSlot, joints.y);
    uint b2 = ge_BoneRowBase(atlasBoneSlot, joints.z);
    uint b3 = ge_BoneRowBase(atlasBoneSlot, joints.w);

    vec4 r0 = weights.x * PrevBonePaletteAtlas.rows[b0 + 0u]
            + weights.y * PrevBonePaletteAtlas.rows[b1 + 0u]
            + weights.z * PrevBonePaletteAtlas.rows[b2 + 0u]
            + weights.w * PrevBonePaletteAtlas.rows[b3 + 0u];
    vec4 r1 = weights.x * PrevBonePaletteAtlas.rows[b0 + 1u]
            + weights.y * PrevBonePaletteAtlas.rows[b1 + 1u]
            + weights.z * PrevBonePaletteAtlas.rows[b2 + 1u]
            + weights.w * PrevBonePaletteAtlas.rows[b3 + 1u];
    vec4 r2 = weights.x * PrevBonePaletteAtlas.rows[b0 + 2u]
            + weights.y * PrevBonePaletteAtlas.rows[b1 + 2u]
            + weights.z * PrevBonePaletteAtlas.rows[b2 + 2u]
            + weights.w * PrevBonePaletteAtlas.rows[b3 + 2u];

    vec4 ph = vec4(position, 1.0);
    return vec3(dot(r0, ph), dot(r1, ph), dot(r2, ph));
}
#endif // GE_BONE_PALETTE_PREV_READABLE
