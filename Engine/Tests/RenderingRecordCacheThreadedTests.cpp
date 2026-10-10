// Threaded stress for the A2.4-D6 record-cache thread-safety work (parallel
// render-graph recording). N workers concurrently drive the shared caches that
// PipelineVariantCache / MaterialBinder mutate on the record hot path, forcing
// genuine concurrent inserts (start cold, no pre-warm — D6.2 makes concurrent
// inserts legal), and assert the results converge to the serial outcome with no
// corruption. Mirrors DescriptorSetAllocatorThreadedTests.
//
// Coverage:
//   - PipelineVariantCache::GetSharedDepthPipelineId — the full L1 + shared_mutex
//     + double-check path on a real variant map, with PreloadSharedDepthShaders
//     run serially first (as production does before the record window).
//   - MaterialBinder per-pass descriptor cache (m_PerPassCache) + the per-worker
//     thread_local warn maps.
//
// The color/depth instanced variant maps use the structurally-identical
// machinery (same InstancedL1Slot, same shared_mutex, same double-check), but
// driving GetOrCompile{Color,Depth}Variant needs a live RGContext + a ready
// material build context; they are additionally exercised by the in-editor
// cold-parallel-record soak (GE_PARALLEL_RECORD=1 records a cold cache under
// Vulkan validation from frame 1).
//
// Sanitizers: MSVC has no ThreadSanitizer, so "TSAN-style" is unavailable here;
// the high worker count x repeat count maximizes interleavings, and the test is
// clean under /fsanitize=address when that config is built. The debug lock-held
// assertions (AssertInsertLockHeld / AssertPerPassLockHeld) trip on any unlocked
// insert.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AssetCore/GUID.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Types/StringId.h"

#include "TestDeviceHelper.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{

// Force real contention even on small hosts / CI agents.
unsigned WorkerCount()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n < 4u ? 4u : n;
}

BufferHandle MakeUniformBuffer(IDevice& dev, size_t size)
{
    BufferDesc desc{};
    desc.size        = size;
    desc.usage       = static_cast<uint32_t>(BufferUsage::Uniform);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.debugName   = "RecordCacheThreaded.Cam";
    return dev.CreateBuffer(desc);
}

// Barrier: all workers spin until every worker is parked, then release together
// so the concurrent window is genuinely simultaneous.
template <class Fn>
void RunConcurrently(unsigned workers, Fn&& body)
{
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned t = 0; t < workers; ++t)
    {
        threads.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {}
            body(t);
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(workers)) {}
    go.store(true, std::memory_order_release);
    for (auto& th : threads)
        th.join();
}

} // namespace

// -----------------------------------------------------------------------------
// PipelineVariantCache::GetSharedDepthPipelineId — concurrent cold inserts.
// -----------------------------------------------------------------------------
TEST(RenderingRecordCacheThreaded, SharedDepthPipelineIdConcurrentInsertsConverge)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto& variants = rs.Materials().Variants();

    // Production warms the shader load (file I/O + reflection) serially before
    // the parallel window; mirror that so no worker triggers a lazy load.
    variants.PreloadSharedDepthShaders();

    // Skip if the shared-depth .spv isn't staged in this test environment — the
    // intern path has nothing to compile without it.
    const auto probe = variants.GetSharedDepthPipelineId(
        VertexAttributeFlags::HasPosition, PrimitiveTopology::TriangleList,
        CullModeFlagBits::Back, /*bias=*/false, /*clamp=*/false,
        FrontFace::CounterClockwise);
    if (!probe.IsValid())
        GTEST_SKIP() << "shared depth shader not available in test env";

    // A matrix of distinct keys, none equal to the canonical keys the preload
    // warmed (HasPosition / HasPosition|Skinned), so the concurrent run does
    // genuine cold inserts. Every dimension (vertex flags, cull, bias, winding)
    // changes the pipeline desc, so distinct keys => distinct pipelines.
    struct Key
    {
        VertexAttributeFlags Flags;
        CullModeFlags        Cull;
        bool                 Bias;
        FrontFace            Winding;
    };
    const VertexAttributeFlags kNonSkinned =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal;
    const VertexAttributeFlags kSkinned =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal
        | VertexAttributeFlags::HasJoints | VertexAttributeFlags::HasWeights;

    std::vector<Key> matrix;
    for (auto flags : {kNonSkinned, kSkinned})
        for (auto cull : {CullModeFlagBits::Back, CullModeFlagBits::None, CullModeFlagBits::Front})
            for (bool bias : {false, true})
                for (auto winding : {FrontFace::CounterClockwise, FrontFace::Clockwise})
                    matrix.push_back({flags, static_cast<CullModeFlags>(cull), bias, winding});

    const auto call = [&](const Key& k) {
        return variants.GetSharedDepthPipelineId(
            k.Flags, PrimitiveTopology::TriangleList, k.Cull, k.Bias, /*clamp=*/false, k.Winding);
    };

    const unsigned kWorkers = WorkerCount();
    const int      kRepeat  = 32;
    std::vector<std::vector<GraphicsPipelineId>> results(kWorkers);

    RunConcurrently(kWorkers, [&](unsigned t) {
        results[t].assign(matrix.size(), GraphicsPipelineId{});
        for (int r = 0; r < kRepeat; ++r)
            for (size_t i = 0; i < matrix.size(); ++i)
                results[t][i] = call(matrix[i]);
    });

    // (a) results identical across all workers for every key; (b) valid;
    // (c) a post-race serial lookup equals each worker's value (cache consistent).
    for (size_t i = 0; i < matrix.size(); ++i)
    {
        const uint32_t ref = results[0][i].Value;
        EXPECT_NE(ref, 0u) << "key " << i << " never interned";
        for (unsigned t = 1; t < kWorkers; ++t)
            EXPECT_EQ(results[t][i].Value, ref)
                << "worker " << t << " diverged on key " << i;
        EXPECT_EQ(call(matrix[i]).Value, ref)
            << "post-race serial lookup disagrees on key " << i;
    }

    // No lost/duplicated inserts: every distinct key resolved to its own pipeline.
    std::unordered_set<uint32_t> ids;
    for (size_t i = 0; i < matrix.size(); ++i)
        ids.insert(results[0][i].Value);
    EXPECT_EQ(ids.size(), matrix.size())
        << "concurrent inserts merged or lost distinct shared-depth pipelines";

    rs.Shutdown();
    device->Shutdown();
}

