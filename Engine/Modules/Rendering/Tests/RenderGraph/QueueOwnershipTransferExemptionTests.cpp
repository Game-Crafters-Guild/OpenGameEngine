// D0 — the pre-registered decision experiment for the cross-queue arc.
//
// The question: VUID-vkCmdPipelineBarrier2-srcStageMask-09675 and its mirror
// dstStageMask-09676 constrain a barrier's stage masks to stages the RECORDING
// queue family supports — but both open with an exemption for a barrier that
// "specifies an acquire operation" (09675) or "a release operation" (09676).
// The release/acquire pair design (slice 2P) exists to buy that exemption, so
// each half can record the true stage of the queue on the other side. The
// exemption is defined against a queue-family ownership transfer, and an
// ownership transfer is defined against VK_SHARING_MODE_EXCLUSIVE — while this
// device creates every image CONCURRENT whenever it has more than one unique
// queue family, which the async-compute path guarantees.
//
// So: does naming two different queue families on a CONCURRENT image earn the
// exemption anyway? The design pre-registered "no — it must still fault". It
// does not: the layer grants the exemption on index inequality alone and never
// reads the image's sharing mode, so the EXCLUSIVE and CONCURRENT arms below
// are indistinguishable to it.
//
// What that settles and what it does not: the validation gate will not object
// to a pair over a CONCURRENT resource. It cannot speak to whether the spec
// defines those halves as acquire/release operations, nor to whether a driver
// honours a stage the recording queue cannot execute — the design's actual
// concern. A silent gate is the absence of an objection, not a guarantee.
//
// Method: record — never submit — deliberate barriers on a COMPUTE-family
// command buffer and read the validation layer's per-VUID verdict. The VUIDs
// under test are recording-time checks, so submission would add nothing but
// layout-mismatch noise from barriers whose oldLayout is a fiction.
//
// The matrix is the instrument: queue-family-IGNORED and same-family arms are
// the positive controls (the fault class must be visible at all), and the
// EXCLUSIVE arms are the negative control (the exemption must attach where the
// spec defines it, or the layer does not implement it and no arm discriminates).

#include "Rendering/Core/Device.h"
#include "VulkanDevice.h"

#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
// These arms deliberately provoke validation errors; the process-wide
// abort-on-error default would kill the run before the verdict is read.
void DisarmValidationAssert()
{
#if defined(_WIN32)
    _putenv_s("GE_VK_VALIDATION_ASSERT", "0");
#else
    setenv("GE_VK_VALIDATION_ASSERT", "0", 1);
#endif
}

// The arc's gate rule (design v0.4) requires the validation layers' resolved
// state to be READ, not assumed. That state is published only as a log line at
// instance creation, and a test binary installs no log sink — so without this
// the line does not exist to be checked. Shared buffer because the logger takes
// ownership of the sink and the test outlives it.
using LogLines = std::vector<std::string>;

class CapturingSink final : public Logger::LogSink
{
  public:
    explicit CapturingSink(std::shared_ptr<LogLines> lines) : m_Lines(std::move(lines)) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Lines->push_back(message.Message);
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "D0LogCapture"; }

  private:
    std::mutex m_Mutex;
    std::shared_ptr<LogLines> m_Lines;
};

// Installs the capture and returns the buffer. Replaces the sink set for the
// duration of the test (the binary has none), and ReleaseLogCapture restores it.
std::shared_ptr<LogLines> InstallLogCapture()
{
    auto lines = std::make_shared<LogLines>();
    Logger::Log::Config cfg;
    cfg.GlobalMinLevel = Logger::LogLevel::Info;
    Logger::Log::Initialize(cfg);
    Logger::Log::ClearSinks(); // drop the console sink Initialize seeds: capture only
    Logger::Log::AddSink(Logger::MakeUnique<CapturingSink>(lines));
    return lines;
}

void ReleaseLogCapture()
{
    Logger::Log::Flush();
    Logger::Log::ClearSinks();
}

// The single line matching `needle`, or empty when the process never logged one.
std::string FindLine(const std::shared_ptr<LogLines>& lines, const char* needle)
{
    Logger::Log::Flush();
    for (const std::string& line : *lines)
        if (line.find(needle) != std::string::npos)
            return line;
    return {};
}

std::unique_ptr<IDevice> MakeValidationDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Count of a VUID's hits by numeric suffix ("09675"), so the match survives the
// layer spelling the command name differently across versions.
uint64_t VuidHits(const ValidationStats& stats, const char* suffix)
{
    uint64_t hits = 0;
    for (const ValidationVuidStat& v : stats.Vuids)
        if (v.Vuid.find(suffix) != std::string::npos)
            hits += v.Count;
    return hits;
}

