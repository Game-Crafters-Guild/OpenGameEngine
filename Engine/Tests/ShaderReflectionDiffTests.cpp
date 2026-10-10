// Reflection-diff guard (roadmap S1.2, finding SHD-2): lock the C++ GPU-struct
// mirrors against the *compiled* shader reflection carried in each .shaderpkg,
// so a silent C++/GLSL layout drift becomes a red test instead of a garbage
// frame. History: the ViewParams 240-vs-304 landmine and the GPUCullingData
// "MUST match the shader layout exactly!" comment that shipped with zero
// asserts. The C++ side now static_asserts its own offsets/sizes (GPUScene.h);
// this test closes the other half — that the shader the GPU actually runs
// agrees with those numbers.
//
// ---------------------------------------------------------------------------
// WHAT THE REFLECTION ACTUALLY CAPTURES (read before adding rows)
// ---------------------------------------------------------------------------
// The package meta is built by ShaderReflection.cpp::FillBlockLayout, which
// records only the TOP-LEVEL members of each UBO/SSBO block. Two shapes:
//
//   1. Struct-WRAPPED block, e.g. `buffer CullingDataBuffer { GPUCullingData
//      cullingData; };` or `buffer InstanceBuffer { GPUInstance instances[]; };`
//      The block has ONE top-level member (`cullingData` / `instances`). Its
//      reflected `Offset` and `Size` are captured; an array member also carries
//      `ArrayStride`, the element stride the shader indexes rows by, and that is
//      what this test diffs for arrays (a runtime array's `Size` is one
//      element's unpadded bytes, and 0 for a scalar or vector element). The
//      nested struct fields live under `Member.Type.StructMembers` with NAMES
//      only — their offset/size are NOT populated. So for these blocks the
//      reflection locks the row stride, NOT the interior field offsets.
//      Interior offsets of these structs stay locked C++-side by the
//      static_asserts in GPUScene.h; this test locks the size the shader agrees
//      on (the exact drift class SHD-2 cites: 240-vs-304, 652-vs-656).
//
//   2. INLINE block, e.g. `uniform ViewParamsBlock { mat4 uInvProj; ... }`.
//      The fields are top-level block members, so their interior Offset/Size
//      ARE reflected and this test diffs them directly.
//
// Capturing interior offsets for shape (1) would require a production change to
// make FillBlockLayout recurse into nested struct members — out of scope for
// this test-only slice. Flagged as a follow-up.
//
// ---------------------------------------------------------------------------
// COVERAGE GAPS (deliberately not registered)
// ---------------------------------------------------------------------------
//   * MaterialData (Shaders/Includes/material_params.glsl + MaterialParamsLayout.h):
//     declared only inside the RUNTIME-composed adapter_forward.glsl. No shader
//     in CompileShaderPkgs' output includes material_params.glsl, so there is
//     no packaged reflection to diff against. Not covered here; adding a
//     packaging step is out of scope for this slice.
//   * Canonical ViewParamsUBO (Shaders/Includes/view_params.glsl, binding
//     7 — the block that had the 240-vs-304 history): only in the runtime-composed
//     adapters, never packaged. Since S1.3 every declaration — canonical, C++
//     mirror (Rendering::ViewParamsUBO), and the packaged prefixes — derives from
//     one field list (view_params_fields.glsl), so locking a packaged prefix
//     against the shared C++ mirror transitively locks the canonical block too.
//     The packaged clustered_light_cull / forwardplus_lighting declare the 160 B
//     core prefix (ge_invProj/ge_view/ge_nearFar/ge_cameraPosWS) at binding 5;
//     heat_distortion declares the 240 B prefix (adds ge_screenSize/ge_viewProj).
//     The ViewParams rows below diff those interior offsets (shape 2) against the
//     shared mirror.
//
// ---------------------------------------------------------------------------
// RUN MODEL
// ---------------------------------------------------------------------------
// Packages resolve CWD-independently: LoadShaderPkg tries the literal path,
// then the resolver EngineTestShaderSetup.cpp installs (ENGINE_TEST_BUILD_DIR,
// where CompileShaderPkgs emits). A MISSING package GTEST_SKIPs that row
// (matches HzbCullingComputeTests); a package that IS present but is missing the
// named BLOCK or MEMBER is a FAILURE — that is the drift signal. Adding coverage
// is one table row.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// The packaged Forward+/post-FX shaders declare inline prefixes of the shared
// per-view block (block `ViewParamsBlock`, instance `ViewParams`) — 160 B core
// at binding 5 (clustered_light_cull / forwardplus_lighting), 240 B at binding 2
// (heat_distortion). Both derive from view_params_fields.glsl, so the rows below
// diff the reflected interior offsets straight against the shared C++ mirror
// Rendering::ViewParamsUBO (ViewParamsLayout.h, std140 offsets static_asserted).