// -----------------------------------------------------------------------------
// MaterialBinder per-pass descriptor cache — concurrent cold inserts.
// -----------------------------------------------------------------------------
TEST(RenderingRecordCacheThreaded, PerPassDescriptorCacheConcurrentInsertsConverge)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // A material whose only set is per-pass-scoped (its single binding resolves
    // from PassResources, not DrawBindings), so lookups route through the
    // per-pass cache under test.
    auto meta = std::make_shared<ShaderMeta>();
    {
        DescriptorSetMeta s{};
        s.Set = 3;
        DescriptorBindingMeta b{};
        b.Binding    = 0;
        b.Name       = "Cam";
        b.Type       = ShaderMetaBindingType::kUniformBuffer;
        b.Count      = 1;
        b.StagesMask = (1u << 0);
        s.Bindings.push_back(b);
        meta->Sets.push_back(s);
    }
    Material mat = Material::TestFactory::Create(GUID::Generate(), "CamOnly", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf = MakeUniformBuffer(*device, 256);
    ASSERT_TRUE(camBuf.IsValid());

    auto& binder = rs.Materials().Binder();
    binder.OnBeginFrame(); // fresh frame => cold per-pass cache + fresh L1 epoch

    const uint32_t kViews   = 16;
    const unsigned kWorkers = WorkerCount();
    const int      kRepeat  = 64;
    std::vector<std::vector<DescriptorSetHandle>> results(kWorkers);

    // Every worker builds the per-pass set for every view; multiple workers hit
    // the SAME (view) key at once, forcing concurrent inserts + the double-check.
    RunConcurrently(kWorkers, [&](unsigned t) {
        results[t].assign(kViews, DescriptorSetHandle{});
        DrawBindings draw{};
        for (int r = 0; r < kRepeat; ++r)
            for (uint32_t v = 0; v < kViews; ++v)
            {
                PassBindingContext pass{};
                pass.View      = v;
                pass.FrameSlot = 0;
                pass.Samples   = 1;
                pass.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf, 0, 0});
                results[t][v] = binder.BuildSetForBinding(pass, mat, 3, draw);
            }
    });

    // Each view resolves to ONE cached handle shared by all workers: the
    // double-check keeps a single entry per (view, keywords, layout) key.
    for (uint32_t v = 0; v < kViews; ++v)
    {
        const DescriptorSetHandle ref = results[0][v];
        EXPECT_TRUE(ref.IsValid()) << "view " << v << " produced no set";
        for (unsigned t = 1; t < kWorkers; ++t)
            EXPECT_EQ(results[t][v], ref)
                << "worker " << t << " got a different per-pass set for view " << v;
    }

    // Distinct views => distinct cached sets (no key collision from the race).
    std::unordered_set<uint64_t> handles;
    for (uint32_t v = 0; v < kViews; ++v)
        handles.insert(results[0][v].id);
    EXPECT_EQ(handles.size(), kViews) << "per-pass cache merged distinct views";

    device->DestroyBuffer(camBuf);
    rs.Shutdown();
    device->Shutdown();
}

