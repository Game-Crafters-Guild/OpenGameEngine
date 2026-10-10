// The two ways a device ends, each measured for retention.
//
// Repeated headless device create/destroy cycles must not grow the process: test
// harnesses across the tree build one IDevice per fixture, so anything a cycle
// retains lands hundreds of times in a single-process suite run. Repeated
// in-place rebuilds must not grow it either — device-loss recovery replaces the
// VkDevice under a running process, so a rebuild that retains its predecessor
// accumulates for as long as the session lasts.
//
// A live headless device costs roughly 8 OS threads and several hundred MB of
// private bytes (driver-side, measured on the NVIDIA ICD), all of which comes
// back when the device is destroyed. The thresholds below are set under one
// device's worth so a single retained device fails either test.

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// psapi.h and tlhelp32.h require windows.h first.
#include <psapi.h>
#include <tlhelp32.h>
#endif

using namespace GameEngine::Rendering;
using GameEngine::Rendering::Tests::ScopedEnvVar;

namespace
{

struct ProcessSample
{
    uint32_t Threads = 0;
    uint64_t PrivateBytes = 0;
};

#if defined(_WIN32)
uint32_t CountProcessThreads()
{
    const DWORD selfPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    uint32_t count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == selfPid)
                ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

ProcessSample SampleProcess()
{
    ProcessSample sample{};
    sample.Threads = CountProcessThreads();
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)))
    {
        sample.PrivateBytes = static_cast<uint64_t>(counters.PrivateUsage);
    }
    return sample;
}
#else
ProcessSample SampleProcess()
{
    return {};
}
#endif

// Same shape as the per-fixture harnesses this test stands in for: headless,
// no swapchain, no debug layer.
std::unique_ptr<IDevice> MakeProbeDevice()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

constexpr int kCycleCount = 30;
// Cycles skipped before the compared windows: first-touch allocations, loader
// and ICD one-time init all land in the first few cycles.
constexpr int kWarmupCycles = 4;
// Cycles averaged at each end. Both endpoints use the window MINIMUM: a
// retained device raises the floor, while the ordinary allocate/free swing
// inside a cycle does not.
constexpr int kWindowCycles = 6;
// Under one device's worth (~8 threads, ~270 MB), so one retained device fails.
constexpr uint32_t kMaxThreadFloorGrowth = 4;
constexpr uint64_t kMaxPrivateBytesFloorGrowth = 192ull * 1024ull * 1024ull;

ProcessSample WindowFloor(const std::vector<ProcessSample>& samples, int begin, int count)
{
    ProcessSample floor = samples[begin];
    for (int i = begin + 1; i < begin + count; ++i)
    {
        floor.Threads = std::min(floor.Threads, samples[i].Threads);
        floor.PrivateBytes = std::min(floor.PrivateBytes, samples[i].PrivateBytes);
    }
    return floor;
}

// --- In-place rebuild arm ----------------------------------------------------

// One frame of the shape the render loop issues. TickDeviceRecovery runs
// unconditionally and before the render, exactly as the hosts drive it: a failed
// rebuild suppresses rendering, so a BeginFrame-driven retry would starve. The
// graphics submit is what an injected loss classifies as VK_ERROR_DEVICE_LOST.
void RunFrame(IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

// Rebuild cycles driven. VulkanDevice caps a burst of losses at kMaxRecoveryCycles
// (3) inside the 60 s recover-relose window: a fourth loss that lands that fast is
// judged a loop the rebuild is not fixing and transitions to Failed instead. Three
// discriminates decisively anyway — a rebuild that retained its predecessor would
// leave three devices' worth behind, well past the thresholds above.
constexpr int kRebuildCycles = 3;
// Frames run at each measurement stage before its samples are taken: a rebuild
// re-creates command pools and VMA blocks on first touch, and those land in the
// first frames after it.
constexpr int kStageWarmFrames = 4;
// Samples per stage, compared by window MINIMUM (WindowFloor) at both endpoints.
constexpr int kStageSampleFrames = 6;
constexpr int kStageFrames = kStageWarmFrames + kStageSampleFrames;
// Healthy frames between the end of a stage and the frame whose submit is lost, so
// no settle sample is taken mid-rebuild. BeginFrame advances the fault-injection
// frame ordinal only on frames that pass the health gate, so this counts the
// frames this test actually runs.
constexpr int kLossFrameMargin = 2;
// Frames allowed for one loss to fire and its rebuild to complete. A loss fires on
// the first submit at or after its target frame and the rebuild finishes on the
// next tick, so this is slack, not a schedule.
constexpr int kMaxFramesPerRebuild = 32;

// GE_VK_FORCE_DEVICE_LOST target frames, derived from the stage schedule so the
// two cannot drift apart. Frame ordinals are 1-based and must strictly increase.
std::string LossFrameSchedule()
{
    std::string schedule;
    for (int cycle = 1; cycle <= kRebuildCycles; ++cycle)
    {
        if (!schedule.empty())
            schedule += ',';
        schedule += std::to_string(cycle * (kStageFrames + kLossFrameMargin));
    }
    return schedule;
}

} // namespace

