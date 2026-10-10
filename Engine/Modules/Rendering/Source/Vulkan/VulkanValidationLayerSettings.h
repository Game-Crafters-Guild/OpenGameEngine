/**
 * @file VulkanValidationLayerSettings.h
 * @brief Settings handed to VK_LAYER_KHRONOS_validation through VK_EXT_layer_settings.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace GameEngine {
namespace Rendering {

    /// Parse the GE_VK_SYNC_VALIDATION toggle. Split out from the env read so the
    /// policy is unit-testable. OFF unless explicitly requested: synchronization
    /// validation shadows every resource access and revalidates every submit, so a
    /// run costs multiples of its normal frame time — it is a diagnostic mode for
    /// hunting a specific hazard, never a default. Same leading-character idiom as
    /// ParseGpuCheckpointsEnabled.
    inline bool ParseSyncValidationEnabled(const char* envValue) noexcept
    {
        return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0' &&
               envValue[0] != 'f' && envValue[0] != 'F';
    }

    /// Parse the GE_VK_GPU_AV toggle. Same leading-character idiom as
    /// ParseSyncValidationEnabled, and OFF for the same reason: GPU-AV inserts a
    /// validation dispatch ahead of every indirect draw/dispatch it checks.
    inline bool ParseGpuAssistedValidationEnabled(const char* envValue) noexcept
    {
        return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0' &&
               envValue[0] != 'f' && envValue[0] != 'F';
    }

    /// How the shader-instrumentation request is decided
    /// (GE_VK_GPU_AV_SHADER_INSTRUMENTATION).
    enum class GpuAvShaderInstrumentationMode : uint8_t
    {
        /// Request it exactly when VK_EXT_descriptor_buffer cannot be enabled.
        Auto,
        /// Request it whatever the extension does. The control arm that proves the
        /// layer's own gate is real rather than only read in its source.
        ForceOn,
        /// Never request it. The control arm that proves this engine's gate.
        ForceOff,
    };

    /// Parse GE_VK_GPU_AV_SHADER_INSTRUMENTATION. Unset means Auto, so the plain
    /// GE_VK_GPU_AV opt-in keeps deciding on its own; 'a' spells Auto back out
    /// explicitly. Otherwise the leading-character idiom the other toggles use.
    inline GpuAvShaderInstrumentationMode ParseGpuAvShaderInstrumentationMode(const char* envValue) noexcept
    {
        if (envValue == nullptr || envValue[0] == '\0' || envValue[0] == 'a' || envValue[0] == 'A')
            return GpuAvShaderInstrumentationMode::Auto;
        if (envValue[0] == '0' || envValue[0] == 'f' || envValue[0] == 'F')
            return GpuAvShaderInstrumentationMode::ForceOff;
        return GpuAvShaderInstrumentationMode::ForceOn;
    }

    /**
     * @brief Whether to ask the layer for shader instrumentation.
     *
     * @param gpuAvEnabled             GE_VK_GPU_AV. Instrumentation is a child of
     *                                 gpuav_enable in the layer's own dependency
     *                                 tree, so without it the request is inert.
     * @param descriptorBufferPossible Whether VK_EXT_descriptor_buffer could still
     *                                 end up enabled on the device. Instance
     *                                 creation cannot know that the extension WILL
     *                                 be enabled — the physical device is not
     *                                 selected yet — but it can know when it
     *                                 CANNOT be, which is the only direction Auto
     *                                 needs.
     */
    inline bool ShouldRequestGpuAvShaderInstrumentation(bool gpuAvEnabled,
                                                        bool descriptorBufferPossible,
                                                        GpuAvShaderInstrumentationMode mode) noexcept
    {
        if (!gpuAvEnabled)
            return false;
        switch (mode)
        {
            case GpuAvShaderInstrumentationMode::ForceOff:
                return false;
            case GpuAvShaderInstrumentationMode::ForceOn:
                return true;
            case GpuAvShaderInstrumentationMode::Auto:
                return !descriptorBufferPossible;
        }
        return false;
    }

    /**
     * @brief What actually happened to the shader-instrumentation request.
     *
     * Three states, not two, because "we did not ask" and "the layer refused" are
     * different facts and a run that reports no shader-side out-of-bounds means
     * something different under each. Collapsing them is how an instrument that was
     * never armed gets read as a clean result.
     */
    enum class GpuAvShaderInstrumentationState : uint8_t
    {
        /// Never asked for. Says nothing about descriptor or BDA out-of-bounds.
        NotRequested,
        /// Asked for, and the layer drops it because VK_EXT_descriptor_buffer is
        /// enabled. Also says nothing — but for a reason that is not our choice.
        RequestedButUnavailable,
        /// Asked for, with the extension out of the way. Shader-side checks run.
        Active,
    };

    /// @param descriptorBufferEnabled Whether VK_EXT_descriptor_buffer was actually
    ///                                enabled on the device — known only after
    ///                                device creation, which is why the resolved
    ///                                state cannot be reported at instance creation.
    inline GpuAvShaderInstrumentationState ClassifyGpuAvShaderInstrumentation(
        bool requested, bool descriptorBufferEnabled) noexcept
    {
        if (!requested)
            return GpuAvShaderInstrumentationState::NotRequested;
        return descriptorBufferEnabled ? GpuAvShaderInstrumentationState::RequestedButUnavailable
                                       : GpuAvShaderInstrumentationState::Active;
    }

    /// One stable, greppable line per state — a diagnostic matrix is only readable
    /// if each arm's outcome can be matched as a string rather than judged.
    inline const char* DescribeGpuAvShaderInstrumentation(GpuAvShaderInstrumentationState state) noexcept
    {
        switch (state)
        {
            case GpuAvShaderInstrumentationState::Active:
                return "GPU-AV shader instrumentation ACTIVE — descriptor indexing and buffer-device-address "
                       "out-of-bounds ARE checked this run (VK_EXT_descriptor_buffer is not enabled)";
            case GpuAvShaderInstrumentationState::RequestedButUnavailable:
                return "GPU-AV shader instrumentation REQUESTED-BUT-UNAVAILABLE — the layer disables it while "
                       "VK_EXT_descriptor_buffer is enabled, so descriptor and buffer-device-address "
                       "out-of-bounds are NOT checked this run";
            case GpuAvShaderInstrumentationState::NotRequested:
                return "GPU-AV shader instrumentation NOT REQUESTED — descriptor and buffer-device-address "
                       "out-of-bounds are NOT checked this run; set GE_VK_GPU_AV_SHADER_INSTRUMENTATION=1 "
                       "with VK_EXT_descriptor_buffer out of the way (GE_VK_CAPTURE_COMPAT=1) to arm them";
        }
        return "GPU-AV shader instrumentation state UNKNOWN";
    }

