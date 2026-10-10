#pragma once

#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Types/StringId.h"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

namespace GameEngine { namespace Rendering {

// Forward declarations
struct TypeDesc;
struct Member;

// Type system
enum class TypeKind : uint32_t { Scalar = 0, Vector = 1, Matrix = 2, Struct = 3 };
enum class BaseType : uint32_t { Unknown = 0, Float = 1, Int = 2, UInt = 3, Bool = 4 };

// The dimension recorded for a runtime (unsized) array.
inline constexpr uint32_t kRuntimeArrayDim = 0;

struct TypeDesc {
    TypeKind Kind = TypeKind::Scalar;
    BaseType Base = BaseType::Unknown;
    uint32_t Rows = 1;     // for matrices
    uint32_t Cols = 1;     // for matrices
    uint32_t VecSize = 1;  // for vectors
    std::vector<uint32_t> ArrayDims; // outer to inner, e.g. [4] or [3,2]; kRuntimeArrayDim for `[]`
    std::vector<Member> StructMembers; // if Kind == Struct
};

struct Member {
    std::string Name;
    TypeDesc Type;
    uint32_t Offset = 0;
    // Bytes the member occupies in its block. For a runtime array this is one
    // element's unpadded size, not the row stride — index rows by ArrayStride.
    uint32_t Size = 0;
    // std140/std430 element stride of an array member, sized or runtime. For a
    // nested array it is the innermost dimension's stride (a sized one's Size is
    // product(ArrayDims) * ArrayStride), not the outer row stride.
    std::optional<uint32_t> ArrayStride;
    std::optional<uint32_t> MatrixStride; // std140/std430
    bool RowMajor = false;
};

struct BlockLayout {
    uint32_t Size = 0;
    std::vector<Member> Members;
};

struct PushConstantRangeMeta {
    uint32_t Id = 0;         // stable id assigned during merge; usable for by-id access
    std::string Name;
    uint32_t Size = 0;
    uint32_t StagesMask = 0;
    BlockLayout Block;
};

// ShaderMeta binding type encoding (set by SPIR-V reflection).
namespace ShaderMetaBindingType
{
    static constexpr uint32_t kUniformBuffer        = 0;
    static constexpr uint32_t kStorageBuffer        = 1;
    static constexpr uint32_t kSampler              = 2;
    static constexpr uint32_t kSampledImage         = 3;
    static constexpr uint32_t kStorageImage         = 4;
    static constexpr uint32_t kCombinedImageSampler = 5;
    static constexpr uint32_t kAccelerationStructure = 6;
}

// ShaderMeta stage-mask encoding (DescriptorBindingMeta::StagesMask and PushConstantRangeMeta::StagesMask,
// set by SPIR-V reflection): one backend-neutral bit per stage. MaterialBuilder maps it to the backend's
// stage flags.
namespace ShaderMetaStage
{
    static constexpr uint32_t kVertex                 = 1u << 0;
    static constexpr uint32_t kFragment               = 1u << 1;
    static constexpr uint32_t kCompute                = 1u << 2;
    static constexpr uint32_t kGeometry               = 1u << 3;
    static constexpr uint32_t kMesh                   = 1u << 4;
    static constexpr uint32_t kTessellationControl    = 1u << 5;
    static constexpr uint32_t kTessellationEvaluation = 1u << 6;
}

struct DescriptorBindingMeta {
    uint32_t Binding = 0;
    std::string Name;
    // HashStringId(Name), populated by FinalizeSetLayout. Binding resolution
    // keys on this on the per-batch hot path so the string is hashed once at
    // meta load, not once per binding per draw. Read via ResolvedNameId() —
    // hand-synthesized metas that skip FinalizeSetLayout leave it 0 and fall
    // back to hashing Name.
    StringId NameId = 0;
    uint32_t Type = 0;     // ShaderMetaBindingType constant
    uint32_t Count = 1;    // array size
    uint32_t StagesMask = 0;
    std::optional<BlockLayout> Block; // for UBO/SSBO
    // SSBO declared read-only in the shader (SPIR-V NonWritable) — WebGPU
    // layouts must state ReadOnlyStorage to match the shader module.
    bool ReadOnly = false;
    // Dim keeps the historical convention (1/2/3; cube reported as 2 with
    // Cube=true). Format is the engine TextureFormat of a storage image's
    // declared texel format (0 = none declared) — WebGPU layouts require it.
    // Depth: the SPIR-V OpTypeImage depth operand. A shadow sampler translates
    // to WGSL as texture_depth_* + sampler_comparison, and a WebGPU bind group
    // layout that claims a filterable float texture instead rejects the bind.
    // Filtered: the module reaches this binding through an OpImageSample*/
    // OpImageGather — WebGPU's static-usage validation then requires the
    // layout to say Float (filterable). texelFetch-only bindings stay
    // unfiltered, which also keeps them bindable to non-filterable formats.
    struct ImageInfo { uint32_t Dim = 2; bool Arrayed = false; bool Multisample = false;
                       bool Cube = false; bool Depth = false; bool Filtered = false;
                       uint32_t Format = 0; bool UnsignedInteger = false; };
    std::optional<ImageInfo> Image; // for image/sampler types

