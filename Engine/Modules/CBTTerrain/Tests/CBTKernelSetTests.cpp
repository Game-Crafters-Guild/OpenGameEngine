// Kernel-set bring-up: the compiled SPIR-V loads and every kernel pipeline is
// created via its specialization constant. Driver acceptance of the SPIR-V is
// the implicit spirv-val gate (the build also runs spirv-val explicitly).
// Also: what the set reports when it takes the reduced narrow-heap arm.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTestHarness.h"

#include "Logger/LogSink.h"
#include "Logger/Logger.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;

namespace
{

class CapturingLogSink final : public Logger::LogSink
{
  public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Out->push_back(std::string(message.Message));
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "CBTCapturingLogSink"; }

  private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};

// Routes log lines into *out for the scope. This target links Logger's objects and
// CBTTerrain directly, so there is one Logger state and no cross-module redirect is
// needed. RAII: the sink holds a raw pointer to the test's vector, so a sink that
// outlived the frame would write through a dangling pointer on the next line logged —
// the destructor removes it on every exit path, ASSERT aborts included, and puts the
// console sink back so the rest of the suite still reports.
class ScopedLogCapture
{
  public:
    explicit ScopedLogCapture(std::vector<std::string>* out)
    {
        Logger::Log::Initialize({});
        Logger::Log::ClearSinks();
        Logger::Log::AddSink(std::make_unique<CapturingLogSink>(out));
    }
    ~ScopedLogCapture()
    {
        Logger::Log::ClearSinks();
        Logger::Log::Initialize({});
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
};

std::string FirstLineContaining(const std::vector<std::string>& lines, std::string_view needle)
{
    for (const std::string& line : lines)
        if (line.find(needle) != std::string::npos)
            return line;
    return {};
}

} // namespace

TEST(CBTKernelSet, LoadsAndCreatesAllPipelines)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    if (!device->GetCapabilities().supportsShaderInt64)
        GTEST_SKIP() << "device lacks shaderInt64; CBT kernels store u64 HeapIDs";

    CBTKernelSet kernels;
    ASSERT_TRUE(kernels.Initialize(*device, ShaderOutputDir()))
        << "cbt_kernels.comp.spv missing or a kernel pipeline failed to create";
    EXPECT_TRUE(kernels.IsReady());

    for (uint32_t i = 0; i < kCBTKernelCount; ++i)
        EXPECT_TRUE(kernels.GetPipeline(static_cast<CBTKernel>(i)).IsValid())
            << "kernel " << i << " pipeline invalid";

    kernels.Shutdown();
    EXPECT_FALSE(kernels.IsReady());
    device->Shutdown();
}

// The set asks the device for its pipelines and never builds them on the calling thread:
// with a device that hands builds to workers, Initialize returns with the set Pending,
// and it turns Ready, every kernel pipeline live, only once those builds have run.
TEST(CBTKernelSet, PendingUntilItsRequestedBuildsRunThenReady)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    if (!device->GetCapabilities().supportsShaderInt64)
        GTEST_SKIP() << "device lacks shaderInt64; CBT kernels store u64 HeapIDs";

    // Stands in for the engine's worker dispatcher; declared after the device, so a
    // job never run is destroyed while the device lives.
    std::vector<std::function<void()>> heldBuilds;
    device->SetPipelineBuildDispatcher([&heldBuilds](std::function<void()> build)
                                       { heldBuilds.push_back(std::move(build)); });

    CBTKernelSet kernels;
    ASSERT_TRUE(kernels.Initialize(*device, ShaderOutputDir()))
        << "cbt_kernels.comp.spv missing or a kernel pipeline failed";
    EXPECT_EQ(kernels.Poll(), CBTKernelSetState::Pending);
    EXPECT_FALSE(kernels.IsReady());
    EXPECT_EQ(heldBuilds.size(), kCBTKernelCount);
    EXPECT_FALSE(kernels.GetPipeline(CBTKernel::Reset).IsValid());

    std::vector<std::function<void()>> builds = std::move(heldBuilds);
    heldBuilds.clear();
    for (std::function<void()>& build : builds)
        build();

    EXPECT_EQ(kernels.Poll(), CBTKernelSetState::Ready);
    EXPECT_TRUE(kernels.IsReady());
    for (uint32_t i = 0; i < kCBTKernelCount; ++i)
        EXPECT_TRUE(kernels.GetPipeline(static_cast<CBTKernel>(i)).IsValid()) << "kernel " << i;

    kernels.Shutdown();
    device->SetPipelineBuildDispatcher({});
    device->Shutdown();
}

// A device that reports no 64-bit shader integer support runs the narrow-heap kernels,
// whose u32 heap ID caps subdivision well below the wide arm — the terrain just renders
// coarser, and nothing else says why. The line must name the arm and both ceilings so a
// report of coarse terrain can be attributed without a debugger.
TEST(CBTKernelSet, NarrowHeapArmReportsItsCeiling)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    std::vector<std::string> lines;
    {
        ScopedLogCapture capture(&lines);
        CBTKernelSet kernels;
        // The by-name override is how a machine that has int64 reaches the arm a machine
        // without it is put on.
        ASSERT_TRUE(kernels.Initialize(*device, ShaderOutputDir(), "cbt_kernels_heap32.comp.spv"))
            << "cbt_kernels_heap32.comp.spv missing or a kernel pipeline failed to create";
        EXPECT_TRUE(kernels.IsNarrowHeap());
        kernels.Shutdown();
        Logger::Log::Flush();
    }
    device->Shutdown();

    const std::string reported = FirstLineContaining(lines, "narrow-heap");
    ASSERT_FALSE(reported.empty()) << "the narrow arm was taken without reporting it";
    EXPECT_NE(reported.find(std::to_string(kHeap32DecodeSubdiv)), std::string::npos)
        << "the narrow arm's subdivision ceiling is missing from: " << reported;
    EXPECT_NE(reported.find(std::to_string(kMaxDecodeSubdiv)), std::string::npos)
        << "what the wide arm would allow is missing from: " << reported;
}

// The counterpart: the wide arm must stay quiet, or the line is noise that trains people
// to ignore it.
TEST(CBTKernelSet, WideHeapArmDoesNotReportTheNarrowOne)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    if (!device->GetCapabilities().supportsShaderInt64)
        GTEST_SKIP() << "device lacks shaderInt64; the wide arm is unreachable here";

    std::vector<std::string> lines;
    {
        ScopedLogCapture capture(&lines);
        CBTKernelSet kernels;
        ASSERT_TRUE(kernels.Initialize(*device, ShaderOutputDir()))
            << "cbt_kernels.comp.spv missing or a kernel pipeline failed to create";
        EXPECT_FALSE(kernels.IsNarrowHeap());
        kernels.Shutdown();
        Logger::Log::Flush();
    }
    device->Shutdown();

    // The set's own pipeline-count line proves the capture is live, so the silence below
    // is a silence and not a sink that saw nothing.
    ASSERT_FALSE(FirstLineContaining(lines, "compute pipelines").empty())
        << "the log capture saw nothing at all; the check below would pass on any build";
    EXPECT_TRUE(FirstLineContaining(lines, "narrow-heap").empty())
        << "the wide arm reported the narrow arm's ceiling";
}
