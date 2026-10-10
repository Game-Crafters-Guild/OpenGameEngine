#include "Rendering/Materials/ShaderReflection.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMetaJson.h"

#if RENDERING_ENABLE_SPIRV_REFLECTION
#include <spirv_reflect.h>
#endif

#include <cstring>

namespace GameEngine { namespace Rendering {

// The ShaderMeta stage-mask bit of a reflected stage: the one definition its readers test against.
static uint32_t StageToMask(ShaderStageKind s) {
    switch (s) {
        case ShaderStageKind::Vertex: return ShaderMetaStage::kVertex;
        case ShaderStageKind::Fragment: return ShaderMetaStage::kFragment;
        case ShaderStageKind::Compute: return ShaderMetaStage::kCompute;
        case ShaderStageKind::Geometry: return ShaderMetaStage::kGeometry;
        case ShaderStageKind::Mesh: return ShaderMetaStage::kMesh;
        case ShaderStageKind::TessControl: return ShaderMetaStage::kTessellationControl;
        case ShaderStageKind::TessEval: return ShaderMetaStage::kTessellationEvaluation;
        default: return 0;
    }
}

#if RENDERING_ENABLE_SPIRV_REFLECTION

// BaseType carries no width, so only 32-bit scalars map to a named base.
static BaseType ScalarBaseType(const SpvReflectTypeDescription& t) {
    constexpr uint32_t kMappedWidth = 32;
    if (t.type_flags & SPV_REFLECT_TYPE_FLAG_BOOL) return BaseType::Bool;
    if (t.traits.numeric.scalar.width != kMappedWidth) return BaseType::Unknown;
    if (t.type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) return BaseType::Float;
    if (t.type_flags & SPV_REFLECT_TYPE_FLAG_INT) return t.traits.numeric.scalar.signedness ? BaseType::Int : BaseType::UInt;
    return BaseType::Unknown;
}

// An array type's `op` is the array op while its element's shape lives in the
// type flags and numeric traits, so the walker classifies from those. The
// dimensions come from the array traits, a runtime array's as 0
// (SPV_REFLECT_ARRAY_DIM_RUNTIME == kRuntimeArrayDim).
static TypeDesc MakeTypeDesc(const SpvReflectTypeDescription* t) {
    TypeDesc o{};
    if (!t) return o;
    o.ArrayDims.assign(t->traits.array.dims, t->traits.array.dims + t->traits.array.dims_count);
    if (t->type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) {
        o.Kind = TypeKind::Struct;
        o.StructMembers.reserve(t->member_count);
        for (uint32_t i = 0; i < t->member_count; ++i) {
            Member m{};
            m.Name = t->members[i].struct_member_name ? t->members[i].struct_member_name : std::string();
            m.Type = MakeTypeDesc(&t->members[i]);
            o.StructMembers.push_back(std::move(m));
        }
        return o;
    }
    o.Base = ScalarBaseType(*t);
    if (t->type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX) {
        o.Kind = TypeKind::Matrix;
        o.Rows = t->traits.numeric.matrix.row_count;
        o.Cols = t->traits.numeric.matrix.column_count;
        return o;
    }
    if (t->type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR) {
        o.Kind = TypeKind::Vector;
        o.VecSize = t->traits.numeric.vector.component_count;
        return o;
    }
    o.Kind = TypeKind::Scalar;
    return o;
}

static void FillBlockLayout(const SpvReflectBlockVariable& src, BlockLayout& dst) {
    dst.Size = src.padded_size ? src.padded_size : src.size;
    dst.Members.reserve(src.member_count);
    for (uint32_t i = 0; i < src.member_count; ++i) {
        const auto& sm = src.members[i];
        Member m{};
        m.Name = sm.name ? sm.name : std::string();
        m.Type = MakeTypeDesc(sm.type_description);
        m.Offset = sm.offset;
        m.Size = sm.size;
        // ArrayStride decorates the array type, sized or runtime; the block
        // variable's own array traits are only copied for sized arrays.
        if (sm.type_description && sm.type_description->traits.array.stride)
            m.ArrayStride = sm.type_description->traits.array.stride;
        dst.Members.push_back(std::move(m));
    }
}

// SPIR-V storage-image texel format -> engine TextureFormat (the subset the
// engine's compute shaders declare; extend on contact). 0 = none/unknown.
static uint32_t ImageFormatToEngineFormat(SpvImageFormat f) {
    switch (f) {
        case SpvImageFormatRgba32f:  return static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        case SpvImageFormatRgba16f:  return static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        case SpvImageFormatR32f:     return static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        case SpvImageFormatRg16f:    return static_cast<uint32_t>(TextureFormat::R16G16_FLOAT);
        case SpvImageFormatR16f:     return static_cast<uint32_t>(TextureFormat::R16_FLOAT);
        case SpvImageFormatRgba8:    return static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        case SpvImageFormatR11fG11fB10f: return static_cast<uint32_t>(TextureFormat::R11G11B10_FLOAT);
        case SpvImageFormatRgb10A2:  return static_cast<uint32_t>(TextureFormat::RGB10A2_UNORM);
        case SpvImageFormatR32ui:    return static_cast<uint32_t>(TextureFormat::R32_UINT);
        case SpvImageFormatR32i:     return static_cast<uint32_t>(TextureFormat::R32_SINT);
        case SpvImageFormatRg32ui:   return static_cast<uint32_t>(TextureFormat::R32G32_UINT);
        case SpvImageFormatRgba32ui: return static_cast<uint32_t>(TextureFormat::RGBA32_UINT);
        case SpvImageFormatR8:       return static_cast<uint32_t>(TextureFormat::R8_UNORM);
        case SpvImageFormatRg8:      return static_cast<uint32_t>(TextureFormat::R8G8_UNORM);
        case SpvImageFormatR16ui:    return static_cast<uint32_t>(TextureFormat::R16_UINT);
        default: return 0;
    }
}

// (set << 32 | binding) of every image variable the module samples through a
// filtering-capable op (OpImageSample*, OpImageGather, OpImageDrefGather).
// texelFetch (OpImageFetch) does not mark. SPIRV-Reflect exposes declarations,
// not usage, so this is a direct instruction walk: result ids are chased
// through OpLoad / OpAccessChain / OpCopyObject / OpSampledImage back to the
// OpVariable that carries the DescriptorSet/Binding decorations. Opaque
// function parameters are followed through every call site's corresponding
// argument, including nested helpers and calls with different textures.
static std::unordered_set<uint64_t> CollectFilterSampledBindings(const uint32_t* words,
                                                                 size_t wordCount) {
    std::unordered_set<uint64_t> out;
    if (wordCount < 5) return out;

    constexpr uint16_t kOpFunction = 54;
    constexpr uint16_t kOpFunctionParameter = 55;
    constexpr uint16_t kOpFunctionEnd = 56;
    constexpr uint16_t kOpFunctionCall = 57;
    constexpr uint16_t kOpDecorate = 71;
    constexpr uint16_t kOpLoad = 61;
    constexpr uint16_t kOpAccessChain = 65;
    constexpr uint16_t kOpInBoundsAccessChain = 66;
    constexpr uint16_t kOpCopyObject = 83;
    constexpr uint16_t kOpSampledImage = 86;
    constexpr uint32_t kDecorationBinding = 33;
    constexpr uint32_t kDecorationDescriptorSet = 34;

    auto isSampleOp = [](uint16_t op) {
        // OpImageSample{Implicit,Explicit,DrefImplicit,DrefExplicit,
        // ProjImplicit,ProjExplicit,ProjDrefImplicit,ProjDrefExplicit}Lod +
        // OpImageGather / OpImageDrefGather.
        return (op >= 87 && op <= 94) || op == 96 || op == 97;
    };

    std::unordered_map<uint32_t, uint32_t> chain;      // result id -> source id
    std::unordered_map<uint32_t, uint32_t> varSet;     // variable id -> set
    std::unordered_map<uint32_t, uint32_t> varBinding; // variable id -> binding
    std::vector<uint32_t> sampledIds;                  // sampled-image operands
    std::unordered_map<uint32_t, std::vector<uint32_t>> parameters;
    std::unordered_map<uint32_t, std::vector<uint32_t>> parameterArguments;
    struct Call { uint32_t Function; size_t Arguments; size_t Count; };
    std::vector<Call> calls;
    uint32_t currentFunction = 0;


    for (size_t i = 5; i < wordCount;) {
        const uint32_t head = words[i];
        const uint16_t op = static_cast<uint16_t>(head & 0xFFFFu);
        const uint16_t len = static_cast<uint16_t>(head >> 16);
        if (len == 0 || i + len > wordCount) break;
        switch (op) {
        case kOpFunction:
            if (len >= 5) currentFunction = words[i + 2];
            break;
        case kOpFunctionParameter:
            if (len >= 3 && currentFunction != 0)
                parameters[currentFunction].push_back(words[i + 2]);
            break;
        case kOpFunctionEnd:
            currentFunction = 0;
            break;
        case kOpFunctionCall:
            if (len >= 4) calls.push_back({words[i + 3], i + 4, size_t(len - 4)});
            break;
        case kOpDecorate:
            if (len >= 4 && words[i + 2] == kDecorationDescriptorSet)
                varSet[words[i + 1]] = words[i + 3];
            else if (len >= 4 && words[i + 2] == kDecorationBinding)
                varBinding[words[i + 1]] = words[i + 3];
            break;
        case kOpLoad:
        case kOpCopyObject:
        case kOpAccessChain:
        case kOpInBoundsAccessChain:
            if (len >= 4) chain[words[i + 2]] = words[i + 3];
            break;
        case kOpSampledImage:
            if (len >= 5) chain[words[i + 2]] = words[i + 3]; // the image operand
            break;
        default:
            if (isSampleOp(op) && len >= 4) sampledIds.push_back(words[i + 3]);
            break;
        }
        i += len;
    }

    for (const auto& call : calls) {
        const auto it = parameters.find(call.Function);
        if (it == parameters.end()) continue;
        for (size_t argument = 0; argument < std::min(call.Count, it->second.size()); ++argument)
            parameterArguments[it->second[argument]].push_back(words[call.Arguments + argument]);
    }

    for (uint32_t sampledId : sampledIds) {
        std::vector<uint32_t> pending{sampledId};
        std::unordered_set<uint32_t> visited;
        while (!pending.empty()) {
            const uint32_t id = pending.back();
            pending.pop_back();
            if (!visited.insert(id).second) continue;
            if (const auto it = chain.find(id); it != chain.end())
                pending.push_back(it->second);
            if (const auto it = parameterArguments.find(id); it != parameterArguments.end())
                pending.insert(pending.end(), it->second.begin(), it->second.end());
            const auto setIt = varSet.find(id);
            const auto bindIt = varBinding.find(id);
            if (setIt != varSet.end() && bindIt != varBinding.end())
                out.insert((static_cast<uint64_t>(setIt->second) << 32) | bindIt->second);
        }
    }
    return out;
}

// spirv-reflect leaves `component` at its internal not-present sentinel when the
// variable carries no Component decoration, so the raw field is not a component
// index. SPIR-V's meaning of an absent decoration is component 0 — the variable
// starts at the first component of its location — and that is what the rest of
// the meta reads this field as: the serializer omits 0, and the validator's
// span check treats anything outside 0..3 as claiming the whole location, which
// would report a false overlap against a variable legally packed beside it.
static uint32_t StageIOComponent(const SpvReflectInterfaceVariable& v) {
    constexpr uint32_t kNoComponentDecoration = 0xFFFFFFFFu;
    return v.component == kNoComponentDecoration ? 0u : v.component;
}

static uint32_t ImageDim(const SpvReflectDescriptorBinding& b) {
    switch (b.image.dim) {
        case SpvDim1D: return 1;
        case SpvDim2D: return 2;
        case SpvDim3D: return 3;
        case SpvDimCube: return 2; // represent cube as 2D array for UI purposes
        default: return 2;
    }
}

bool ReflectSpirv(ShaderStageKind stage, const uint32_t* words, size_t wordCount,
                  const ReflectionOptions& opts,
                  StageReflectionResult& out,
                  std::string* outError) {
    if (!words || wordCount == 0) { if (outError) *outError = "Empty SPIR-V"; return false; }

    SpvReflectShaderModule module;
    SpvReflectResult res = spvReflectCreateShaderModule(wordCount * sizeof(uint32_t), words, &module);
    if (res != SPV_REFLECT_RESULT_SUCCESS) { if (outError) *outError = "spvReflectCreateShaderModule failed"; return false; }

    // Entry point
    out.Stage = stage;
    out.Meta.EntryPoint = module.entry_point_name ? module.entry_point_name : std::string("main");

    // Stage IO (inputs/outputs)
    uint32_t varCount = 0;
    spvReflectEnumerateInputVariables(&module, &varCount, nullptr);
    std::vector<SpvReflectInterfaceVariable*> inputs(varCount);
    spvReflectEnumerateInputVariables(&module, &varCount, inputs.data());
    for (auto* v : inputs) {
        if (!opts.IncludeBuiltins && (v->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN)) continue;
        StageIO io{}; io.Location = v->location; io.Component = StageIOComponent(*v); io.Name = v->name ? v->name : std::string(); io.Type = MakeTypeDesc(v->type_description);
        out.Meta.Inputs.push_back(std::move(io));
    }

    varCount = 0;
    spvReflectEnumerateOutputVariables(&module, &varCount, nullptr);
    std::vector<SpvReflectInterfaceVariable*> outputs(varCount);
    spvReflectEnumerateOutputVariables(&module, &varCount, outputs.data());
    for (auto* v : outputs) {
        if (!opts.IncludeBuiltins && (v->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN)) continue;
        StageIO io{}; io.Location = v->location; io.Component = StageIOComponent(*v); io.Name = v->name ? v->name : std::string(); io.Type = MakeTypeDesc(v->type_description);
        out.Meta.Outputs.push_back(std::move(io));
    }

    // Compute/Mesh sizes
    if (stage == ShaderStageKind::Compute) {
        StageMeta::LocalSize l{}; l.X = module.entry_points[0].local_size.x; l.Y = module.entry_points[0].local_size.y; l.Z = module.entry_points[0].local_size.z; out.Meta.ComputeLocalSize = l;
    }

    // Descriptor sets
    const std::unordered_set<uint64_t> filterSampled = CollectFilterSampledBindings(words, wordCount);
    uint32_t setCount = 0; spvReflectEnumerateDescriptorSets(&module, &setCount, nullptr);
    std::vector<SpvReflectDescriptorSet*> sets(setCount);
    spvReflectEnumerateDescriptorSets(&module, &setCount, sets.data());
    out.Sets.reserve(setCount);
    for (auto* s : sets) {
        DescriptorSetMeta sm{}; sm.Set = s->set;
        for (uint32_t i = 0; i < s->binding_count; ++i) {
            const auto* b = s->bindings[i];
            DescriptorBindingMeta bm{}; bm.Binding = b->binding; bm.Count = std::max(b->count, 1u);
            // Prefer the variable (instance) name; fall back to block name
            // or type name for unnamed buffer block instances (e.g.
            // `buffer Foo { ... };` with no GLSL instance name).
            const char* bindingName = b->name;
            if (!bindingName || bindingName[0] == '\0')
            {
                // Try block name (block type name for UBO/SSBO)
                if (b->block.name && b->block.name[0] != '\0')
                    bindingName = b->block.name;
                // Try type_description type_name
                else if (b->type_description && b->type_description->type_name && b->type_description->type_name[0] != '\0')
                    bindingName = b->type_description->type_name;
            }
            bm.Name = bindingName ? bindingName : std::string();
            bm.StagesMask |= StageToMask(stage);
            switch (b->descriptor_type) {
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: bm.Type = 0; break; // map later
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: bm.Type = 1; break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: bm.Type = 2; break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: bm.Type = 3; break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: bm.Type = 4; break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: bm.Type = 5; break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: bm.Type = 6; break;
                default: bm.Type = 255; break;
            }
            if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                BlockLayout bl{}; FillBlockLayout(b->block, bl); bm.Block = bl;
            }
            if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                // GLSL `readonly buffer` decorates the variable or the block;
                // either way the module's WGSL says var<storage, read> and
                // WebGPU layouts must agree.
                bm.ReadOnly = (b->decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0 ||
                              (b->block.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
            }
            if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                // `readonly image2D` carries the same NonWritable decoration and
                // becomes texture_storage_* <..., read> in WGSL. A layout that
                // claims write-only against it is rejected outright — the ocean
                // FFT reads its spectrum cascades this way.
                bm.ReadOnly = (b->decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0;
            }
            if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER || b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE || b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                DescriptorBindingMeta::ImageInfo info{}; info.Dim = ImageDim(*b); info.Arrayed = b->image.arrayed; info.Multisample = b->image.ms;
                info.Cube = b->image.dim == SpvDimCube;
                info.Depth = b->image.depth == 1;
                info.UnsignedInteger = b->type_description &&
                    ScalarBaseType(*b->type_description) == BaseType::UInt;
                info.Filtered = filterSampled.count(
                    (static_cast<uint64_t>(s->set) << 32) | b->binding) != 0;
                if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                    info.Format = ImageFormatToEngineFormat(static_cast<SpvImageFormat>(b->image.image_format));
                bm.Image = info;
            }
            sm.Bindings.push_back(std::move(bm));
        }
        out.Sets.push_back(std::move(sm));
    }
    // Specialization constants
    uint32_t scCount = 0; spvReflectEnumerateSpecializationConstants(&module, &scCount, nullptr);
    if (scCount > 0) {
        std::vector<SpvReflectSpecializationConstant*> scs(scCount);
        spvReflectEnumerateSpecializationConstants(&module, &scCount, scs.data());
        for (auto* sc : scs) {
            SpecConstantMeta scm{}; scm.Id = sc->constant_id; scm.Name = std::string();
            // Type information not provided by this SPIRV-Reflect struct in our version; leave default
            out.SpecConstants.push_back(std::move(scm));
        }
    }


