/**
 * @file GPUInstanceDepthClass.h
 * @brief Depth/shadow batching class for a material + the GPUInstance.flags
 *        bits that carry it to the GPU.
 *
 * Shadow slices scatter and draw at (depth-class, mesh) granularity: casters
 * that share the material-independent depth PSO (opaque, no alpha test, no
 * vertex modify, not transmissive) collapse into one of two class sentinels
 * per mesh, while material-dependent casters keep their (material, mesh)
 * identity so cutout / vertex-animated / glass casters still shadow
 * correctly. This is the single source consumed by extraction (flags bits),
 * the shadow batch-table build (GPUDrawStreamBuilder), and the depth consumer
 * (RenderServicesDepthShadowPasses).
 */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace GameEngine
{
namespace Rendering
{

/// Whether a caster's shadow draw collapses into a shared-depth class batch
/// or keeps its (material, mesh) identity. uint8_t so it packs into the
/// per-materialIndex class array RenderServices owns and the span it hands the
/// shadow table build.
enum class MaterialDepthClass : uint8_t
{
    EligibleSingleSided = 0, ///< shared-depth eligible; PSO cull = back-face
    EligibleDoubleSided = 1, ///< shared-depth eligible; PSO cull = none
    MaterialDependent   = 2, ///< alpha-test / vertex-mod / transmissive / non-opaque — keep (mat, mesh)
};

// GPUInstance.flags low-nibble layout. MUST mirror the packing in
// RenderExtractionSystem and the reads in gpu_instance_fields.glsl /
// draw_command_scatter.comp:
//   bit  0     cast shadows
//   bit  1     receive shadows
//   bit  2     depth material-dependent  (this feature)
//   bit  3     depth double-sided        (this feature; meaningful only when bit 2 clear)
//   bit  4     mirrored winding           (odd-determinant world transform)
//   bit  5     sorted transparent         (order-dependent Blend; peeled to the sorted pass)
//   bits 16..31  16-bit world tag
inline constexpr uint32_t kInstanceFlagDepthMaterialDependent = 1u << 2;
inline constexpr uint32_t kInstanceFlagDepthDoubleSided       = 1u << 3;

/// Set when the instance's world transform has a negative upper-3x3
/// determinant (an odd number of mirror/negative-scale axes), which reverses
/// rasterized triangle winding. The scatter bucketer routes mirrored instances
/// into a parity-1 sibling batch row so the consumer can draw them with a
/// flipped front face; single-sided materials would otherwise back-face cull
/// the visible surface (black statues, one-sided walls). See
/// draw_command_scatter.comp and GPUDrawStreamBuilder's table builders.
inline constexpr uint32_t kInstanceFlagMirrored = 1u << 4;

/// Set when the instance is an ORDER-DEPENDENT alpha-blend surface that the
/// sorted transparent path owns (material alpha mode == Blend AND the blend
/// state is order-dependent; additive/multiply are commutative and stay on the
/// batched path). Stamped at extraction from exactly the predicate the CPU peel
/// uses (RenderServicesSortedTransparent + the recordEntityBatch peel), so the
/// GPU cull's transparent bucket membership is bit-identical to the CPU peel.
/// Transmission is coerced to Opaque before m_AlphaMode is set (MaterialRegistry)
/// so glass never gets this bit. Free bit 5 (bits 5..15 are unused). The scatter
/// shader does not read it; the sorted transparent GPU pipeline does.
inline constexpr uint32_t kInstanceFlagSortedTransparent = 1u << 5;

// DDGI publishes this instance's emissive as a sphere-proxy light record, so
// the probe trace must NOT also add its emissive on a ray hit. Per INSTANCE,
// not per material: two instances can share a material and disagree.
inline constexpr uint32_t kInstanceFlagGIEmitter = 1u << 6;

/// Kill switch: GE_MIRRORED_WINDING=0 disables the parity stamp at extraction,
/// reverting to pre-feature behavior (mirrored geometry renders inside-out;
/// batch tables and draw stream identical to before the feature landed). Read
/// once per process — the parity bit is never stamped when disabled, so no
/// later layer (registry, table build, scatter) ever sees a parity-1 row. An
/// inline function's function-local static has one process-wide instance, so
/// the env var is read exactly once regardless of how many TUs call this.
inline bool MirroredWindingEnabled()
{
    static const bool kEnabled = []
    {
        const char* v = std::getenv("GE_MIRRORED_WINDING");
        return v == nullptr || v[0] != '0';
    }();
    return kEnabled;
}

/// The (bit 2, bit 3) contribution a class stamps into GPUInstance.flags. The
/// scatter shader reads these bits to route the instance into its class
/// sentinel (bit 2 clear) or its own (mat, mesh) region (bit 2 set).
inline constexpr uint32_t DepthClassInstanceFlagBits(MaterialDepthClass cls)
{
    switch (cls)
    {
    case MaterialDepthClass::MaterialDependent:   return kInstanceFlagDepthMaterialDependent;
    case MaterialDepthClass::EligibleDoubleSided: return kInstanceFlagDepthDoubleSided;
    case MaterialDepthClass::EligibleSingleSided:
    default:                                      return 0u;
    }
}

} // namespace Rendering
} // namespace GameEngine