// The scatter/gen indirect record stream is literally VkDrawIndexedIndirectCommand[]
// (5x uint32 = 20 B: indexCount, instanceCount, firstIndex, vertexOffset,
// firstInstance). Mirrors BatchScatterComputeTests' kDrawRecordUints precedent
// rather than pulling <vulkan/vulkan.h> in for a single sizeof.
constexpr std::size_t kVkDrawIndexedIndirectCommandBytes = 5u * sizeof(uint32_t);

// One registration row. Pkg is resolved relative to the build root (test cwd).
// Block is the reflected binding (or push-constant) name; Member is a top-level
// member within that block. Offset/Size are the C++ mirror's expectation,
// always via offsetof/sizeof — never a literal; for an array member Size is
// one element's sizeof, the stride the writer indexes rows by. Held as size_t
// so offsetof/sizeof land without a narrowing conversion; compared against the
// reflected uint32_t fields below (both unsigned, no signedness surprise).
struct MemberExpectation
{
    const char* Pkg;
    const char* Block;
    const char* Member;
    std::size_t Offset;
    std::size_t Size;
};

const std::vector<MemberExpectation>& ExpectationTable()
{
    // clang-format off
    static const std::vector<MemberExpectation> kRows = {
        // -- GPUInstance: 240 B std430 element stride. Struct-wrapped runtime
        //    array `InstanceBuffer { GPUInstance instances[]; }` — sizeof is both
        //    the reflected element Size and the ArrayStride, the struct being
        //    16-byte granular (interior offsets locked by GPUScene.h
        //    static_asserts). Present in every GPU-driven pass.
        { "Shaders/frustum_culling.shaderpkg",     "InstanceBuffer",    "instances",   0u, sizeof(GPUInstance) },
        { "Shaders/hzb_culling.shaderpkg",         "InstanceBuffer",    "instances",   0u, sizeof(GPUInstance) },
        { "Shaders/draw_command_scatter.shaderpkg","InstanceBuffer",    "instances",   0u, sizeof(GPUInstance) },

        // -- GPUMesh: 128 B std430 element stride (`MeshBuffer { GPUMesh meshes[]; }`).
        { "Shaders/draw_command_scatter.shaderpkg","MeshBuffer",        "meshes",      0u, sizeof(GPUMesh) },

        // Both culling shaders share the complete 656-byte block.
        { "Shaders/hzb_culling.shaderpkg",         "CullingDataBuffer", "cullingData", 0u, sizeof(GPUCullingData) },
        { "Shaders/frustum_culling.shaderpkg",     "CullingDataBuffer", "cullingData", 0u, sizeof(GPUCullingData) },

        // -- Scatter table/record structs (struct-wrapped runtime arrays).
        //    BatchTableEntry is a public 16 B mirror; drawCommands is the 20 B
        //    VkDrawIndexedIndirectCommand record stream.
        { "Shaders/draw_command_scatter.shaderpkg","BatchTableBuffer",  "batchTable",    0u, sizeof(GPUDrawStreamBuilder::BatchTableEntry) },
        { "Shaders/draw_command_scatter.shaderpkg","DrawCommandsBuffer","drawCommands",  0u, kVkDrawIndexedIndirectCommandBytes },
        //    LOD crossfade state: a uvec2 per instance. Also proves the compiled
        //    package declares the buffer MakeScatterDescriptorSetLayout reserves a
        //    binding for — that binding count is a CPU-side constant which would
        //    keep agreeing with itself if the shader had never gained the buffer.
        { "Shaders/draw_command_scatter.shaderpkg","LodFadeBuffer",     "lodFade",       0u, GPUDrawStreamBuilder::kLodFadeStateBytes },
        //    Per-slice stats rows: 16 B, and the row the CPU reduce reads must be
        //    the row the shader's atomics write — the reserved word became
        //    lodChanges, so this pins the pair against a one-sided edit. Reflected
        //    under the block's INSTANCE name ("stats"), not its type name: that is
        //    what SPIRV-Reflect reports whenever a block declares one.
        { "Shaders/draw_command_scatter.shaderpkg","stats",             "sliceStats",    0u, sizeof(GPUDrawStreamBuilder::ScatterSliceStats) },
        //    Dwell-band history: a uint per instance, at binding 10 (same
        //    binding-count argument as lodFade).
        { "Shaders/draw_command_scatter.shaderpkg","PrevLodBuffer",     "prevLod",       0u, sizeof(uint32_t) },

        // -- ViewParams inline block: the one place a GPU struct's INTERIOR
        //    offsets are reflected, so these rows diff field-by-field against the
        //    shared Rendering::ViewParamsUBO mirror. Core prefix locked in both
        //    binding-5 consumers; heat_distortion extends coverage through the
        //    240 B prefix (ge_screenSize/ge_viewProj).
        { "Shaders/clustered_light_cull.shaderpkg", "ViewParams", "ge_invProj",     offsetof(ViewParamsUBO, ge_invProj),     sizeof(decltype(ViewParamsUBO::ge_invProj)) },
        { "Shaders/clustered_light_cull.shaderpkg", "ViewParams", "ge_view",        offsetof(ViewParamsUBO, ge_view),        sizeof(decltype(ViewParamsUBO::ge_view)) },
        { "Shaders/clustered_light_cull.shaderpkg", "ViewParams", "ge_nearFar",     offsetof(ViewParamsUBO, ge_nearFar),     sizeof(decltype(ViewParamsUBO::ge_nearFar)) },
        { "Shaders/clustered_light_cull.shaderpkg", "ViewParams", "ge_cameraPosWS", offsetof(ViewParamsUBO, ge_cameraPosWS), sizeof(decltype(ViewParamsUBO::ge_cameraPosWS)) },
        { "Shaders/forwardplus_lighting.shaderpkg", "ViewParams", "ge_invProj",     offsetof(ViewParamsUBO, ge_invProj),     sizeof(decltype(ViewParamsUBO::ge_invProj)) },
        { "Shaders/forwardplus_lighting.shaderpkg", "ViewParams", "ge_view",        offsetof(ViewParamsUBO, ge_view),        sizeof(decltype(ViewParamsUBO::ge_view)) },
        { "Shaders/forwardplus_lighting.shaderpkg", "ViewParams", "ge_nearFar",     offsetof(ViewParamsUBO, ge_nearFar),     sizeof(decltype(ViewParamsUBO::ge_nearFar)) },
        { "Shaders/forwardplus_lighting.shaderpkg", "ViewParams", "ge_cameraPosWS", offsetof(ViewParamsUBO, ge_cameraPosWS), sizeof(decltype(ViewParamsUBO::ge_cameraPosWS)) },
        { "Shaders/heat_distortion.shaderpkg",      "ViewParams", "ge_invProj",     offsetof(ViewParamsUBO, ge_invProj),     sizeof(decltype(ViewParamsUBO::ge_invProj)) },
        { "Shaders/heat_distortion.shaderpkg",      "ViewParams", "ge_nearFar",     offsetof(ViewParamsUBO, ge_nearFar),     sizeof(decltype(ViewParamsUBO::ge_nearFar)) },
        { "Shaders/heat_distortion.shaderpkg",      "ViewParams", "ge_screenSize",  offsetof(ViewParamsUBO, ge_screenSize),  sizeof(decltype(ViewParamsUBO::ge_screenSize)) },
        { "Shaders/heat_distortion.shaderpkg",      "ViewParams", "ge_viewProj",    offsetof(ViewParamsUBO, ge_viewProj),    sizeof(decltype(ViewParamsUBO::ge_viewProj)) },
    };
    // clang-format on
    return kRows;
}

