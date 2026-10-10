#include "VulkanDeviceFault.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace GameEngine {
namespace Rendering {

#if defined(VK_EXT_device_fault)

namespace
{
// Fixed-size hex/decimal scratch for one formatted field. Wide enough for
// "0x" + 16 hex digits + terminator with room to spare.
constexpr size_t kFormatScratch = 64;

std::string HexU64(uint64_t value)
{
    char buffer[kFormatScratch];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string DecU64(uint64_t value)
{
    char buffer[kFormatScratch];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return buffer;
}

bool IsPowerOfTwo(uint64_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}
} // namespace

std::string DescribeDeviceFaultAddressType(VkDeviceFaultAddressTypeEXT type)
{
    switch (type)
    {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:                        return "NONE";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:                return "READ_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:               return "WRITE_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:             return "EXECUTE_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "IP_UNKNOWN";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "IP_INVALID";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:   return "IP_FAULT";
    default:
        break;
    }
    char buffer[kFormatScratch];
    std::snprintf(buffer, sizeof(buffer), "UNKNOWN(%d)", static_cast<int>(type));
    return buffer;
}

DeviceFaultAddressRange ResolveDeviceFaultAddressRange(VkDeviceAddress reportedAddress,
                                                       VkDeviceSize addressPrecision) noexcept
{
    // A precision that is not a positive power of two cannot define a granule;
    // reporting the bare address beats synthesizing a range from a bad mask.
    if (!IsPowerOfTwo(static_cast<uint64_t>(addressPrecision)))
    {
        return DeviceFaultAddressRange{static_cast<uint64_t>(reportedAddress),
                                       static_cast<uint64_t>(reportedAddress) + 1};
    }
    const uint64_t precision = static_cast<uint64_t>(addressPrecision);
    const uint64_t begin     = static_cast<uint64_t>(reportedAddress) & ~(precision - 1);
    return DeviceFaultAddressRange{begin, begin + precision};
}

std::string DescribeFaultDescriptionField(const char* description, size_t capacity)
{
    if (description == nullptr || capacity == 0)
    {
        return {};
    }
    size_t length = 0;
    while (length < capacity && description[length] != '\0')
    {
        ++length;
    }
    return std::string(description, length);
}

std::string FormatDeviceFaultInfo(const VkDeviceFaultInfoEXT& info,
                                  const VkDeviceFaultAddressInfoEXT* addressInfos,
                                  uint32_t addressInfoCount,
                                  const VkDeviceFaultVendorInfoEXT* vendorInfos,
                                  uint32_t vendorInfoCount)
{
    std::string out = "description='";
    out += DescribeFaultDescriptionField(info.description, VK_MAX_DESCRIPTION_SIZE);
    out += "'";

    if (addressInfos == nullptr || addressInfoCount == 0)
    {
        out += "; address records: <none reported>";
    }
    else
    {
        out += "; address records (" + DecU64(addressInfoCount) + "):";
        for (uint32_t i = 0; i < addressInfoCount; ++i)
        {
            const VkDeviceFaultAddressInfoEXT& address = addressInfos[i];
            const DeviceFaultAddressRange range =
                ResolveDeviceFaultAddressRange(address.reportedAddress, address.addressPrecision);
            out += " [" + DecU64(i) + "] " + DescribeDeviceFaultAddressType(address.addressType);
            out += " reported=" + HexU64(static_cast<uint64_t>(address.reportedAddress));
            out += " precision=" + DecU64(static_cast<uint64_t>(address.addressPrecision));
            // A buffer VA is matched against the range; the reported address is
            // printed beside it because a driver may be more precise than it claims.
            out += " range=[" + HexU64(range.Begin) + "," + HexU64(range.End) + ")";
            if (i + 1 < addressInfoCount)
            {
                out += ";";
            }
        }
    }

    if (vendorInfos == nullptr || vendorInfoCount == 0)
    {
        out += "; vendor records: <none reported>";
    }
    else
    {
        out += "; vendor records (" + DecU64(vendorInfoCount) + "):";
        for (uint32_t i = 0; i < vendorInfoCount; ++i)
        {
            const VkDeviceFaultVendorInfoEXT& vendor = vendorInfos[i];
            out += " [" + DecU64(i) + "] '";
            out += DescribeFaultDescriptionField(vendor.description, VK_MAX_DESCRIPTION_SIZE);
            out += "' code=" + HexU64(vendor.vendorFaultCode);
            out += " data=" + HexU64(vendor.vendorFaultData);
            if (i + 1 < vendorInfoCount)
            {
                out += ";";
            }
        }
    }

    return out;
}

std::string QueryAndFormatDeviceFault(PFN_vkGetDeviceFaultInfoEXT getFaultInfo, VkDevice device)
{
    if (getFaultInfo == nullptr || device == VK_NULL_HANDLE)
    {
        return "<device-fault retrieval unavailable>";
    }

    VkDeviceFaultCountsEXT counts{};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult result = getFaultInfo(device, &counts, nullptr);
    if (result != VK_SUCCESS)
    {
        return "<device-fault count query failed (VkResult=" + std::to_string(static_cast<int>(result)) + ")>";
    }
    // What the driver says it holds. The spec pins these identical across sizing
    // calls, so they are the denominator every "did we lose any" answer below is
    // measured against — and they have to be read before the capacities overwrite
    // them in the same structure.
    const uint32_t     availableAddressCount     = counts.addressInfoCount;
    const uint32_t     availableVendorCount      = counts.vendorInfoCount;
    const VkDeviceSize availableVendorBinarySize = counts.vendorBinarySize;

    const uint32_t addressCapacity = std::min(availableAddressCount, kMaxFaultRecords);
    const uint32_t vendorCapacity  = std::min(availableVendorCount, kMaxFaultRecords);

    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(addressCapacity);
    std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(vendorCapacity);

    // Asked for even when both counts are zero: `description` is the driver's own
    // sentence about the fault and only the second call fills it in.
    VkDeviceFaultInfoEXT info{};
    info.sType         = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
    info.pVendorInfos  = vendorInfos.empty() ? nullptr : vendorInfos.data();
    // Left null on purpose: the vendor binary blob only means something to a
    // vendor's offline tool, and asking for it makes the driver stage it. A zero
    // capacity against a nonzero available size is a VK_INCOMPLETE cause by
    // itself, which is why the note below has to name which member was short.
    info.pVendorBinaryData  = nullptr;
    counts.addressInfoCount = addressCapacity;
    counts.vendorInfoCount  = vendorCapacity;
    counts.vendorBinarySize = 0;

    // VK_INCOMPLETE is a success code: the driver filled what fits and has more to
    // give. Treating it as a failure would discard the records we did get, which is
    // the whole reason for asking.
    result = getFaultInfo(device, &counts, &info);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        return "<device-fault record query failed (VkResult=" + std::to_string(static_cast<int>(result)) + ")>";
    }

