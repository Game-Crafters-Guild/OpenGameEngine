// Pipeline-creation-only guard for the ACES 2 tier tables' storage class.
//
// THE REGRESSION THIS FILE EXISTS FOR
// -----------------------------------
// glslang materializes dynamically indexed CONST arrays as a Function-storage
// copy with a whole-array OpStore — every fragment invocation memcpys the
// tables into scratch memory. At the tier ladder's size (~91 KB/invocation)
// the machine-wide scratch reservation faults the device outright
// (VK_ERROR_DEVICE_LOST at BeginFrame, recurring IP_FAULT — design doc B4),
// and even the old single-tier form cost 21.7 ms of local-memory traffic per
// 3070x902 frame. The fix keeps the tables in the Aces2Tables SSBO; this test
// pins that they never silently move back into shader constants.
//
// HOW IT MEASURES
// ---------------
// VK_KHR_pipeline_executable_properties reports per-executable compiler
// statistics at pipeline CREATION — no draw, no submission, no frame, so it is
// safe on a machine whose GPU queue is otherwise reserved. The tonemap
// pipeline is created with CAPTURE_STATISTICS and the fragment executable's
// scratch/local-memory statistic must read near zero. Measured on this
// machine: the const-array shader reads ~91-108 KB here; the SSBO shader reads
// 0. The 2 KB bound leaves room for incidental driver spill without admitting
// any table-sized copy.
//
// Population guards: executable list non-empty, statistics list non-empty, and
// the memory statistic found BY NAME — a driver that stops reporting it fails
// loudly with the full list printed rather than passing vacuously.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace
{

std::vector<uint32_t> LoadSpirv(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0)
        return {};
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool LooksLikeScratchStat(const std::string& name)
{
    const std::string n = ToLower(name);
    return n.find("local") != std::string::npos || n.find("scratch") != std::string::npos ||
           n.find("spill") != std::string::npos || n.find("stack") != std::string::npos ||
           n.find("private") != std::string::npos;
}

uint64_t StatValueAsU64(const VkPipelineExecutableStatisticKHR& s)
{
    switch (s.format)
    {
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return s.value.b32 ? 1u : 0u;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
            return s.value.i64 < 0 ? 0u : static_cast<uint64_t>(s.value.i64);
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: return s.value.u64;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
            return s.value.f64 < 0.0 ? 0u : static_cast<uint64_t>(s.value.f64);
        default: return 0u;
    }
}

// Everything raw-Vulkan and creation-only: the engine's VulkanDevice does not
// enable VK_KHR_pipeline_executable_properties, and this guard must not depend
// on the engine's device configuration anyway — it is a claim about the SPIR-V
// as the driver compiles it.
TEST(Aces2PipelineStats, TierTablesDoNotSpillToScratchMemory)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Aces2PipelineStatsTests";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
        GTEST_SKIP() << "no Vulkan instance on this machine";

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    if (gpus.empty())
    {
        vkDestroyInstance(instance, nullptr);
        GTEST_SKIP() << "no Vulkan physical device";
    }
    const VkPhysicalDevice gpu = gpus[0];

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, exts.data());
    const bool hasExecProps = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0;
    });
    if (!hasExecProps)
    {
        vkDestroyInstance(instance, nullptr);
        GTEST_SKIP() << "VK_KHR_pipeline_executable_properties unsupported here";
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfs.data());
    uint32_t graphicsFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            graphicsFamily = i;
            break;
        }
    ASSERT_NE(graphicsFamily, UINT32_MAX) << "no graphics queue family";

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR execFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    execFeat.pipelineExecutableInfo = VK_TRUE;
    const char* devExts[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &execFeat;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VkDevice device = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateDevice(gpu, &dci, nullptr, &device), VK_SUCCESS);

    const std::string shaderDir = RENDERING_SHADER_OUTPUT_DIR;
    const auto vsWords = LoadSpirv(shaderDir + "/fullscreen_noinput.vert.spv");
    const auto fsWords = LoadSpirv(shaderDir + "/tonemap.frag.spv");
    ASSERT_FALSE(vsWords.empty()) << "fullscreen_noinput.vert.spv missing — shaders not compiled";
    ASSERT_FALSE(fsWords.empty()) << "tonemap.frag.spv missing — shaders not compiled";

    auto makeModule = [&](const std::vector<uint32_t>& words) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = words.size() * sizeof(uint32_t);
        smci.pCode = words.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &smci, nullptr, &m);
        return m;
    };
    VkShaderModule vs = makeModule(vsWords);
    VkShaderModule fs = makeModule(fsWords);
    ASSERT_NE(vs, VK_NULL_HANDLE);
    ASSERT_NE(fs, VK_NULL_HANDLE);

    // Set 0 exactly as tonemap.frag declares it: uHDRColor, uExposure, Aces2Tables.
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl), VK_SUCCESS);

    const VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreatePipelineLayout(device, &plci, nullptr, &layout), VK_SUCCESS);

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateRenderPass(device, &rpci, nullptr, &renderPass), VK_SUCCESS);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, 1, 1, 0, 1};
    const VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &blend;
    gpci.layout = layout;
    gpci.renderPass = renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
              VK_SUCCESS)
        << "tonemap pipeline failed to create — creation itself is part of the guard";

    auto pfnProps = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    auto pfnStats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    ASSERT_NE(pfnProps, nullptr);
    ASSERT_NE(pfnStats, nullptr);

    VkPipelineInfoKHR pinfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pinfo.pipeline = pipeline;
    uint32_t execCount = 0;
    ASSERT_EQ(pfnProps(device, &pinfo, &execCount, nullptr), VK_SUCCESS);
    ASSERT_GT(execCount, 0u) << "driver reported zero pipeline executables — the guard cannot run";
    std::vector<VkPipelineExecutablePropertiesKHR> props(
        execCount, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    ASSERT_EQ(pfnProps(device, &pinfo, &execCount, props.data()), VK_SUCCESS);

    bool sawFragmentExec = false;
    bool sawScratchStat = false;
    uint64_t worstScratchBytes = 0;
    std::string allStats;
    for (uint32_t e = 0; e < execCount; ++e)
    {
        if (!(props[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT))
            continue;
        sawFragmentExec = true;
        VkPipelineExecutableInfoKHR einfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        einfo.pipeline = pipeline;
        einfo.executableIndex = e;
        uint32_t statCount = 0;
        ASSERT_EQ(pfnStats(device, &einfo, &statCount, nullptr), VK_SUCCESS);
        ASSERT_GT(statCount, 0u) << "driver reported an empty statistics list for the fragment "
                                    "executable — the guard cannot run";
        std::vector<VkPipelineExecutableStatisticKHR> stats(
            statCount, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        ASSERT_EQ(pfnStats(device, &einfo, &statCount, stats.data()), VK_SUCCESS);
        for (const auto& s : stats)
        {
            const uint64_t v = StatValueAsU64(s);
            allStats += std::string("  [") + props[e].name + "] " + s.name + " = " +
                        std::to_string(v) + "\n";
            if (LooksLikeScratchStat(s.name))
            {
                sawScratchStat = true;
                // The NVIDIA driver reports "Local Memory Size" as a constant
                // 2^36 base plus the actual per-invocation bytes in the low
                // word (measured here: const-array shader = 2^36 + 89,264;
                // SSBO shader = 2^36 + 0). Take the low 32 bits — a no-op for
                // drivers that report plain byte counts below 4 GB.
                worstScratchBytes = std::max(worstScratchBytes, v & 0xFFFFFFFFull);
            }
        }
    }

    // Evidence on the GREEN arm too — a passing run must leave its reading.
    std::cout << "[ MEASURED ] fragment scratch/local memory: " << worstScratchBytes
              << " bytes (bound 2048)\n"
              << allStats;

    EXPECT_TRUE(sawFragmentExec) << "no fragment-stage executable reported:\n" << allStats;
    EXPECT_TRUE(sawScratchStat)
        << "no scratch/local/spill/stack statistic reported by this driver — the guard cannot "
           "assert its bound; full statistics list:\n"
        << allStats;

    // PRE-REGISTERED: the SSBO-backed shader must not spill the tables. The
    // const-array form reads ~91-108 KB here; 2 KB admits incidental spill
    // only. If this fires, the tables have moved back into shader constants
    // (or a new dynamically-indexed const array was added) — see design doc B4.
    constexpr uint64_t kMaxScratchBytes = 2048;
    EXPECT_LE(worstScratchBytes, kMaxScratchBytes)
        << "fragment stage reserves " << worstScratchBytes
        << " bytes of scratch/local memory — table data is being copied per invocation again; "
           "full statistics:\n"
        << allStats;

    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, dsl, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}

} // namespace