// Search the reflected sets (UBO/SSBO bindings) and push-constant ranges for a
// block whose name matches, returning its layout (nullptr if absent).
const BlockLayout* FindBlock(const ShaderMeta& meta, const std::string& blockName)
{
    for (const DescriptorSetMeta& set : meta.Sets)
        for (const DescriptorBindingMeta& binding : set.Bindings)
            if (binding.Block.has_value() && binding.Name == blockName)
                return &*binding.Block;
    for (const PushConstantRangeMeta& pc : meta.PushConstants)
        if (pc.Name == blockName)
            return &pc.Block;
    return nullptr;
}

const Member* FindMember(const BlockLayout& block, const std::string& memberName)
{
    for (const Member& m : block.Members)
        if (m.Name == memberName)
            return &m;
    return nullptr;
}

std::string PkgStem(const std::string& pkgPath)
{
    const size_t slash = pkgPath.find_last_of("/\\");
    const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    const size_t dot = pkgPath.find('.', start);
    return pkgPath.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
}

class ShaderReflectionDiffTest : public ::testing::TestWithParam<MemberExpectation>
{
};

TEST_P(ShaderReflectionDiffTest, MemberMatchesCppMirror)
{
    const MemberExpectation& e = GetParam();

    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg(e.Pkg, ShaderSourceKind::SpirV, pkg, &err))
        GTEST_SKIP() << e.Pkg << " not available (build the CompileShaderPkgs target): " << err;

    const BlockLayout* block = FindBlock(pkg.meta, e.Block);
    ASSERT_NE(block, nullptr) << "block '" << e.Block << "' not found in " << e.Pkg
                              << " — reflection drift (block renamed, moved, or dropped)";

    const Member* member = FindMember(*block, e.Member);
    ASSERT_NE(member, nullptr) << "member '" << e.Member << "' not found in block '" << e.Block
                               << "' of " << e.Pkg << " — struct drift (field renamed or removed)";

    EXPECT_EQ(member->Offset, e.Offset)
        << e.Pkg << " :: " << e.Block << "." << e.Member << " offset drift (C++ mirror vs shader)";
    // An array member is indexed by its stride, so that is the byte count the
    // mirror's sizeof has to match; a runtime array's Size is one element's
    // unpadded bytes (none at all for a scalar or vector element) and pins nothing.
    const uint32_t mirroredBytes = member->ArrayStride ? *member->ArrayStride : member->Size;
    EXPECT_EQ(mirroredBytes, e.Size)
        << e.Pkg << " :: " << e.Block << "." << e.Member << " size/stride drift (C++ mirror vs shader)";
}

std::string ExpectationName(const testing::TestParamInfo<MemberExpectation>& info)
{
    std::string name = PkgStem(info.param.Pkg) + "_" + info.param.Block + "_" + info.param.Member;
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    return std::to_string(info.index) + "_" + name;
}

INSTANTIATE_TEST_SUITE_P(GpuStructMirrors, ShaderReflectionDiffTest,
                         ::testing::ValuesIn(ExpectationTable()), ExpectationName);

// Canary: the reflection load + block/member lookup path must work end to end on
// at least one package, so a passing suite can never be "everything skipped".
// hzb_culling is exercised by HzbCullingComputeTests today, so its package is
// expected to be present in any environment that runs the GPU-driven suite.
TEST(ShaderReflectionDiffCanary, ReflectionPipelineResolvesCullingBlock)
{
    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg("Shaders/hzb_culling.shaderpkg", ShaderSourceKind::SpirV, pkg, &err))
        GTEST_SKIP() << "hzb_culling.shaderpkg not available: " << err;

    const BlockLayout* block = FindBlock(pkg.meta, "CullingDataBuffer");
    ASSERT_NE(block, nullptr);
    const Member* member = FindMember(*block, "cullingData");
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->Size, sizeof(GPUCullingData));
}

} // namespace
