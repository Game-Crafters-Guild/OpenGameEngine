#include "NativeScripting/ToolchainFingerprint.h"

namespace GameEngine
{
namespace NativeScripting
{

namespace
{
const char* VendorName(uint32_t vendor)
{
    switch (vendor)
    {
        case GE_COMPILER_MSVC:  return "MSVC";
        case GE_COMPILER_CLANG: return "Clang";
        case GE_COMPILER_GCC:   return "GCC";
        default:                return "unknown";
    }
}

void AppendMismatch(std::string& out, const char* field, uint32_t host, uint32_t module)
{
    if (host == module)
        return;
    if (!out.empty())
        out += "; ";
    out += field;
    out += " mismatch (host=" + std::to_string(host) + ", module=" + std::to_string(module) + ")";
}
} // namespace

GE_ToolchainFingerprint HostToolchainFingerprint()
{
    GE_ToolchainFingerprint fp{};
    GE_FillToolchainFingerprint(&fp);
    return fp;
}

std::string DescribeToolchainFingerprintMismatch(const GE_ToolchainFingerprint& host,
                                                 const GE_ToolchainFingerprint& module)
{
    std::string out;
    if (host.CompilerVendor != module.CompilerVendor)
    {
        out += "CompilerVendor mismatch (host=";
        out += VendorName(host.CompilerVendor);
        out += ", module=";
        out += VendorName(module.CompilerVendor);
        out += ")";
    }
    AppendMismatch(out, "CompilerVersionMajor", host.CompilerVersionMajor, module.CompilerVersionMajor);
    AppendMismatch(out, "CompilerVersionMinor", host.CompilerVersionMinor, module.CompilerVersionMinor);
    AppendMismatch(out, "CrtId", host.CrtId, module.CrtId);
    AppendMismatch(out, "IteratorDebugLevel", host.IteratorDebugLevel, module.IteratorDebugLevel);
    AppendMismatch(out, "ZcFlags", host.ZcFlags, module.ZcFlags);
    AppendMismatch(out, "StdLibAbiTag", host.StdLibAbiTag, module.StdLibAbiTag);
    return out;
}

} // namespace NativeScripting
} // namespace GameEngine