std::string VuidSummary(const ValidationStats& stats)
{
    std::string s = "errors=" + std::to_string(stats.ErrorCount) +
                    " warnings=" + std::to_string(stats.WarningCount);
    for (const ValidationVuidStat& v : stats.Vuids)
    {
        s += "\n  [" + std::string(v.IsError ? "ERROR" : "warn") + " x" +
             std::to_string(v.Count) + "] " + v.Vuid;
        if (!v.FirstMessage.empty())
            s += "\n    " + v.FirstMessage;
    }
    return s;
}

// A bare VkImage with its own allocation: the sharing mode is the experiment's
// controlled variable, so it is chosen here rather than inherited from
// VulkanDevice::CreateTexture (which always picks CONCURRENT on a multi-family
// device and offers no way to ask for EXCLUSIVE).
struct RawImage
{
    VkImage Image = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
};

bool CreateRawImage(VulkanDevice& device, VkSharingMode sharing,
                    const std::vector<uint32_t>& families, RawImage& out)
{
    constexpr uint32_t kImageExtent = 64;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kImageExtent, kImageExtent, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // COLOR_ATTACHMENT and SAMPLED are what make ColorAttachmentOptimal and
    // ShaderReadOnlyOptimal legal layouts for the barriers below.
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.sharingMode = sharing;
    if (sharing == VK_SHARING_MODE_CONCURRENT)
    {
        ci.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        ci.pQueueFamilyIndices = families.data();
    }

    VkDevice vk = device.GetVkDevice();
    if (vkCreateImage(vk, &ci, nullptr, &out.Image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vk, out.Image, &req);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device.GetVkPhysicalDevice(), &memProps);
    uint32_t typeIndex = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        const bool allowed = (req.memoryTypeBits & (1u << i)) != 0;
        const bool deviceLocal = (memProps.memoryTypes[i].propertyFlags &
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (allowed && deviceLocal)
        {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == VK_MAX_MEMORY_TYPES)
    {
        vkDestroyImage(vk, out.Image, nullptr);
        out.Image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(vk, &ai, nullptr, &out.Memory) != VK_SUCCESS ||
        vkBindImageMemory(vk, out.Image, out.Memory, 0) != VK_SUCCESS)
    {
        if (out.Memory != VK_NULL_HANDLE)
            vkFreeMemory(vk, out.Memory, nullptr);
        vkDestroyImage(vk, out.Image, nullptr);
        out = {};
        return false;
    }
    return true;
}

void DestroyRawImage(VulkanDevice& device, RawImage& img)
{
    VkDevice vk = device.GetVkDevice();
    if (img.Image != VK_NULL_HANDLE)
        vkDestroyImage(vk, img.Image, nullptr);
    if (img.Memory != VK_NULL_HANDLE)
        vkFreeMemory(vk, img.Memory, nullptr);
    img = {};
}

struct BarrierArm
{
    VkImage Image = VK_NULL_HANDLE;
    uint32_t SrcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t DstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkPipelineStageFlags2 SrcStage = 0;
    VkAccessFlags2 SrcAccess = 0;
    VkPipelineStageFlags2 DstStage = 0;
    VkAccessFlags2 DstAccess = 0;
};

// Records one arm into a fresh command buffer from `pool` and returns the
// validation verdict for that recording alone (stats are reset first). The row
// is printed because the matrix — not the pass/fail — is what this test exists
// to record; a reader re-running it should see the counts, not infer them from
// the assertions' polarity.
ValidationStats RecordArm(VulkanDevice& device, IDevice& idevice, VkCommandPool pool,
                          const char* armName, const BarrierArm& arm)
{
    VkDevice vk = device.GetVkDevice();

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk, &cbai, &cmd) != VK_SUCCESS)
        return {};

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkImageMemoryBarrier2 imb{};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imb.srcStageMask = arm.SrcStage;
    imb.srcAccessMask = arm.SrcAccess;
    imb.dstStageMask = arm.DstStage;
    imb.dstAccessMask = arm.DstAccess;
    imb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.srcQueueFamilyIndex = arm.SrcQueueFamily;
    imb.dstQueueFamilyIndex = arm.DstQueueFamily;
    imb.image = arm.Image;
    imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &imb;

    idevice.ResetValidationStats();
    vkBeginCommandBuffer(cmd, &bi);
    device.GetCmdPipelineBarrier2()(cmd, &dep);
    vkEndCommandBuffer(cmd);
    const ValidationStats stats = idevice.GetValidationStats();

    // Never submitted: these barriers claim an oldLayout the images were never
    // in, which is a submit-time check unrelated to the question under test.
    vkFreeCommandBuffers(vk, pool, 1, &cmd);

    std::cout << "[ arm      ] " << armName << ": 09675=" << VuidHits(stats, "09675")
              << " 09676=" << VuidHits(stats, "09676") << " errors=" << stats.ErrorCount
              << std::endl;
    return stats;
}
} // namespace