#if defined(VK_EXT_layer_settings)

    /// Layer setting that turns on synchronization validation: "check for resource
    /// access conflicts caused by missing or incorrectly used synchronization
    /// operations". Two neighbouring knobs are deliberately left at their layer
    /// defaults — syncval_submit_time_validation (the cross-submit and cross-queue
    /// boundary check) is already ON by default, and
    /// syncval_shader_accesses_heuristic is documented by the layer as
    /// false-positive-prone.
    inline constexpr const char* kSyncValidationSettingName = "validate_sync";

    /// GPU-assisted validation master switch. `validate_gpu_based` is its
    /// deprecated predecessor (it was an enum before 1.3.296) and is deliberately
    /// not used.
    inline constexpr const char* kGpuAvEnableSettingName = "gpuav_enable";

    /// Shader instrumentation — rewriting application SPIR-V to bounds-check
    /// descriptor and buffer-device-address accesses. The layer's own default is
    /// true (manifest, SDK 1.4.321.0), so the value chained here is what decides
    /// it, in both directions.
    ///
    /// Requested only when VK_EXT_descriptor_buffer is out of the picture. With
    /// that extension enabled the layer force-disables this whole family
    /// ("VK_EXT_descriptor_buffer is enabled, but GPU-AV does not currently
    /// support validation of descriptor buffers. [Disabling all shader
    /// instrumentation checks]") and the request buys a warning plus
    /// instrumentation work that is thrown away.
    inline constexpr const char* kGpuAvShaderInstrumentationSettingName = "gpuav_shader_instrumentation";

    /// The three checks shader instrumentation is made of, pinned individually for
    /// the same reason as the buffer-validation five: the layer's manifest gives
    /// each of them `dependence` on BOTH gpuav_enable and
    /// gpuav_shader_instrumentation with mode ALL, so they apply only when both
    /// parents are true and are otherwise left at their own defaults. A default
    /// flip on any one would empty exactly the coverage this request exists for
    /// while the parent still reported instrumentation as on.
    ///
    /// descriptor_checks and buffer_address_oob are the two the descriptor-buffer
    /// path costs us — out-of-bounds descriptor indexing and invalid access
    /// through a buffer device address. post_process_descriptor_indexing is what
    /// turns a descriptor-indexing hit into the index that was actually accessed.
    inline constexpr const char* kGpuAvDescriptorChecksSettingName = "gpuav_descriptor_checks";
    inline constexpr const char* kGpuAvPostProcessDescriptorIndexingSettingName =
        "gpuav_post_process_descriptor_indexing";
    inline constexpr const char* kGpuAvBufferAddressOobSettingName = "gpuav_buffer_address_oob";

    /// Buffer-content validation: the contents of indirect draw, dispatch and
    /// trace-rays argument buffers, index buffers and buffer copies. This family
    /// does NOT instrument application shaders, so it survives descriptor buffers
    /// — it is the half of GPU-AV that remains usable on this engine's
    /// configuration.
    inline constexpr const char* kGpuAvBuffersValidationSettingName = "gpuav_buffers_validation";

    /// The five checks the family above is made of, each pinned on individually.
    ///
    /// Setting only the parent does not pin them: the layer reads the parent, and
    /// when it is true takes the branch that reads these five and otherwise leaves
    /// each at its own default (`layers/layer_options.cpp`, the
    /// `gpuav_buffers_validation` block). So a default flip on any one of them
    /// would empty exactly the check a device-loss investigation was relying on,
    /// with the parent still reporting the family as enabled. These are the
    /// engine's GPU-driven indirect draws, CBT's indirect dispatches, its index
    /// buffers and its staging copies — the whole reason this family is armed.
    inline constexpr const char* kGpuAvIndirectDrawsBuffersSettingName = "gpuav_indirect_draws_buffers";
    inline constexpr const char* kGpuAvIndirectDispatchesBuffersSettingName = "gpuav_indirect_dispatches_buffers";
    inline constexpr const char* kGpuAvIndirectTraceRaysBuffersSettingName = "gpuav_indirect_trace_rays_buffers";
    inline constexpr const char* kGpuAvBufferCopiesSettingName = "gpuav_buffer_copies";
    inline constexpr const char* kGpuAvIndexBuffersSettingName = "gpuav_index_buffers";

    /// 0 disables the layer's per-VUID message cap entirely.
    inline constexpr uint32_t kDuplicateMessageLimitUnlimited = 0;

    /**
     * @brief Which optional validation families to request from the layer.
     *
     * A named field per family: these are independent diagnostic modes with
     * different costs, and positional bools at the call site stop being readable
     * at two.
     */
    struct ValidationLayerSettingsDesc
    {
        /// Synchronization validation (GE_VK_SYNC_VALIDATION).
        bool SyncValidation = false;
        /// GPU-assisted buffer-content validation (GE_VK_GPU_AV).
        bool GpuAssistedValidation = false;
        /// GPU-AV shader instrumentation: descriptor and buffer-device-address
        /// bounds checks. Ignored unless GpuAssistedValidation is also set — the
        /// layer nests it under gpuav_enable. Decide it with
        /// ShouldRequestGpuAvShaderInstrumentation rather than by hand.
        bool GpuAvShaderInstrumentation = false;
    };

    /**
     * @brief Owns the VkLayerSettingEXT array chained into vkCreateInstance, and
     *        the values that array points at.
     *
     * Each VkLayerSettingEXT holds a bare pointer to one of this object's value
     * members, and the layer dereferences those inside vkCreateInstance, so the
     * object has to outlive that call and must never relocate — a copy's settings
     * would still address the original's storage.
     */
    class ValidationLayerSettings
    {
    public:
        /// @param layerName Layer the settings address. Borrowed, and must outlive
        ///                  this object.
        /// @param desc      Optional validation families to request.
        ValidationLayerSettings(const char* layerName, const ValidationLayerSettingsDesc& desc) noexcept
            : m_SyncValidation(desc.SyncValidation ? VK_TRUE : VK_FALSE)
            , m_GpuAvEnable(desc.GpuAssistedValidation ? VK_TRUE : VK_FALSE)
            , m_GpuAvShaderInstrumentation(desc.GpuAvShaderInstrumentation ? VK_TRUE : VK_FALSE)
        {
            // ValidationStatsStore owns message dedup and must count exactly, so the
            // layer's silent per-VUID cap (default 10 in current SDKs) comes off.
            Append(layerName,
                   "duplicate_message_limit",
                   VK_LAYER_SETTING_TYPE_UINT32_EXT,
                   &m_DuplicateMessageLimit);

            if (desc.SyncValidation)
            {
                Append(layerName,
                       kSyncValidationSettingName,
                       VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                       &m_SyncValidation);
            }

            if (desc.GpuAssistedValidation)
            {
                Append(layerName,
                       kGpuAvEnableSettingName,
                       VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                       &m_GpuAvEnable);
                Append(layerName,
                       kGpuAvShaderInstrumentationSettingName,
                       VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                       &m_GpuAvShaderInstrumentation);
                // Keyed on the value actually chained, not on the request that
                // produced it: the layer reads these only when both parents are
                // true, so they can never be chained while the parent says false.
                if (m_GpuAvShaderInstrumentation == VK_TRUE)
                {
                    for (const char* check : {kGpuAvDescriptorChecksSettingName,
                                              kGpuAvPostProcessDescriptorIndexingSettingName,
                                              kGpuAvBufferAddressOobSettingName})
                    {
                        Append(layerName, check, VK_LAYER_SETTING_TYPE_BOOL32_EXT, &m_GpuAvSubCheckPinnedOn);
                    }
                }
                Append(layerName,
                       kGpuAvBuffersValidationSettingName,
                       VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                       &m_GpuAvSubCheckPinnedOn);
                for (const char* check : {kGpuAvIndirectDrawsBuffersSettingName,
                                          kGpuAvIndirectDispatchesBuffersSettingName,
                                          kGpuAvIndirectTraceRaysBuffersSettingName,
                                          kGpuAvBufferCopiesSettingName,
                                          kGpuAvIndexBuffersSettingName})
                {
                    Append(layerName, check, VK_LAYER_SETTING_TYPE_BOOL32_EXT, &m_GpuAvSubCheckPinnedOn);
                }
            }
        }

        /// Self-referential storage: deleting the copy operations also suppresses
        /// the implicit move operations, which is the point — see the class note.
        ValidationLayerSettings(const ValidationLayerSettings&) = delete;
        ValidationLayerSettings& operator=(const ValidationLayerSettings&) = delete;

        /// Valid for this object's lifetime; pair with Count() for
        /// VkLayerSettingsCreateInfoEXT.
        const VkLayerSettingEXT* Data() const noexcept { return m_Settings.data(); }

        uint32_t Count() const noexcept { return m_Count; }

        bool SyncValidationEnabled() const noexcept { return m_SyncValidation == VK_TRUE; }

        bool GpuAssistedValidationEnabled() const noexcept { return m_GpuAvEnable == VK_TRUE; }

        /// What was ASKED of the layer. Whether the request survives is the layer's
        /// call and belongs to GpuAvShaderInstrumentationState, not here.
        bool GpuAvShaderInstrumentationRequested() const noexcept
        {
            return m_GpuAvShaderInstrumentation == VK_TRUE;
        }

    private:
        /// Closed set: one slot per setting this engine sets. Adding a setting
        /// means growing this — the assert in Append is what says so out loud.
        /// duplicate_message_limit + validate_sync + gpuav_enable +
        /// gpuav_shader_instrumentation + its three instrumentation sub-checks +
        /// gpuav_buffers_validation + its five individual buffer checks.
        static constexpr size_t kMaxSettings = 13;

        void Append(const char* layerName,
                    const char* settingName,
                    VkLayerSettingTypeEXT type,
                    const void* value) noexcept
        {
            assert(m_Count < kMaxSettings && "grow kMaxSettings before adding a layer setting");
            VkLayerSettingEXT& setting = m_Settings[m_Count++];
            setting.pLayerName = layerName;
            setting.pSettingName = settingName;
            setting.type = type;
            setting.valueCount = 1;
            setting.pValues = value;
        }

        const uint32_t m_DuplicateMessageLimit = kDuplicateMessageLimitUnlimited;
        const VkBool32 m_SyncValidation;
        const VkBool32 m_GpuAvEnable;
        const VkBool32 m_GpuAvShaderInstrumentation;
        /// One storage slot behind every pinned-on sub-check: the buffer-validation
        /// family with its five checks, and the three instrumentation checks when
        /// instrumentation is requested at all. None of them is ever asked for
        /// individually off, and VkLayerSettingEXT only borrows the value.
        /// Splitting them out is what making any one of them conditional would take.
        const VkBool32 m_GpuAvSubCheckPinnedOn = VK_TRUE;
        std::array<VkLayerSettingEXT, kMaxSettings> m_Settings{};
        uint32_t m_Count = 0;
    };

#endif // VK_EXT_layer_settings

} // namespace Rendering
} // namespace GameEngine
