#pragma once

// CBTKernelSet — creates one compute pipeline per kernel and owns the shared
// descriptor-set layout they all bind. Mirrors the K_* ids in Shaders/cbt_kernels.comp
// exactly.
//
// Two shapes, one per shader arm. Where specialization constants exist the set loads
// cbt_kernels.comp.spv ONCE and specializes constant_id=0 kCBTKernelCount ways. WebGPU
// has no specialization constants (and ingests no SPIR-V), so there the cook emits one
// WGSL program per kernel — cbt_kernels_<i>.comp.wgsl — and the set loads each in turn.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

namespace GameEngine::Rendering
{
class IDevice;
struct DescriptorSetLayoutDesc;
}

namespace GameEngine::Rendering
{
enum class ShaderSourceKind : uint8_t;
}

namespace GameEngine::CBTTerrain
{

// Kernel ids — MUST match the K_* defines in cbt_kernels.comp.
enum class CBTKernel : uint32_t
{
    Reset = 0,
    Classify = 1,
    PrepareIndirect = 2,
    Split = 3,
    Allocate = 4,
    Bisect = 5,
    PropagateBisect = 6,
    PrepareSimplify = 7,
    Simplify = 8,
    PropagateSimplify = 9,
    ReducePrePass = 10,
    ReduceFirstPass = 11,
    ReduceSecondPass = 12,
    BisectorIndexation = 13,
    PrepareBisectorIndirect = 14,
    Validate = 15,
    VertexEval = 16,
    RegionFreeClear = 17, // far-field drain: free selected root subtrees back to base
    RegionFreeSeed = 18,  // far-field drain: reseed each freed root's base bisector
    RegionFreeLink = 19,  // far-field drain: wire reseeded roots' base neighbor links
    NeighborCopy = 20,    // scatter CURRENT -> NEXT neighbors over the live set, before Bisect
    ValidateCompactStream = 21, // debug: IndicesAll is exactly the live set, at the update head
    HeightRangeBuild = 22, // content-aware split: one level of the height-range pyramid
    Count = 23
};
inline constexpr uint32_t kCBTKernelCount = static_cast<uint32_t>(CBTKernel::Count);

// The bindings a cooked kernel declares in ONE descriptor group, parsed from its WGSL text.
// Exposed for the layout test: the filtered per-kernel layout is built from this, and reading a
// binding number without its group silently grants kernels a buffer they never declared.
std::vector<uint32_t> DeclaredBindings(const std::vector<uint8_t>& wgsl, uint32_t group);

// Where a kernel set's pipelines are (CBTKernelSet::Poll).
enum class CBTKernelSetState : uint8_t
{
    Pending, // requested, some pipeline still building
    Ready,   // every kernel pipeline is live
    Failed,  // a program was missing or a pipeline failed to build
};

class CBTKernelSet
{
  public:
    CBTKernelSet() = default;
    ~CBTKernelSet();

    CBTKernelSet(const CBTKernelSet&) = delete;
    CBTKernelSet& operator=(const CBTKernelSet&) = delete;

    // Loads the kernel program(s) from `shaderDir` and requests every kernel pipeline
    // from the device (IDevice::RequestComputePipeline), which builds them off the
    // calling thread when it has a build dispatcher. Then polls once: Ready on return
    // when the builds ran inline. Returns false if a program is missing or a pipeline
    // has failed. The heap width follows the device: a device without 64-bit integers
    // gets the narrow-heap (GE_CBT_HEAP32) program. programFileOverride replaces the
    // SPIR-V blob file name; the tests use it to run the narrow-heap arm on a device
    // that has int64. Ignored where the device ingests per-kernel WGSL, whose names
    // are fixed.
    bool Initialize(Rendering::IDevice& device, const std::filesystem::path& shaderDir,
                    const char* programFileOverride = nullptr);

    // Pending until every kernel pipeline Initialize requested has built, then Ready
    // with GetPipeline valid for every kernel. Failed (once, logged with the kernel's
    // name, and the set shuts down) when a build failed. Cheap: one cache probe per
    // kernel while pending, none once Ready.
    CBTKernelSetState Poll();

    // True between Initialize and Shutdown.
    bool IsRequested() const { return m_Device != nullptr; }

    // The kernel program `shaderDir` must hold for a device: one specializable SPIR-V
    // blob (wide or narrow heap), or the per-kernel WGSL a device with no specialization
    // constants ingests. Callers probe for it to locate the directory in the first place,
    // so the name lives here with the loader.
    static std::string KernelProgramFileName(Rendering::ShaderSourceKind source, bool narrowHeap,
                                             uint32_t kernelIndex);

    // Whether the loaded kernels carry the narrow (u32) heap. CBTResources mirrors the
    // texture-ring shape of that arm, and the depth cap follows the heap's own range.
    bool IsNarrowHeap() const { return m_NarrowHeap; }
    void Shutdown();

    bool IsReady() const { return m_Ready; }
    Rendering::PipelineHandle GetPipeline(CBTKernel kernel) const;

    // The descriptor-set layout every kernel binds at set 0 (14 storage buffers).
    // `narrowHeap` selects the collapsed texture ring that arm declares (CBTTextureRingFor).
    static Rendering::DescriptorSetLayoutDesc MakeDescriptorSetLayout(bool narrowHeap);

    // The layout ONE kernel needs. On a profile with a per-stage storage-buffer limit below what
    // the family needs between them, each kernel carries only the bindings its own cooked program
    // declares, and its descriptor set must match that exactly. Empty on the profile where every
    // kernel shares the full layout.
    const Rendering::DescriptorSetLayoutDesc* KernelLayout(uint32_t kernelIndex) const;
    bool UsesPerKernelLayouts() const { return !m_KernelLayouts.empty(); }

  private:
    Rendering::IDevice* m_Device = nullptr;
    std::vector<Rendering::DescriptorSetLayoutDesc> m_KernelLayouts;
    std::array<Rendering::ComputePipelineId, kCBTKernelCount> m_PipelineIds{};
    std::array<Rendering::PipelineHandle, kCBTKernelCount> m_Pipelines{};
    bool m_Ready = false;
    bool m_NarrowHeap = false;
};

} // namespace GameEngine::CBTTerrain
