// The renderer's incremental extraction and upload, measured: a manual microbenchmark over the
// extraction fast path (ExtractionHarness.h). Not a gate and not registered with CTest: build
// EngineRenderOptInTests and run this test by name with --gtest_also_run_disabled_tests and
// GE_RENDER_BENCH_JSON set to the report's path.

#include "ExtractionHarness.h"

#include "Components/Rendering/MeshGPUData.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <tuple>
#include <vector>
#include <nlohmann/json.hpp>

using namespace GameEngine;
using namespace GameEngine::Testing::ExtractionFastPath;

// Manual measurement only; run it by name with --gtest_also_run_disabled_tests.
// No time threshold participates in correctness; source writes, fence waits,
// final readback and setup are all outside the measured extraction/upload spans.
TEST_F(ExtractionFastPathTest, DISABLED_RendererIncrementalMicrobenchmark)
{
    const char* output = std::getenv("GE_RENDER_BENCH_JSON");
    if (!output || !*output)
        GTEST_SKIP() << "Set GE_RENDER_BENCH_JSON to an output path to opt in";
    const auto envText = [](const char* name, const char* fallback)
    {
        const char* value = std::getenv(name);
        return std::string(value && *value ? value : fallback);
    };
    const auto envCount = [&](const char* name, uint32 fallback)
    {
        return static_cast<uint32>(std::stoul(envText(name, std::to_string(fallback).c_str())));
    };
    const uint32 staticCount = envCount("GE_RENDER_BENCH_STATIC", 100000u);
    const uint32 moverCount = envCount("GE_RENDER_BENCH_MOVERS", 10000u);
    const uint32 warmup = envCount("GE_RENDER_BENCH_WARMUP", 32u);
    const uint32 samples = envCount("GE_RENDER_BENCH_SAMPLES", 120u);
    ASSERT_GT(staticCount, 0u);
    ASSERT_GT(moverCount, 0u);
    ASSERT_LE(static_cast<uint64>(staticCount) + moverCount, 500000u);
    ASSERT_GE(warmup, m_Device->GetFramesInFlight() * 2u);
    ASSERT_GT(samples, 0u);
    ASSERT_LE(warmup + static_cast<uint64>(samples), 10000u);
    const bool fast = envText("GE_RENDER_BENCH_FULL", "0") != "1";
    const std::string selectedCase = envText("GE_RENDER_BENCH_CASE", "all");
    ASSERT_TRUE(selectedCase == "all" || selectedCase == "idle" ||
                selectedCase == "contiguous" || selectedCase == "sparse");
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from, Clock::time_point to)
    { return std::chrono::duration<double, std::milli>(to - from).count(); };
    const auto summary = [](std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        double sum = 0.0;
        for (double value : values)
            sum += value;
        return nlohmann::json{{"mean", sum / values.size()}, {"min", values.front()},
                              {"p50", values[(values.size() - 1) / 2]},
                              {"p95", values[(values.size() - 1) * 95 / 100]},
                              {"max", values.back()}};
    };
    nlohmann::json report{{"schema", 1}, {"label", envText("GE_RENDER_BENCH_LABEL", "manual")},
                           {"gitSha", envText("GE_RENDER_BENCH_SHA", "unspecified")},
                           {"fastPath", fast}, {"framesInFlight", m_Device->GetFramesInFlight()},
                           {"warmupFrames", warmup}, {"sampleFrames", samples},
                           {"scatterCompact", envText("GE_SCATTER_COMPACT", "default")},
                           {"cases", nlohmann::json::array()}};
    for (const std::string distribution : {"idle", "contiguous", "sparse"})
    {
        if (selectedCase != "all" && selectedCase != distribution)
            continue;
        SCOPED_TRACE(distribution);
        ExtractionHarness h;
        ASSERT_TRUE(h.Initialize(m_Device.get(), fast));
        const uint32 movers = distribution == "idle" ? 0u : moverCount;
        const uint32 total = staticCount + movers;
        const auto handles = h.world->CreateBatchWithInit<WorldTransform, MeshRenderer, MeshGPUData>(
            total, [&](size_t i, WorldTransform& wt, MeshRenderer& mr, MeshGPUData& bridge)
            {
                wt = {};
                SetIdentityMatrix(wt.matrix);
                wt.matrix[12] = static_cast<float>(i % 1000u);
                wt.matrix[14] = static_cast<float>(i / 1000u);
                wt.Version = 1u;
                mr = {};
                mr.meshGpuHandleId = static_cast<uint64>(h.meshHandle);
                mr.materialAssetGuid.Set(h.materialGuid);
                mr.renderLayerMask = 1u;
                bridge = {};
            });
        h.extraction->Update(*h.world, kDt); // allocate stable slots outside timing
        ASSERT_EQ(h.rs.GetGPUScene()->GetInstances().size(), total);

        std::vector<std::tuple<const WorldTransform*, const MeshGPUData*>> resolved(total);
        h.world->GetComponentsBatch<WorldTransform, MeshGPUData>(std::span{handles}, std::span{resolved});
        struct Candidate { EntityHandle Handle; WorldTransform* Transform; uint32 Slot; };
        std::vector<Candidate> candidates;
        candidates.reserve(total);
        for (uint32 i = 0; i < total; ++i)
        {
            const auto [wt, bridge] = resolved[i];
            ASSERT_NE(wt, nullptr);
            ASSERT_NE(bridge, nullptr);
            candidates.push_back({handles[i], const_cast<WorldTransform*>(wt), bridge->instanceIndex});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.Slot < b.Slot; });
        std::vector<Candidate> moving;
        std::vector<EntityHandle> movingHandles;
        std::vector<bool> isMoving(total, false);
        for (uint32 i = 0; i < movers; ++i)
        {
            const uint32 rank = distribution == "sparse"
                ? static_cast<uint32>(static_cast<uint64>(i) * total / movers)
                : total - movers + i;
            moving.push_back(candidates[rank]);
            movingHandles.push_back(candidates[rank].Handle);
            isMoving[candidates[rank].Slot] = true;
        }
        auto* scene = h.rs.GetGPUScene();
        const uint32 slots = m_Device->GetFramesInFlight();
        std::vector<float> lastY(slots, 0.0f);
        std::vector<double> extractionTimes, processTimes, submitTimes, uploadTimes, uploadBytes;
        nlohmann::json raw = nlohmann::json::array();
        uint32 fastFrames = 0u;
        for (uint32 frame = 0; frame < warmup + samples; ++frame)
        {
            ASSERT_TRUE(m_Device->BeginFrame());
            scene->AdvanceFrameSlot(); // rotate without prematurely uploading old data
            h.rs.BeginWorldDrawFrame();
            const float y = 1.0f + static_cast<float>(frame) * 0.125f;
            h.world->StampComponentWriteBatch(movingHandles.data(), movingHandles.size(),
                                               GetComponentTypeId<WorldTransform>());
            for (const auto& item : moving)
            {
                item.Transform->matrix[13] = y;
                ++item.Transform->Version;
            }
            h.world->EmitComponentDirtyBatch(GetComponentTypeId<WorldTransform>(),
                                              movingHandles.data(), movingHandles.size());
            const auto extractionStart = Clock::now();
            h.extraction->Update(*h.world, kDt);
            const auto extractionEnd = Clock::now();
            const uint64 bytesBefore = scene->GetInstanceUploadBytesTotal();
            const auto uploadStart = Clock::now();
            scene->FlushGPUBuffers();
            const auto uploadEnd = Clock::now();
            const uint64 bytes = scene->GetInstanceUploadBytesTotal() - bytesBefore;
            const uint32 slot = m_Device->GetFrameIndex() % slots;
            lastY[slot] = y;
            if (frame >= warmup)
            {
                const double extractionMs = ms(extractionStart, extractionEnd);
                const double uploadMs = ms(uploadStart, uploadEnd);
                extractionTimes.push_back(extractionMs);
                processTimes.push_back(h.Stats().ProcessMs);
                submitTimes.push_back(h.Stats().SubmitMs);
                uploadTimes.push_back(uploadMs);
                uploadBytes.push_back(static_cast<double>(bytes));
                fastFrames += h.Stats().FastFrame != 0u;
                raw.push_back({{"frame", frame}, {"slot", slot}, {"extractionMs", extractionMs},
                               {"uploadMs", uploadMs}, {"uploadBytes", bytes},
                               {"patched", h.Stats().FeedPatchedCount},
                               {"rebuilds", h.Stats().RebuildCount},
                               {"fast", h.Stats().FastFrame},
                               {"escalationBits", h.Stats().EscalationReasonBits}});
            }
            h.world->SwapComponentDirtyFeed();
            scene->EndFrame();
            m_Device->Present();
        }
        // Independently verify every physical slot after measurement, without
        // flushing it again: it must retain its own last extracted snapshot.
        for (uint32 i = 0; i < slots; ++i)
        {
            ASSERT_TRUE(m_Device->BeginFrame());
            scene->AdvanceFrameSlot();
            const uint32 slot = m_Device->GetFrameIndex() % slots;
            const auto* mapped = static_cast<const GPUInstance*>(m_Device->MapBuffer(scene->GetInstanceBuffer()));
            ASSERT_NE(mapped, nullptr);
            std::vector<GPUInstance> actual(mapped, mapped + total);
            m_Device->UnmapBuffer(scene->GetInstanceBuffer());
            for (uint32 row = 0; row < total; ++row)
                EXPECT_FLOAT_EQ(actual[row].transform.Data()[13], isMoving[row] ? lastY[slot] : 0.0f)
                    << "slot " << slot << " row " << row;
            scene->EndFrame();
            m_Device->Present();
        }
        m_Device->WaitForIdle();
        report["cases"].push_back({{"distribution", distribution}, {"static", staticCount},
                                   {"movers", movers}, {"instances", total},
                                   {"fastFrames", fastFrames},
                                   {"extractionMs", summary(extractionTimes)},
                                   {"processMs", summary(processTimes)},
                                   {"submitMs", summary(submitTimes)},
                                   {"uploadMs", summary(uploadTimes)},
                                   {"uploadBytes", summary(uploadBytes)}, {"frames", raw}});
        std::ofstream file(output);
        ASSERT_TRUE(file.is_open()) << output;
        file << report.dump(2) << '\n';
        ASSERT_TRUE(file.good()) << output;
    }
}