TEST(DeviceLifecycle, RepeatedCreateDestroyDoesNotGrowProcess)
{
    {
        auto probe = MakeProbeDevice();
        if (!probe)
            GTEST_SKIP() << "no Vulkan device";
    }

    std::vector<ProcessSample> samples;
    samples.reserve(kCycleCount);

    for (int cycle = 0; cycle < kCycleCount; ++cycle)
    {
        {
            auto dev = MakeProbeDevice();
            ASSERT_TRUE(dev) << "device creation failed at cycle " << cycle;
        }
        samples.push_back(SampleProcess());
        std::printf("cycle %2d  threads %4u  private %8.1f MB\n",
                    cycle,
                    samples.back().Threads,
                    static_cast<double>(samples.back().PrivateBytes) / (1024.0 * 1024.0));
        std::fflush(stdout);
    }

    const ProcessSample early = WindowFloor(samples, kWarmupCycles, kWindowCycles);
    const ProcessSample late = WindowFloor(samples, kCycleCount - kWindowCycles, kWindowCycles);

    EXPECT_LE(late.Threads, early.Threads + kMaxThreadFloorGrowth)
        << "thread floor rose from " << early.Threads << " to " << late.Threads << " across "
        << kCycleCount << " device cycles — a destroyed device is being retained";
    EXPECT_LE(late.PrivateBytes, early.PrivateBytes + kMaxPrivateBytesFloorGrowth)
        << "private-byte floor rose from " << early.PrivateBytes << " to " << late.PrivateBytes
        << " across " << kCycleCount << " device cycles";
}

// The other lifecycle: an in-place rebuild (device-loss recovery) destroys the
// VkDevice and brings a new one up while deliberately KEEPING the shared
// VkInstance — the refcount decrement is gated on the rebuild flag. Measuring
// across rebuilds, with the instance and the IDevice still live, is the only
// placement where a retained VkDevice shows up at all: taking the instance
// refcount to zero destroys the instance, and that reclaims ICD state wholesale,
// so an outer create/destroy loop would read clean whatever a rebuild kept.
TEST(DeviceLifecycle, RepeatedRebuildsDoNotGrowProcess)
{
    const std::string schedule = LossFrameSchedule();
    // Parsed once, at Initialize — must be set before the device is created.
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", schedule.c_str());

    auto dev = MakeProbeDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device";

    std::vector<ProcessSample> samples;
    samples.reserve(static_cast<size_t>(kRebuildCycles + 1) * kStageSampleFrames);

    // One measurement stage: settle the device, then sample every frame. Stage 0
    // is the pre-loss baseline; stage k follows the kth completed rebuild. Both
    // ends of the comparison therefore hold exactly one live device.
    const auto runStage = [&](int stage)
    {
        for (int i = 0; i < kStageWarmFrames; ++i)
            RunFrame(*dev);
        for (int i = 0; i < kStageSampleFrames; ++i)
        {
            RunFrame(*dev);
            samples.push_back(SampleProcess());
        }
        const ProcessSample floor = WindowFloor(samples, stage * kStageSampleFrames, kStageSampleFrames);
        std::printf("stage %d  generation %llu  threads %4u  private %8.1f MB\n",
                    stage,
                    static_cast<unsigned long long>(dev->GetDeviceRebuildGeneration()),
                    floor.Threads,
                    static_cast<double>(floor.PrivateBytes) / (1024.0 * 1024.0));
        std::fflush(stdout);
        return floor;
    };

    const ProcessSample baseline = runStage(0);
    ASSERT_EQ(dev->GetDeviceRebuildGeneration(), 0ull) << "baseline stage must precede every rebuild";

    ProcessSample latest = baseline;
    for (int cycle = 1; cycle <= kRebuildCycles; ++cycle)
    {
        const uint64_t generationBefore = dev->GetDeviceRebuildGeneration();
        for (int frame = 0;
             frame < kMaxFramesPerRebuild && dev->GetDeviceRebuildGeneration() == generationBefore;
             ++frame)
        {
            RunFrame(*dev);
        }
        // A loop that silently never rebuilt reads perfectly flat, so the
        // generation is the instrument's positive control, not a nicety.
        ASSERT_EQ(dev->GetDeviceRebuildGeneration(), generationBefore + 1)
            << "cycle " << cycle << ": the injected loss did not complete a rebuild within "
            << kMaxFramesPerRebuild << " frames (health "
            << DeviceHealthToString(dev->GetDeviceHealth())
            << ") — device recovery must be enabled (GE_DEVICE_RECOVERY unset or non-zero)";
        ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::AwaitingReprovision);
        // Stand in for the re-provision consumer, so the next injected loss can fire.
        dev->NotifyReprovisionComplete();
        ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

        latest = runStage(cycle);
    }

    ASSERT_EQ(dev->GetDeviceRebuildGeneration(), static_cast<uint64_t>(kRebuildCycles));

    EXPECT_LE(latest.Threads, baseline.Threads + kMaxThreadFloorGrowth)
        << "thread floor rose from " << baseline.Threads << " to " << latest.Threads << " across "
        << kRebuildCycles << " in-place rebuilds — a replaced device is being retained";
    EXPECT_LE(latest.PrivateBytes, baseline.PrivateBytes + kMaxPrivateBytesFloorGrowth)
        << "private-byte floor rose from " << baseline.PrivateBytes << " to " << latest.PrivateBytes
        << " across " << kRebuildCycles << " in-place rebuilds";

    dev->Shutdown();
}