    StringId ResolvedNameId() const
    {
        return NameId != 0 ? NameId : HashStringId(Name);
    }
};

struct DescriptorSetMeta {
    uint32_t Set = 0;
    std::vector<DescriptorBindingMeta> Bindings;

    // Precomputed FNV-1a hash over the bindings — populated by
    // `FinalizeSetLayout` (called at the end of `MergeStages` and after
    // JSON deserialization). MaterialBinder uses this for its per-pass
    // descriptor-set cache key, avoiding a heap-allocating layout build
    // + walk-and-hash per draw. Tests that synthesize DescriptorSetMeta
    // directly may leave this at zero — the binder falls back to an
    // on-the-fly compute when it sees zero, so test setups still work
    // without an extra call.
    uint64_t BuiltLayoutHash = 0;
};

// FNV-1a over the binding signature (binding index, type, count, stages
// mask). Inputs are the RAW ShaderMetaBindingType / stages mask — NOT
// the backend-mapped `Rendering::DescriptorType` / shader-stage bits the
// old `HashSetLayout(DescriptorSetLayoutDesc)` operated on. Deliberate:
// the hash only needs to be consistent + discriminating across calls
// inside MaterialBinder, and hashing pre-mapping skips a per-binding
// map call on the hot path.
//
// Implication if MapShaderMetaBindingType / MapShaderMetaStagesToBackend
// ever stops being injective (two raw types collapsing onto one backend
// type, or vice versa): this hash will discriminate raw inputs that the
// backend would treat as identical. That's safe — the cache holds the
// descriptor set keyed on its actual reflected layout, so distinct raw
// shapes that the backend lumps together would live in separate cache
// entries (slight memory cost, no correctness issue). Conversely the
// hash will NEVER produce a false collision the old function avoided,
// since the backend mapping cannot expand the domain.
//
// Used as cache-key material in MaterialBinder's per-pass descriptor-
// set cache.
inline uint64_t ComputeDescriptorSetLayoutHash(const DescriptorSetMeta& sm) noexcept
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    const auto mix = [&](uint64_t x) noexcept { h ^= x; h *= 1099511628211ull; };
    mix(sm.Bindings.size());
    for (const auto& b : sm.Bindings)
    {
        mix(uint64_t(b.Binding));
        mix(uint64_t(b.Type));
        mix(uint64_t(b.Count));
        mix(uint64_t(b.StagesMask));
    }
    // Distinguish "empty / not-yet-built" (hash of zero bindings) from a
    // deliberately-empty layout. We never produce raw zero on a real meta
    // because the FNV mixing of size=0 yields a nonzero seed.
    return h != 0 ? h : 1ull;
}

// Convenience: finalize the precomputed fields on a single set.
inline void FinalizeSetLayout(DescriptorSetMeta& sm) noexcept
{
    sm.BuiltLayoutHash = ComputeDescriptorSetLayoutHash(sm);
    for (auto& b : sm.Bindings)
        b.NameId = HashStringId(b.Name);
}

struct StageIO {
    uint32_t Location = 0;
    // First 32-bit component of the location this variable occupies (SPIR-V
    // `Component` decoration). Several narrow variables may share one location
    // as long as their component spans are disjoint, so location alone does not
    // identify an interface slot.
    uint32_t Component = 0;
    std::string Name;
    TypeDesc Type;
    std::optional<std::string> Builtin; // if a builtin variable
};

struct StageMeta {
    std::vector<StageIO> Inputs;
    std::vector<StageIO> Outputs;
    struct LocalSize { uint32_t X=1, Y=1, Z=1; };
    std::optional<LocalSize> ComputeLocalSize;
    std::optional<LocalSize> MeshLocalSize;
    std::string EntryPoint = "main";
};

struct SpecConstantMeta {
    uint32_t Id = 0;
    std::string Name;
    TypeDesc Type;
    std::optional<std::string> DefaultValue; // textual repr
};

struct ShaderMeta {
    uint32_t Version = 1;
    std::unordered_map<std::string, std::string> EntryPoints; // stage -> name
    std::vector<PushConstantRangeMeta> PushConstants;
    std::vector<DescriptorSetMeta> Sets;
    std::unordered_map<std::string, StageMeta> Stages; // "vs","fs","cs","ms","gs"
    std::vector<SpecConstantMeta> SpecConstants;
    std::vector<std::string> Requirements; // features/extensions implied
    std::vector<std::string> RequiredDefines; // variant defines
    // The program's `// @property` declarations with their packed lanes — what
    // the composed GE_Props block reads. Empty for a program that composes no
    // declared surface (compute, post-process, packaged shaders).
    std::vector<ShaderProperty> DeclaredProperties;
};

}} // namespace GameEngine::Rendering
