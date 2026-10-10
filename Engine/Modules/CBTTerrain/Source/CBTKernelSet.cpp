#include "CBTTerrain/CBTKernelSet.h"

#include <algorithm>

#include "CBTTerrain/CBTLayout.h"

#include "AssetCore/SharedFileRead.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Logger/Logger.h"

#include <cstdint>
#include <string_view>
#include <memory>
#include <utility>

namespace GameEngine::CBTTerrain
{

using namespace GameEngine::Rendering;

namespace
{
const char* KernelDebugName(CBTKernel k)
{
    switch (k)
    {
    case CBTKernel::Reset: return "CBT.Reset";
    case CBTKernel::Classify: return "CBT.Classify";
    case CBTKernel::PrepareIndirect: return "CBT.PrepareIndirect";
    case CBTKernel::Split: return "CBT.Split";
    case CBTKernel::Allocate: return "CBT.Allocate";
    case CBTKernel::Bisect: return "CBT.Bisect";
    case CBTKernel::PropagateBisect: return "CBT.PropagateBisect";
    case CBTKernel::PrepareSimplify: return "CBT.PrepareSimplify";
    case CBTKernel::Simplify: return "CBT.Simplify";
    case CBTKernel::PropagateSimplify: return "CBT.PropagateSimplify";
    case CBTKernel::ReducePrePass: return "CBT.ReducePrePass";
    case CBTKernel::ReduceFirstPass: return "CBT.ReduceFirstPass";
    case CBTKernel::ReduceSecondPass: return "CBT.ReduceSecondPass";
    case CBTKernel::BisectorIndexation: return "CBT.BisectorIndexation";
    case CBTKernel::PrepareBisectorIndirect: return "CBT.PrepareBisectorIndirect";
    case CBTKernel::Validate: return "CBT.Validate";
    case CBTKernel::VertexEval: return "CBT.VertexEval";
    case CBTKernel::RegionFreeClear: return "CBT.RegionFreeClear";
    case CBTKernel::RegionFreeSeed: return "CBT.RegionFreeSeed";
    case CBTKernel::RegionFreeLink: return "CBT.RegionFreeLink";
    case CBTKernel::NeighborCopy: return "CBT.NeighborCopy";
    case CBTKernel::ValidateCompactStream: return "CBT.ValidateCompactStream";
    default: return "CBT.Unknown";
    }
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    // Kernel .spv blobs get rewritten by external shader recompiles while the
    // engine may be re-reading them; shared-read keeps both sides unblocked.
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(path, bytes))
        return {};
    return bytes;
}
} // namespace

CBTKernelSet::~CBTKernelSet()
{
    Shutdown();
}