    // The second call reports what it actually wrote; never format past that.
    const uint32_t writtenAddressCount = std::min(counts.addressInfoCount, addressCapacity);
    const uint32_t writtenVendorCount  = std::min(counts.vendorInfoCount, vendorCapacity);

    std::string report = FormatDeviceFaultInfo(info,
                                               addressInfos.empty() ? nullptr : addressInfos.data(),
                                               writtenAddressCount,
                                               vendorInfos.empty() ? nullptr : vendorInfos.data(),
                                               writtenVendorCount);

    if (writtenAddressCount < availableAddressCount || writtenVendorCount < availableVendorCount)
    {
        report += "; NOTE: records TRUNCATED (" + DecU64(writtenAddressCount) + "/" +
                  DecU64(availableAddressCount) + " address, " + DecU64(writtenVendorCount) + "/" +
                  DecU64(availableVendorCount) + " vendor)";
        if (availableAddressCount > kMaxFaultRecords || availableVendorCount > kMaxFaultRecords)
        {
            report += " — capped at " + DecU64(kMaxFaultRecords) + " records per array";
        }
    }
    else if (availableVendorBinarySize > 0)
    {
        // Nothing was lost: the one thing the driver still holds is the blob we
        // declined. Reported anyway because with deviceFaultVendorBinary off a
        // conformant driver must report zero here, so a nonzero size says
        // something about the driver as well as about the fault.
        report += "; NOTE: all records retrieved; driver also holds a " +
                  DecU64(static_cast<uint64_t>(availableVendorBinarySize)) +
                  "-byte vendor binary crash dump (not requested; deviceFaultVendorBinary is off, so a "
                  "conformant driver reports 0 here)";
    }
    else if (result == VK_INCOMPLETE)
    {
        report += "; NOTE: driver returned VK_INCOMPLETE but reported nothing short — its record counts "
                  "are not stable across calls, so records may be missing that these numbers cannot name";
    }
    return report;
}

#endif // VK_EXT_device_fault

} // namespace Rendering
} // namespace GameEngine