// -----------------------------------------------------------------------------
// MaterialBinder per-set builder (the shared-state core of BindMaterialForDraw)
// under N-worker concurrency, exercising BOTH the per-pass cache and the now
// per-worker thread_local warn maps. The pipeline bind that BindMaterialForDraw
// also does is per-worker command recording with no shared state, so
// BuildSetForBindingFromMeta is the meaningful concurrent surface.
//
// Not covered here (documented, deferred to the in-editor cold-parallel soak):
// the pointer-caching instanced-variant compile paths GetOrCompile{Color,Depth}-
// Variant. Driving them needs a live RGContext plus a ready material build
// context (editor asset mount + shaderc), neither of which the headless
// RenderServices harness has. Their L1 + shared_mutex + double-check machinery is
// structurally identical to GetSharedDepthPipelineId (tested above), and the ON
// smoke re-run forks all 6 passes to cold-compile color/depth variants
// concurrently under Vulkan validation.
// -----------------------------------------------------------------------------
TEST(RenderingRecordCacheThreaded, BinderSetBuilderAndWarnMapsConcurrent)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Resolvable per-pass set (routes through m_PerPassCache).
    DescriptorSetMeta resolvable{};
    resolvable.Set = 2;
    {
        DescriptorBindingMeta b{};
        b.Binding    = 0;
        b.Name       = "Cam";
        b.Type       = ShaderMetaBindingType::kUniformBuffer;
        b.Count      = 1;
        b.StagesMask = (1u << 0);
        resolvable.Bindings.push_back(b);
    }
    // Set with a statically-used SSBO that resolves from NEITHER DrawBindings nor
    // PassResources — trips the unresolved-buffer guard and the warn-once path
    // (m_WarnedBindings), now per-worker thread_local.
    DescriptorSetMeta unresolvable{};
    unresolvable.Set = 5;
    {
        DescriptorBindingMeta b{};
        b.Binding    = 0;
        b.Name       = "MissingSSBO";
        b.Type       = ShaderMetaBindingType::kStorageBuffer;
        b.Count      = 1;
        b.StagesMask = (1u << 0);
        unresolvable.Bindings.push_back(b);
    }

    auto meta = std::make_shared<ShaderMeta>();
    meta->Sets.push_back(resolvable);
    meta->Sets.push_back(unresolvable);
    Material mat = Material::TestFactory::Create(GUID::Generate(), "BinderConc", 16u);
    Material::TestFactory::SetShaderMeta(mat, meta);

    auto camBuf = MakeUniformBuffer(*device, 256);
    ASSERT_TRUE(camBuf.IsValid());

    auto& binder = rs.Materials().Binder();
    binder.OnBeginFrame();

    const uint32_t kViews   = 16;
    const unsigned kWorkers = WorkerCount();
    const int      kRepeat  = 64;
    std::vector<std::vector<DescriptorSetHandle>> resolved(kWorkers);
    std::atomic<uint64_t> unresolvableCount{0};
    std::atomic<uint64_t> unexpectedResolvable{0};

    RunConcurrently(kWorkers, [&](unsigned t) {
        resolved[t].assign(kViews, DescriptorSetHandle{});
        DrawBindings draw{};
        for (int r = 0; r < kRepeat; ++r)
            for (uint32_t v = 0; v < kViews; ++v)
            {
                PassBindingContext pass{};
                pass.View      = v;
                pass.FrameSlot = 0;
                pass.Samples   = 1;
                pass.PassResources.Buffers.push_back({HashStringId("Cam"), camBuf, 0, 0});

                // Per-pass cache path (via the override meta that BindMaterialForDraw walks).
                resolved[t][v] = binder.BuildSetForBindingFromMeta(pass, mat, resolvable, draw);

                // Warn-map + unresolved-guard path (thread_local warn maps).
                bool unres = false;
                binder.BuildSetForBindingFromMeta(pass, mat, unresolvable, draw, &unres);
                if (unres)
                    unresolvableCount.fetch_add(1, std::memory_order_relaxed);
                else
                    unexpectedResolvable.fetch_add(1, std::memory_order_relaxed);
            }
    });

    // Resolvable set converges to one cached handle per view (double-check).
    for (uint32_t v = 0; v < kViews; ++v)
    {
        const DescriptorSetHandle ref = resolved[0][v];
        EXPECT_TRUE(ref.IsValid()) << "view " << v << " produced no set";
        for (unsigned t = 1; t < kWorkers; ++t)
            EXPECT_EQ(resolved[t][v], ref)
                << "worker " << t << " diverged on resolvable set for view " << v;
    }
    // The unresolvable set trips the guard on EVERY call (never spuriously resolves),
    // and the thread_local warn maps take the concurrent hits without corruption.
    EXPECT_EQ(unexpectedResolvable.load(), 0u) << "unresolved-buffer guard raced";
    EXPECT_EQ(unresolvableCount.load(),
              static_cast<uint64_t>(kWorkers) * kRepeat * kViews);

    device->DestroyBuffer(camBuf);
    rs.Shutdown();
    device->Shutdown();
}