DescriptorSetLayoutDesc CBTKernelSet::MakeDescriptorSetLayout(bool narrowHeap)
{
    const uint32_t textureRing = CBTTextureRingFor(narrowHeap);
    DescriptorSetLayoutDesc dsl{};
    dsl.debugName = "CBT.KernelSet.DSL";
    dsl.bindings.reserve(kCBTDescriptorBindingCount);
    // Bindings 0..13: the SoA storage buffers.
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
    {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Binding 14: per-frame camera + terrain params (Classify + VertexEval read it).
    {
        DescriptorBinding b{};
        b.binding = kCBTFrameParamsBinding;
        b.type = DescriptorType::UniformBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Binding 15: the terrain heightmap ring (combined image samplers) VertexEval
    // samples. CBTTextureRingFor elements; the CPU rewrites only the current frame
    // slot so a mid-session handle change never touches an in-flight element (see
    // CBTTextureRingFor for why the narrow arm needs no ring).
    // VertexEval taps this through textureLod with a linear sampler, so the binding
    // is a filterable float: a hand-built layout must say so, because backends that
    // derive the sample type from the layout (WebGPU) default a compute-stage image
    // to unfilterable and then reject the filtering sampler the tap pairs it with.
    {
        DescriptorBinding b{};
        b.binding = kCBTHeightTextureBinding;
        b.type = DescriptorType::CombinedImageSampler;
        b.count = textureRing;
        b.shaderStages = kShaderStageCompute;
        b.imageFilterableFloat = true;
        dsl.bindings.push_back(b);
    }
    // Binding 16: the editable sphere sculpt atlas SSBO (planet editing, plan
    // §planet-editing). Host-visible, ring-buffered; only VertexEval reads it (gated on
    // pc.sphereSculptEnabled). A single storage buffer covering the whole ring.
    {
        DescriptorBinding b{};
        b.binding = kCBTSphereSculptBinding;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Binding 17: the resident-window atlas indirection table SSBO (Phase E). Host-visible
    // ring; only VertexEval reads it (gated on the frame-params atlas-enabled flag). A single
    // storage buffer covering the whole ring (ring * kAtlasMaxTiles rows).
    {
        DescriptorBinding b{};
        b.binding = kCBTAtlasRowsBinding;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Binding 18: the atlas height texture ring (combined image samplers), rebound per frame
    // slot exactly like binding 15. CBTTextureRingFor elements; VertexEval samples the
    // current frame slot only.
    {
        DescriptorBinding b{};
        b.binding = kCBTAtlasHeightBinding;
        b.type = DescriptorType::CombinedImageSampler;
        b.count = textureRing;
        b.shaderStages = kShaderStageCompute;
        b.imageFilterableFloat = true;
        dsl.bindings.push_back(b);
    }
    // Binding 19: the coarse height field ring (out-of-window fallback), same shape as 18.
    {
        DescriptorBinding b{};
        b.binding = kCBTAtlasCoarseBinding;
        b.type = DescriptorType::CombinedImageSampler;
        b.count = textureRing;
        b.shaderStages = kShaderStageCompute;
        b.imageFilterableFloat = true;
        dsl.bindings.push_back(b);
    }
    // Binding 20: the sphere sculpt PAGE TABLE SSBO (planet editing v2). Host-visible ring, same
    // shape as binding 16; only VertexEval reads it (gated on pc.sphereSculptEnabled).
    {
        DescriptorBinding b{};
        b.binding = kCBTSphereSculptPageTableBinding;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Binding 21: the height-range pyramid (content-aware split). The narrow-heap kernels keep the
    // screen-space rule alone and do not declare it.
    if (!narrowHeap)
    {
        DescriptorBinding b{};
        b.binding = kCBTHeightRangeBinding;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(b);
    }
    // Bindings 22/23: the paged height resolve's page table (host-visible SSBO ring) and page cache
    // texture ring. Wide arm only: the narrow arm's VertexEval already binds the WebGPU per-stage
    // storage-buffer limit, so it keeps the unified and atlas heights.
    if (!narrowHeap)
    {
        DescriptorBinding table{};
        table.binding = kCBTPageTableBinding;
        table.type = DescriptorType::StorageBuffer;
        table.count = 1;
        table.shaderStages = kShaderStageCompute;
        dsl.bindings.push_back(table);
        DescriptorBinding cache{};
        cache.binding = kCBTPageCacheBinding;
        cache.type = DescriptorType::CombinedImageSampler;
        cache.count = textureRing;
        cache.shaderStages = kShaderStageCompute;
        cache.imageFilterableFloat = true;
        dsl.bindings.push_back(cache);
    }
    return dsl;
}

// The bindings ONE cooked kernel actually declares in the CBT descriptor set, read out of its
// WGSL. Not EMSCRIPTEN-guarded: it is pure text parsing, so a native test can pin it.
//
// Every kernel shares one hand-authored layout, which names all 17 storage buffers the family
// needs between them. WebGPU guarantees only 10 per compute stage, so that shared layout is
// rejected outright and every CBT kernel pipeline fails to create — no compute, no terrain
// geometry, a black terrain rather than a degraded one. No single kernel needs more than nine:
// the count is an artefact of sharing one layout, not of what any kernel reads.
//
// The cooked program is the authority on its own bindings (it is one kernel per program here,
// not a specialization switch), so the layout is the shared one filtered to the bindings its
// text declares. Filtering rather than re-deriving keeps the types, stages and debug names in
// the single hand-authored place.
//
// GROUP-AWARE, and that is the point: the cook rewrites the push-constant block to
// `@group(3) @binding(0)`, so a scan that reads binding numbers without their group sees a
// "binding 0" in every kernel that takes push constants — and binding 0 of THIS set is the
// HeapID SSBO. Four kernels were handed a storage buffer they never read, spending one of the
// ten slots the filtering exists to conserve.
// The compute kernels' descriptor set: CBT_DESCRIPTOR_SET in cbt_layout.glsl, and the index
// CBTInstance binds at (BindDescriptorSet(0, ...)). The push-constant block the cook appends
// lives in its own group, which is exactly what this must not confuse for a set-0 binding.
inline constexpr uint32_t kCBTComputeDescriptorSet = 0u;

std::vector<uint32_t> DeclaredBindings(const std::vector<uint8_t>& wgsl, uint32_t group)
{
    const std::string text(reinterpret_cast<const char*>(wgsl.data()), wgsl.size());
    std::vector<uint32_t> bindings;
    const std::string groupNeedle = "@group(" + std::to_string(group) + ")";
    for (size_t at = text.find(groupNeedle); at != std::string::npos;
         at = text.find(groupNeedle, at + 1))
    {
        // `@binding(N)` must be the next attribute on this declaration: scan only to the end of
        // the line so a later declaration's binding cannot be attributed to this group.
        const size_t lineEnd = text.find('\n', at);
        const size_t bindAt = text.find("@binding(", at);
        if (bindAt == std::string::npos || (lineEnd != std::string::npos && bindAt > lineEnd))
            continue;
        const size_t start = bindAt + std::string("@binding(").size();
        const size_t end = text.find(')', start);
        if (end == std::string::npos)
            break;
        try
        {
            bindings.push_back(static_cast<uint32_t>(std::stoul(text.substr(start, end - start))));
        }
        catch (const std::exception&)
        {
        }
    }
    return bindings;
}

std::string CBTKernelSet::KernelProgramFileName(ShaderSourceKind source, bool narrowHeap,
                                                uint32_t kernelIndex)
{
    if (source == ShaderSourceKind::Wgsl)
    {
        // The WGSL cook has no specialization constants: one program per kernel,
        // and WGSL has no 64-bit integers, so the cook is always the narrow arm.
        return "cbt_kernels_" + std::to_string(kernelIndex) + ".comp.wgsl";
    }
    (void)kernelIndex; // one blob serves every kernel; the index is a specialization
    return narrowHeap ? "cbt_kernels_heap32.comp.spv" : "cbt_kernels.comp.spv";
}

const DescriptorSetLayoutDesc* CBTKernelSet::KernelLayout(uint32_t kernelIndex) const
{
    if (kernelIndex >= m_KernelLayouts.size())
        return nullptr;
    return &m_KernelLayouts[kernelIndex];
}

bool CBTKernelSet::Initialize(IDevice& device, const std::filesystem::path& shaderDir,
                              const char* programFileOverride)
{
    Shutdown();
    m_Device = &device;
    const RenderingDeviceCapabilities& caps = device.GetCapabilities();
    const ShaderSourceKind source = device.PreferredShaderSource();
    const bool perKernelPrograms = source == ShaderSourceKind::Wgsl;
    // The narrow arm is what a device without int64 can run; a test may also ask for it
    // by name on a device that has int64.
    m_NarrowHeap = CBTDeviceRunsNarrowArm(perKernelPrograms, caps.supportsShaderInt64) ||
                   (programFileOverride != nullptr &&
                    std::string_view(programFileOverride).find("heap32") != std::string_view::npos);

    // Nothing else tells a user why their terrain stopped refining: the narrow arm's u32 heap
    // caps subdivision far below the wide arm's, and the terrain simply renders coarser. Say
    // which arm is running, why, and what it costs. The WGSL cook is narrow by construction
    // and stays quiet; every SPIR-V host on this arm gets the line.
    if (m_NarrowHeap && !perKernelPrograms)
    {
        Logger::Log::Warning(
            "CBTKernelSet: running the narrow-heap (u32) CBT kernels — {}. Terrain subdivision "
            "is capped at {} instead of the wide arm's {}, so the finest facet is coarser.",
            caps.supportsShaderInt64 ? "requested by program name"
                                     : "this device reports no 64-bit shader integer support",
            kHeap32DecodeSubdiv, kMaxDecodeSubdiv);
    }

    // The shared layout names every storage buffer the family needs between them. A
    // device whose per-stage budget is below that cannot intern it at all; each kernel
    // then carries only the bindings its own program declares, and CBTResources builds a
    // descriptor set per kernel to match. Declared bindings are read from WGSL text, so
    // the filtered shape exists only where per-kernel WGSL programs do.
    const DescriptorSetLayoutDesc sharedLayout = MakeDescriptorSetLayout(m_NarrowHeap);
    uint32_t sharedStorageBuffers = 0;
    for (const DescriptorBinding& b : sharedLayout.bindings)
        if (b.type == DescriptorType::StorageBuffer)
            ++sharedStorageBuffers;
    const bool filterLayouts =
        caps.maxPerStageStorageBuffers != 0 && caps.maxPerStageStorageBuffers < sharedStorageBuffers;
    if (filterLayouts && !perKernelPrograms)
    {
        Logger::Log::Error("CBTKernelSet: the device binds {} storage buffers per stage but the "
                           "kernel family needs {}, and only per-kernel WGSL programs can be "
                           "filtered to fit",
                           caps.maxPerStageStorageBuffers, sharedStorageBuffers);
        return false;
    }

    const DescriptorSetLayoutId dslId =
        filterLayouts ? DescriptorSetLayoutId{} : device.InternDescriptorSetLayout(sharedLayout);

    std::shared_ptr<const std::vector<uint8_t>> sharedBlob;
    if (!perKernelPrograms)
    {
        // One blob for every kernel: the specialization is what distinguishes them.
        const std::filesystem::path spvPath =
            shaderDir / (programFileOverride ? std::string(programFileOverride)
                                             : KernelProgramFileName(source, m_NarrowHeap, 0));
        std::vector<uint8_t> spirv = ReadFileBytes(spvPath);
        if (spirv.empty())
        {
            Logger::Log::Error("CBTKernelSet: failed to load SPIR-V at {}", spvPath.string());
            return false;
        }
        sharedBlob = std::make_shared<const std::vector<uint8_t>>(std::move(spirv));
    }

    // Build the typed ComputePipelineDesc directly (the Intern* / GetOrCreate*
    // production path). The convenience CreatePipeline(PipelineDesc) drops the
    // specialization constants in its translator, which would collapse every
    // kernel to KERNEL=0; ComputePipelineDesc::Specialization is carried into
    // the pipeline's ContentHash, so distinct KERNEL ids intern distinctly (kCBTKernelCount of
    // them, one specialization each). Distinct per-kernel PROGRAMS intern distinctly on
    // their own, so the no-specialization arm needs nothing extra.
    if (filterLayouts)
        m_KernelLayouts.resize(kCBTKernelCount);
    for (uint32_t i = 0; i < kCBTKernelCount; ++i)
    {
        ComputePipelineDesc cd{};
        if (perKernelPrograms)
        {
            const std::filesystem::path programPath =
                shaderDir / KernelProgramFileName(source, m_NarrowHeap, i);
            std::vector<uint8_t> program = ReadFileBytes(programPath);
            if (program.empty())
            {
                Logger::Log::Error("CBTKernelSet: failed to load kernel program at {}",
                                   programPath.string());
                Shutdown();
                return false;
            }
            if (filterLayouts)
            {
                const std::vector<uint32_t> declared =
                    DeclaredBindings(program, kCBTComputeDescriptorSet);
                DescriptorSetLayoutDesc kernelLayout = sharedLayout;
                std::vector<DescriptorBinding> used;
                used.reserve(declared.size());
                for (const DescriptorBinding& b : kernelLayout.bindings)
                {
                    if (std::find(declared.begin(), declared.end(), b.binding) != declared.end())
                        used.push_back(b);
                }
                kernelLayout.bindings = std::move(used);
                m_KernelLayouts[i] = kernelLayout;
                cd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(kernelLayout));
            }
            else
            {
                cd.DescriptorSetLayouts.push_back(dslId);
            }
            cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(program));
        }
        else
        {
            SpecializationConstants spec;
            spec.AddConstant(0u, i, "KERNEL");
            cd.ComputeShader = sharedBlob;
            cd.Specialization = std::move(spec);
            cd.DescriptorSetLayouts.push_back(dslId);
        }
        cd.PushConstants.Offset = 0;
        cd.PushConstants.Size = static_cast<uint32_t>(sizeof(CBTPushConstants));
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = KernelDebugName(static_cast<CBTKernel>(i));

        m_PipelineIds[i] = device.InternComputePipeline(cd);
        device.RequestComputePipeline(m_PipelineIds[i]);
    }

    return Poll() != CBTKernelSetState::Failed;
}

CBTKernelSetState CBTKernelSet::Poll()
{
    if (m_Ready)
        return CBTKernelSetState::Ready;
    if (m_Device == nullptr)
        return CBTKernelSetState::Failed;
    bool pending = false;
    for (uint32_t i = 0; i < kCBTKernelCount; ++i)
    {
        // A request, not a probe: it is free while the build is queued or running,
        // and it asks again for a build the device cancelled.
        const PipelineBuildState state = m_Device->RequestComputePipeline(m_PipelineIds[i]);
        if (state == PipelineBuildState::Failed)
        {
            Logger::Log::Error("CBTKernelSet: failed to create pipeline for kernel {} ({})", i,
                               KernelDebugName(static_cast<CBTKernel>(i)));
            Shutdown();
            return CBTKernelSetState::Failed;
        }
        pending |= state == PipelineBuildState::Pending;
    }
    if (pending)
        return CBTKernelSetState::Pending;
    for (uint32_t i = 0; i < kCBTKernelCount; ++i)
        m_Pipelines[i] = m_Device->TryGetWarmComputePipeline(m_PipelineIds[i]);
    m_Ready = true;
    Logger::Log::Info("CBTKernelSet: created {} compute pipelines ({} heap{})", kCBTKernelCount,
                      m_NarrowHeap ? "narrow" : "wide", UsesPerKernelLayouts() ? ", per-kernel layouts" : "");
    return CBTKernelSetState::Ready;
}

void CBTKernelSet::Shutdown()
{
    m_KernelLayouts.clear();
    m_NarrowHeap = false;
    // Pipelines come from the device pipeline cache (RequestComputePipeline,
    // then TryGetWarmComputePipeline), which owns them and destroys them at
    // device shutdown. Only the references are dropped here.
    for (auto& p : m_Pipelines)
        p = {};
    m_PipelineIds = {};
    m_Ready = false;
    m_Device = nullptr;
}

PipelineHandle CBTKernelSet::GetPipeline(CBTKernel kernel) const
{
    const uint32_t idx = static_cast<uint32_t>(kernel);
    if (idx >= kCBTKernelCount)
        return {};
    return m_Pipelines[idx];
}

} // namespace GameEngine::CBTTerrain
