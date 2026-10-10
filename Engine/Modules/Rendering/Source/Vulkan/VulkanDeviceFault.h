/**
 * @file VulkanDeviceFault.h
 * @brief Post-mortem GPU fault records via VK_EXT_device_fault.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace GameEngine {
namespace Rendering {

    /// Whether VK_EXT_device_fault is requested when the toggle says nothing.
    ///
    /// ON in the developer configs, OFF in Release. Retrieval costs nothing until
    /// a loss and a loss is exactly when it is too late to go and enable it — but
    /// enabling an extension changes what a shipped build asks of the driver, so
    /// a shipping run states the request rather than inheriting it. GE_DEV_DIAG is
    /// the existing Debug/DebugFast/RelWithDebInfo signal (root CMakeLists.txt).
#if defined(GE_DEV_DIAG)
    inline constexpr bool kDeviceFaultDefaultEnabled = true;
#else
    inline constexpr bool kDeviceFaultDefaultEnabled = false;
#endif

    /// Parse the GE_VK_DEVICE_FAULT toggle against the config default.
    ///
    /// Tri-state rather than the plain leading-character idiom the validation
    /// toggles use, because this one has a default to fall back to: unset keeps
    /// the config policy, while an explicit value overrides it in either
    /// direction — arming a Release investigation, or disarming a developer build
    /// to A/B whether the extension itself changed anything.
    inline bool ParseDeviceFaultEnabled(const char* envValue, bool defaultEnabled) noexcept
    {
        if (envValue == nullptr || envValue[0] == '\0')
        {
            return defaultEnabled;
        }
        return envValue[0] != '0' && envValue[0] != 'f' && envValue[0] != 'F';
    }

#if defined(VK_EXT_device_fault)

    /// Spelled-out name for a fault address type. A value these headers do not
    /// know (driver newer than the SDK) renders as its numeric value rather than
    /// being dropped, so an unfamiliar fault still reads as a fault.
    std::string DescribeDeviceFaultAddressType(VkDeviceFaultAddressTypeEXT type);

    /// The set of GPU virtual addresses a reported fault resolves to, half-open.
    struct DeviceFaultAddressRange
    {
        uint64_t Begin = 0;
        uint64_t End   = 0; ///< Exclusive.
    };

    /**
     * @brief Widen a reported fault address by its precision granule.
     *
     * The driver reports the faulting address only to a power-of-two precision,
     * so this range is what a buffer allocation gets matched against: the only
     * guarantee is that the faulting address lies in
     * [reportedAddress & ~(precision-1), reportedAddress | (precision-1)].
     * A driver may report more precisely than it claims, in which case
     * `reportedAddress` is a valid additional hint — which is why the formatter
     * prints both rather than the range alone.
     *
     * A precision of 0 (driver declining to state one) or a non-power-of-two
     * yields the single reported address, which keeps a malformed report
     * readable instead of producing a wild range.
     */
    DeviceFaultAddressRange ResolveDeviceFaultAddressRange(VkDeviceAddress reportedAddress,
                                                           VkDeviceSize addressPrecision) noexcept;

    /// Copy a driver-supplied fixed-size description field. Treats the array as
    /// bounded rather than NUL-terminated, so a driver that fills all
    /// VK_MAX_DESCRIPTION_SIZE bytes cannot walk the formatter off the end.
    std::string DescribeFaultDescriptionField(const char* description, size_t capacity);

    /// Cap on the records of each kind requested from the driver.
    ///
    /// The available counts arrive as bare driver-controlled `uint32_t`, and
    /// retrieval runs inside the device-lost handler. Sizing the arrays straight
    /// from those counts lets a nonsense count throw `std::bad_alloc` out of the
    /// one path whose entire job is to preserve the evidence — at UINT32_MAX the
    /// two arrays ask for ~103 GB and ~1.16 TB. Real faults report a handful of
    /// records, so this never bites in practice, and when it does bite the report
    /// names it rather than silently dropping the remainder.
    inline constexpr uint32_t kMaxFaultRecords = 256;

    /**
     * @brief Format retrieved fault records for the device-lost log.
     *
     * Returns a bracketed placeholder rather than an empty string so the log line
     * always separates "the driver returned no records" from "we never asked" —
     * an instrument whose silence is ambiguous is not an instrument.
     */
    std::string FormatDeviceFaultInfo(const VkDeviceFaultInfoEXT& info,
                                      const VkDeviceFaultAddressInfoEXT* addressInfos,
                                      uint32_t addressInfoCount,
                                      const VkDeviceFaultVendorInfoEXT* vendorInfos,
                                      uint32_t vendorInfoCount);

    /**
     * @brief Run the extension's two-call retrieval and format what comes back.
     *
     * Valid only after the device has been lost (VUID-vkGetDeviceFaultInfoEXT-
     * device-07336) — that is the extension's whole purpose, and a synthesized
     * loss must not reach here. The opaque vendor binary blob is deliberately not
     * requested: it needs a vendor-specific offline tool to mean anything, while
     * the address and vendor-info records are readable here.
     *
     * The retrieval call is made even when the driver reports zero records,
     * because `VkDeviceFaultInfoEXT::description` — the driver's own sentence
     * about the fault — is only ever populated by that second call.
     *
     * VK_INCOMPLETE has three independent causes (a short address array, a short
     * vendor array, a short vendor-binary buffer), so the returned string names
     * which member was actually short. Declining the vendor binary sets its
     * capacity to zero, which is a VK_INCOMPLETE cause all on its own with no
     * record lost at all; reporting that as dropped records would send the next
     * investigation looking for evidence that was never missing.
     */
    std::string QueryAndFormatDeviceFault(PFN_vkGetDeviceFaultInfoEXT getFaultInfo, VkDevice device);

#endif // VK_EXT_device_fault

} // namespace Rendering
} // namespace GameEngine
