#include "EditorDebugMetrics.h"

#include "Core/DebugMetrics.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Audio/AudioSystem.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Materials/ShaderCompileService.h"

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace GameEngine
{

// RSS query interval — syscall is cheap but not free; once per ~0.5s at 60fps is plenty.
// CollectEditorDebugMetrics is called only from the main thread.
static constexpr int kRssSampleIntervalFrames = 30;

void CollectEditorDebugMetrics(Rendering::IDevice* device,
                               Rendering::RenderGraph::RGFrame* frame)
{
#if GE_ENABLE_METRICS
    auto& metrics = Debug::DebugMetrics::Get();

    // The Application main-loop timing series (Time/FPS, Time/FrameMs, and the
    // per-phase costs) are pushed directly by Application::Tick. Now that
    // DebugMetrics::Get() is exported from Engine.dll (one shared instance),
    // those pushes land in the same registry this panel reads — no editor-side
    // re-push is needed, and adding one here would double-sample every "Time/*"
    // series and compress the charts' time axis.

    if (device)
    {
        metrics.PushSample("Memory/VRAM",          static_cast<float>(device->DebugGetAllocatedBytes()));
        metrics.PushSample("Memory/VRAM/Textures", static_cast<float>(device->DebugGetTextureBytes()));
        metrics.PushSample("Memory/VRAM/Buffers",  static_cast<float>(device->DebugGetBufferBytes()));

        // Frame-to-frame period, not GPU-busy time: a display-paced frame plots
        // ~260 ms here while the GPU is idle at its lowest power state. Named for
        // what it is so the chart cannot be read as GPU cost.
        Rendering::IDevice::FrameSyncTimings sync{};
        if (device->GetLastFrameSyncTimings(sync) && sync.frameGpuPeriodMs > 0.0)
            metrics.PushSample("Time/GPUFramePeriod", static_cast<float>(sync.frameGpuPeriodMs));
    }

    // Render-graph counters from the main window's live frame (previous frame's
    // stable compiled graph at this point in the loop). Submissions/render-pass
    // counts come from the recording stats.
    if (frame)
    {
        const auto& g = frame->Graph();
        const auto& s = frame->Stats();
        metrics.PushSample("Render/Passes",       static_cast<float>(g.ScheduledOrder().size()));
        metrics.PushSample("Render/Resources",    static_cast<float>(g.ResourceCount()));
        metrics.PushSample("Render/Barriers",     static_cast<float>(g.BarrierCount()));
        metrics.PushSample("Render/Submissions",  static_cast<float>(s.SubmissionsMade));
        metrics.PushSample("Render/RenderPasses", static_cast<float>(s.RenderPassesBegun));
    }
    else
    {
        metrics.PushSample("Render/Passes",    0.0f);
        metrics.PushSample("Render/Resources", 0.0f);
        metrics.PushSample("Render/Barriers",  0.0f);
    }

    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        // One indirect draw is issued per BatchKey, so the sum of batch-key
        // counts across views maps to per-frame indirect draw count.
        uint32_t batchCount = 0;
        for (const auto& view : rs->Views().GetViews())
            batchCount += static_cast<uint32_t>(rs->GetEntityBatchKeys(view.id).size());
        metrics.PushSample("Render/DrawCalls", static_cast<float>(batchCount));
    }

    {
        static int s_RssFrameCounter = 0;
        if (++s_RssFrameCounter >= kRssSampleIntervalFrames)
        {
            s_RssFrameCounter = 0;
            float rssBytes = 0.0f;
#if defined(__APPLE__) || defined(__linux__)
            struct rusage ru{};
            if (getrusage(RUSAGE_SELF, &ru) == 0)
            {
#if defined(__APPLE__)
                rssBytes = static_cast<float>(ru.ru_maxrss);            // bytes on macOS
#else
                rssBytes = static_cast<float>(ru.ru_maxrss) * 1024.0f; // KB on Linux
#endif
            }
#elif defined(_WIN32)
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                rssBytes = static_cast<float>(pmc.WorkingSetSize);
#endif
            if (rssBytes > 0.0f)
                metrics.PushSample("Memory/Static", rssBytes);
        }
    }

    if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
    {
        metrics.PushSample("Physics/Bodies",       static_cast<float>(pw->GetBodyCount()));
        metrics.PushSample("Physics/ContactPairs", static_cast<float>(pw->GetContactPairCount()));
        // Per-frame simulation cost measured at the step boundary in
        // PhysicsStepSystem; 0 on frames where no fixed sub-step ran.
        metrics.PushSample("Time/Physics", PhysicsECS::PhysicsWorldService::GetLastStepMs());
        // Entities the writebacks composed in their most recent update; skipped (unchanged) ones do not count.
        metrics.PushSample("Physics/BodiesComposed",
                           static_cast<float>(PhysicsECS::PhysicsWorldService::GetLastBodiesComposed()));
        metrics.PushSample("Physics/CharactersComposed",
                           static_cast<float>(PhysicsECS::PhysicsWorldService::GetLastCharactersComposed()));
    }

    if (auto* audio = EngineCore::GetInstance().GetAudioSystem())
    {
        const float latencyMs = audio->GetOutputLatencyMs();
        if (latencyMs > 0.0f)
            metrics.PushSample("Audio/OutputLatencyMs", latencyMs);
    }

    metrics.PushSample("Pipeline/Compilations",
                       static_cast<float>(Rendering::ShaderCompileService::GetTotalCompilations()));
#else
    (void)device;
#endif
}

} // namespace GameEngine
