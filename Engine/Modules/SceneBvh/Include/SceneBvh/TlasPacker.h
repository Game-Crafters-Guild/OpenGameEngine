#pragma once

#include <span>
#include <vector>

#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "SceneBvh/UberMaterial.h"
#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// ── Packed scene buffer ────────────────────────────────────────────────────
//
// One float array holds three tables back to back:
//
//   [ uber materials x 28f | instances x 28f | TLAS nodes x 12f ]
//
// so the whole top level costs a SINGLE storage binding. Materials and instance
// records share the 28-float stride; TLAS nodes use their own 12-float stride
// and start at TlasBase.
//
// INSTANCE RECORD (28 floats, at InstanceBase + slot * 28):
//
//   [0..11]  inverse world matrix as three ROWS of four
//              row r = (m[r][0], m[r][1], m[r][2], translation[r])
//            A world-space ray becomes a local-space ray with three dot
//            products plus the translation terms, and a local normal becomes a
//            world normal via n.x*row0 + n.y*row1 + n.z*row2 — the inverse
//            transpose falls out of the same rows for free.
//   [12]     BLAS root node index
//   [13]     BLAS end node index (traversal stops here)
//   [14]     winding sign, +1 or -1. A mirrored instance flips the sign of the
//            Moller-Trumbore determinant, so front/back-face tests multiply by
//            this before deciding.
//   [15]     PER-INSTANCE material slot into the uber-material table, or -1 for
//            "this instance has no material of its own — take the one the
//            geometry carries". A BLAS is shared by every instance of a mesh,
//            so its per-triangle material table can only hold ONE answer;
//            without this slot two instances of the same mesh could not shade
//            differently. Negative rather than a large sentinel because the
//            record is a float buffer (see the TLAS node note below).
//   [16]     1.0 when this instance is a DDGI GI emitter, else 0. Its emissive
//            is already published as a sphere-proxy light, so the probe trace
//            must not add it again on a ray hit (the hardware lane reads the
//            same fact from GPUInstance.flags bit 6).
//   [17..27] zero
//
// The local ray direction is left UNNORMALIZED on purpose: the ray parameter t
// is then identical in local and world space, so best-t comparisons stay valid
// across instances and non-uniform scale works.
//
// TLAS NODE (12 floats, at TlasBase + node * 12):
//
//   [0..5]   AABB min xyz, max xyz (world space)
//   [6]      miss link
//   [7]      instance offset (leaf only, else 0)
//   [8]      instance count  (0 marks an INTERIOR node -> descend to node + 1)
//   [9..11]  zero
//
// Every value is a plain float holding an exact small integer. Unlike the BLAS
// nodes (ThreadedBvh.h) nothing is bit-cast here, because this table lives in a
// FLOAT buffer: a uint payload smuggled through a float buffer breaks on real
// drivers — denormal miss links flush to zero, and a 0xFFFFFFFF interior marker
// is a NaN whose payload bits may be canonicalized away. The leaf test is
// therefore "instance count > 0" rather than a sentinel word.
//
// Threading matches the BLAS exactly: pre-order nodes, left child at node + 1,
// miss link = first node after the subtree.
//
// Leaves address PERMUTED instance slots. Instance records are written in TLAS
// slot order so a leaf's instances are contiguous; InstanceOrder maps a slot
// back to the caller's instance index.

inline constexpr uint32 kTlasNodeStrideFloats = 12u;

// Instances per TLAS leaf. Matches the reference (buildTlasRecords leafSize 2).
inline constexpr uint32 kTlasLeafInstances = 2u;

// Instance count ceiling: slot indices ship as f32 in the leaf payload, which
// represents integers exactly only up to 2^24.
inline constexpr uint32 kMaxTlasInstances = 1u << 24;

// TlasInstance::MaterialSlot value meaning "no per-instance material": the
// traversal falls back to the material the hit geometry carries. This is the
// default, so a caller that shares one material per BLAS need not think about
// instance materials at all.
inline constexpr uint32 kTlasInstanceMaterialFromGeometry = 0xFFFFFFFFu;

// One instance of an already-built BLAS. The packer derives the inverse
// transform, the winding sign and the world-space AABB from these.
struct TlasInstance
{
    Mathematics::Matrix4x4 WorldFromLocal{};

    // The BLAS's local-space AABB (ThreadedBvh::LocalBounds).
    Mathematics::AABB LocalBounds{};

    // Node index range of this instance's BLAS inside the shared node buffer.
    uint32 BlasRoot = 0;
    uint32 BlasEnd = 0;

    // Index into the uber-material table this instance shades with, overriding
    // whatever its shared BLAS's triangles carry.
    uint32 MaterialSlot = kTlasInstanceMaterialFromGeometry;

    // Emissive already published as a DDGI sphere-proxy light; exclude it from
    // per-hit emissive so the energy is not counted twice.
    bool GIEmitter = false;
};

struct PackedTlas
{
    std::vector<float32> Buffer;

    // Slot -> caller instance index. Instance records live in slot order.
    std::vector<uint32> InstanceOrder;

    uint32 MaterialCount = 0;
    uint32 InstanceCount = 0;
    uint32 TlasNodeCount = 0;

    // Float-element offsets into Buffer, ready to hand to a shader as uniforms.
    uint32 InstanceBase = 0;
    uint32 TlasBase = 0;

    Mathematics::AABB WorldBounds{};

    bool IsEmpty() const { return Buffer.empty(); }
};

// Builds a threaded TLAS over per-instance world AABBs and packs it, the
// instance records and the uber-material table into one float buffer.
class TlasPacker
{
public:
    // Returns an empty PackedTlas when there are no instances or when the
    // instance count exceeds the format's ceiling. Rejections are logged.
    static PackedTlas Pack(std::span<const UberMaterial> materials,
                           std::span<const TlasInstance> instances);
};

} // namespace GameEngine::SceneBvh