    // Push constants
    uint32_t pcrCount = 0; spvReflectEnumeratePushConstantBlocks(&module, &pcrCount, nullptr);
    std::vector<SpvReflectBlockVariable*> pcrs(pcrCount);
    spvReflectEnumeratePushConstantBlocks(&module, &pcrCount, pcrs.data());
    for (auto* p : pcrs) {
        PushConstantRangeMeta r{}; r.Name = p->name ? p->name : std::string("PushConstants"); r.Size = p->size; r.StagesMask = StageToMask(stage);
        FillBlockLayout(*p, r.Block);
        out.Pcr.push_back(std::move(r));
    }

    spvReflectDestroyShaderModule(&module);
    return true;
}

#else

bool ReflectSpirv(ShaderStageKind, const uint32_t*, size_t, const ReflectionOptions&, StageReflectionResult&, std::string*) {
    return false;
}

#endif

ShaderMeta MergeStages(const std::vector<StageReflectionResult>& stages) {
    ShaderMeta meta{}; meta.Version = 1;
    // Merge entry points and stages map
    for (const auto& s : stages) {
        switch (s.Stage) {
            case ShaderStageKind::Vertex:   meta.EntryPoints["vs"] = s.Meta.EntryPoint; meta.Stages["vs"] = s.Meta; break;
            case ShaderStageKind::Fragment: meta.EntryPoints["fs"] = s.Meta.EntryPoint; meta.Stages["fs"] = s.Meta; break;
            case ShaderStageKind::Compute:  meta.EntryPoints["cs"] = s.Meta.EntryPoint; meta.Stages["cs"] = s.Meta; break;
            case ShaderStageKind::Geometry: meta.EntryPoints["gs"] = s.Meta.EntryPoint; meta.Stages["gs"] = s.Meta; break;
            case ShaderStageKind::Mesh:     meta.EntryPoints["ms"] = s.Meta.EntryPoint; meta.Stages["ms"] = s.Meta; break;
            default: break;
        }
        // Merge push constants (coalesce by name; OR stages)
        for (const auto& r : s.Pcr) {
            bool merged = false;
            for (auto& existing : meta.PushConstants) {
                if (existing.Name == r.Name && existing.Size == r.Size) {
                    existing.StagesMask |= r.StagesMask;
                    merged = true; break;
                }
            }
            if (!merged) meta.PushConstants.push_back(r);
        }
		// Merge descriptor sets (coalesce by set and binding; validate type/count)
		for (const auto& set : s.Sets) {
			auto* dstSet = [&]() -> DescriptorSetMeta* {
				for (auto& ds : meta.Sets)
				{
					if (ds.Set == set.Set)
						return &ds;
				}
				meta.Sets.push_back(DescriptorSetMeta{ set.Set, {} });
				return &meta.Sets.back();
			}();
			for (const auto& b : set.Bindings) {
                bool mergedB = false;
                for (auto& eb : dstSet->Bindings) {
                    if (eb.Binding == b.Binding) {
                        // Validate type/count; record conflict in requirements if mismatch
                        if (eb.Type != b.Type || eb.Count != b.Count) {
                            meta.Requirements.push_back(
                                std::string("conflict: set=") + std::to_string(set.Set) +
                                ", binding=" + std::to_string(b.Binding) +
                                ", type/count mismatch (lhsType=" + std::to_string(eb.Type) +
                                ", rhsType=" + std::to_string(b.Type) +
                                ", lhsCount=" + std::to_string(eb.Count) +
                                ", rhsCount=" + std::to_string(b.Count) + ")");
                        }
                        // OR stages regardless to reflect visibility across stages
                        eb.StagesMask |= b.StagesMask;
                        // Read-only only if every stage's usage is read-only:
                        // a writable layout can't serve a stage that writes.
                        eb.ReadOnly = eb.ReadOnly && b.ReadOnly;
                        // Filtered if ANY stage samples through it — the one
                        // layout must satisfy the strictest consumer.
                        if (eb.Image && b.Image && b.Image->Filtered)
                            eb.Image->Filtered = true;
                        mergedB = true; break;
                    }
                }
                if (!mergedB) dstSet->Bindings.push_back(b);
            }
        }
    }
    // Merge specialization constants (unique by id)
    {
        std::unordered_set<uint32_t> seen;
        for (const auto& existing : meta.SpecConstants) seen.insert(existing.Id);
        for (const auto& s : stages) {
            for (const auto& sc : s.SpecConstants) {
                if (seen.insert(sc.Id).second) {
                    meta.SpecConstants.push_back(sc);
                }
            }
        }
    }
    // Assign stable ids to push constant ranges (by current order)
    for (uint32_t i = 0; i < meta.PushConstants.size(); ++i) {
        meta.PushConstants[i].Id = i;
    }

    // Precompute per-set layout hash so MaterialBinder's per-pass cache
    // lookup can skip the layout-build + walk-and-hash on every draw.
    for (auto& set : meta.Sets) {
        FinalizeSetLayout(set);
    }

    return meta;
}

}} // namespace GameEngine::Rendering