// Five arms on a compute-family command buffer, every one of them recording a
// barrier whose stage mask on one side is foreign to that queue. Only the queue
// family indices and the image's sharing mode vary:
//
//   families IGNORED            faults    — the fault class is visible at all
//   named, EQUAL families       faults    — no transfer, so no exemption
//   named, distinct, EXCLUSIVE  clean     — the exemption as the spec defines it
//   named, distinct, CONCURRENT clean     — the exemption attaches here too
//
// The last row is D0. The design pre-registered the opposite: it MUST fault,
// because a CONCURRENT resource cannot carry an ownership transfer and so its
// halves are not acquire/release operations. It does not fault. What this pins
// is the layer's rule — the exemption keys on srcQueueFamilyIndex differing
// from dstQueueFamilyIndex, and does not consult the image's sharing mode.
//
// Read the limit with the result: a clean run says the validation layer will
// not object, not that the spec permits it and not that a driver honours it.
TEST(QueueOwnershipTransferExemption, StageValidityExemptionKeysOnFamilyIndicesNotSharingMode)
{
    DisarmValidationAssert();
    auto logLines = InstallLogCapture();
    auto idev = MakeValidationDevice();
    if (!idev)
    {
        ReleaseLogCapture();
        GTEST_SKIP() << "no validation-layer device available";
    }
    if (!idev->GetValidationStats().Enabled)
    {
        ReleaseLogCapture();
        GTEST_SKIP() << "validation layer not present on this machine";
    }

    // Resolved layer state, read rather than assumed. The line is emitted once per
    // instance creation, and this device shares an instance with any earlier test in
    // the binary whose instance configuration matched, so the line is absent then —
    // run this test filtered for the armed evidence run.
    const std::string syncValLine = FindLine(logLines, "synchronization validation");
    if (!syncValLine.empty())
        EXPECT_NE(syncValLine.find("ENABLED"), std::string::npos)
            << "sync validation was asked for but did not arm: " << syncValLine;
    std::cout << "[ sync-val ] " << (syncValLine.empty() ? "not resolved in this process (instance "
                                                           "pre-created by an earlier test)"
                                                         : syncValLine)
              << std::endl;
    ReleaseLogCapture();

    auto& device = static_cast<VulkanDevice&>(*idev);
    const uint32_t graphicsFamily = device.GetGraphicsQueueFamilyIndex();
    const uint32_t computeFamily = device.GetComputeQueueFamilyIndex();
    if (graphicsFamily == computeFamily)
        GTEST_SKIP() << "no distinct compute family: the question does not arise on this device";
    ASSERT_NE(device.GetCmdPipelineBarrier2(), nullptr) << "synchronization2 recording required";

    uint32_t familyStorage[3] = {};
    uint32_t familyCount = 0;
    device.GetUniqueQueueFamilies(familyStorage, familyCount);
    ASSERT_GT(familyCount, 1u) << "CONCURRENT sharing is only reachable with >1 unique family";
    const std::vector<uint32_t> families(familyStorage, familyStorage + familyCount);

    RawImage concurrentImage;
    RawImage exclusiveImage;
    ASSERT_TRUE(CreateRawImage(device, VK_SHARING_MODE_CONCURRENT, families, concurrentImage));
    ASSERT_TRUE(CreateRawImage(device, VK_SHARING_MODE_EXCLUSIVE, families, exclusiveImage));

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = computeFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateCommandPool(device.GetVkDevice(), &pci, nullptr, &pool), VK_SUCCESS);

    // The acquire shape: recorded on the DESTINATION queue (compute), naming the
    // producer's ColorAttachmentOutput as its source stage — foreign to compute.
    BarrierArm acquire{};
    acquire.SrcQueueFamily = graphicsFamily;
    acquire.DstQueueFamily = computeFamily;
    acquire.SrcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquire.SrcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    acquire.DstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    acquire.DstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    // The release shape: recorded on the SOURCE queue (compute), naming the
    // consumer's ColorAttachmentOutput as its destination stage — foreign again.
    BarrierArm release{};
    release.SrcQueueFamily = computeFamily;
    release.DstQueueFamily = graphicsFamily;
    release.SrcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    release.SrcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    release.DstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    release.DstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

    auto withImage = [](BarrierArm arm, VkImage image)
    {
        arm.Image = image;
        return arm;
    };
    auto withoutTransfer = [](BarrierArm arm, VkImage image)
    {
        arm.Image = image;
        arm.SrcQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        arm.DstQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        return arm;
    };
    auto withSameFamily = [computeFamily](BarrierArm arm, VkImage image)
    {
        arm.Image = image;
        arm.SrcQueueFamily = computeFamily;
        arm.DstQueueFamily = computeFamily;
        return arm;
    };

    // Positive control FIRST: if the layer cannot see a foreign stage mask on a
    // plain barrier, every "no fault" below is an instrument reading, not a fact.
    const ValidationStats controlSrc =
        RecordArm(device, *idev, pool, "control.ignoredFamilies.src", 
                  withoutTransfer(acquire, concurrentImage.Image));
    const ValidationStats controlDst =
        RecordArm(device, *idev, pool, "control.ignoredFamilies.dst", 
                  withoutTransfer(release, concurrentImage.Image));
    ASSERT_GT(VuidHits(controlSrc, "09675"), 0u)
        << "positive control blind: a graphics-only srcStageMask on a compute-family command "
           "buffer must fault 09675 — no verdict is available from this instrument.\n"
        << VuidSummary(controlSrc);
    ASSERT_GT(VuidHits(controlDst, "09676"), 0u)
        << "positive control blind: a graphics-only dstStageMask on a compute-family command "
           "buffer must fault 09676 — no verdict is available from this instrument.\n"
        << VuidSummary(controlDst);

    // Negative control: the same stage masks on an EXCLUSIVE image, where the
    // halves ARE acquire/release operations and the exemption is defined.
    const ValidationStats exclusiveAcquire =
        RecordArm(device, *idev, pool, "exclusive.acquire", withImage(acquire, exclusiveImage.Image));
    const ValidationStats exclusiveRelease =
        RecordArm(device, *idev, pool, "exclusive.release", withImage(release, exclusiveImage.Image));
    ASSERT_EQ(VuidHits(exclusiveAcquire, "09675"), 0u)
        << "negative control blind: an EXCLUSIVE acquire half must earn the 09675 exemption. "
           "If it does not, this layer build does not implement the exemption at all and the "
           "CONCURRENT arms below cannot discriminate.\n"
        << VuidSummary(exclusiveAcquire);
    ASSERT_EQ(VuidHits(exclusiveRelease, "09676"), 0u)
        << "negative control blind: an EXCLUSIVE release half must earn the 09676 exemption.\n"
        << VuidSummary(exclusiveRelease);

    // Mechanism control: named but EQUAL families are not a transfer, so the
    // exemption must not attach — this is what separates "naming any indices
    // silences the check" from "naming DIFFERENT indices silences it".
    const ValidationStats sameFamily =
        RecordArm(device, *idev, pool, "control.sameFamily", 
                  withSameFamily(acquire, concurrentImage.Image));
    EXPECT_GT(VuidHits(sameFamily, "09675"), 0u)
        << "srcQueueFamilyIndex == dstQueueFamilyIndex is not an ownership transfer and must "
           "not earn the exemption.\n"
        << VuidSummary(sameFamily);

    // Under test: byte-identical barriers over an image whose only difference is
    // CONCURRENT sharing — the mode this device gives every image it creates.
    const ValidationStats concurrentAcquire =
        RecordArm(device, *idev, pool, "concurrent.acquire", withImage(acquire, concurrentImage.Image));
    const ValidationStats concurrentRelease =
        RecordArm(device, *idev, pool, "concurrent.release", withImage(release, concurrentImage.Image));

    EXPECT_EQ(VuidHits(concurrentAcquire, "09675"), 0u)
        << "the layer's 09675 exemption tracked the image's sharing mode.\n"
        << VuidSummary(concurrentAcquire);
    EXPECT_EQ(VuidHits(concurrentRelease, "09676"), 0u)
        << "the layer's 09676 exemption tracked the image's sharing mode.\n"
        << VuidSummary(concurrentRelease);

    idev->ResetValidationStats();
    vkDestroyCommandPool(device.GetVkDevice(), pool, nullptr);
    DestroyRawImage(device, concurrentImage);
    DestroyRawImage(device, exclusiveImage);
}
