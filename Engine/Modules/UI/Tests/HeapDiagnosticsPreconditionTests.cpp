// Precondition on the PROCESS, not on any UI behaviour (#842).
//
// Windows can attach heap diagnostics to an image by name, out of band from the
// build, through Image File Execution Options. The user-mode stack trace
// database (`+ust`) is the expensive one: ntdll then records a captured call
// stack for every allocation and every free in the process, and that database
// only grows for the life of the process. Cost per allocation therefore rises
// with how long the process has been running.
//
// This suite creates and destroys a headless Vulkan device per device-backed
// test, and the ICDs allocate heavily inside vkCreateInstance/vkCreateDevice, so
// the suite is close to a worst case: measured on this repo, per-test cost went
// from 3 ms early to 8.8 s by test 400 in one process with the flag set, and the
// whole 1092-test suite ran in 152 s with it clear. The flag is invisible —
// nothing in the build, the command line or the log mentions it — so a run that
// takes hours looks like a code regression. Fail loudly at the front of the
// suite instead.
//
// Set GE_ALLOW_HEAP_DIAGNOSTICS=1 when the flags are deliberate, which is what
// leaves them set in the first place: they are the right tool for hunting a leak
// (#803 was found this way) and the wrong state to leave a machine in.

#include <gtest/gtest.h>

#ifdef _WIN32

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace
{

// NtGlobalFlag bits that change heap behaviour enough to invalidate both the
// timing and the memory profile of a run. Names as gflags.exe reports them.
struct HeapDiagnosticFlag
{
    unsigned long Bit;
    const char* Abbreviation;
    const char* Description;
};

constexpr HeapDiagnosticFlag kHeapDiagnosticFlags[] = {
    {0x00000010ul, "htc", "enable heap tail checking"},
    {0x00000020ul, "hfc", "enable heap free checking"},
    {0x00000040ul, "hpc", "enable heap parameter checking"},
    {0x00000080ul, "hvc", "enable heap validation on call"},
    {0x00001000ul, "ust", "create user mode stack trace database"},
    {0x00800000ul, "vrf", "enable application verifier"},
    {0x02000000ul, "hpa", "enable page heap"},
};

unsigned long QueryNtGlobalFlags()
{
    // RtlGetNtGlobalFlags is a stable ntdll export; resolving it dynamically
    // keeps this file off the DDK import libraries.
    using RtlGetNtGlobalFlagsFn = unsigned long(NTAPI*)();
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return 0ul;
    const auto fn =
        reinterpret_cast<RtlGetNtGlobalFlagsFn>(::GetProcAddress(ntdll, "RtlGetNtGlobalFlags"));
    return fn ? fn() : 0ul;
}

bool DiagnosticsExplicitlyAllowed()
{
    const char* v = std::getenv("GE_ALLOW_HEAP_DIAGNOSTICS");
    return v && v[0] != '\0' && v[0] != '0';
}

} // namespace

TEST(HeapDiagnosticsPrecondition, ProcessRunsWithoutHeapDiagnosticFlags)
{
    if (DiagnosticsExplicitlyAllowed())
        GTEST_SKIP() << "GE_ALLOW_HEAP_DIAGNOSTICS is set; heap diagnostics are intentional here.";

    const unsigned long flags = QueryNtGlobalFlags();

    std::vector<std::string> active;
    for (const HeapDiagnosticFlag& f : kHeapDiagnosticFlags)
    {
        if ((flags & f.Bit) != 0ul)
            active.push_back(std::string(f.Abbreviation) + " (" + f.Description + ")");
    }
    if (active.empty())
        return;

    std::string list;
    for (const std::string& a : active)
        list += "\n    " + a;

    // The image name is what IFEO keys on, so it is the whole of the fix.
    char path[MAX_PATH] = {};
    ::GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    const std::string name = leaf ? leaf + 1 : path;

    ADD_FAILURE() << "This process is running under Windows heap diagnostics (NtGlobalFlag=0x"
                  << std::hex << flags << std::dec << "):" << list
                  << "\n\n  Every allocation and free takes the instrumented path, so this run's"
                  << "\n  timings and memory figures are not the engine's. With the stack trace"
                  << "\n  database on, per-test cost also grows with position in the run, because"
                  << "\n  the database only grows (#842)."
                  << "\n\n  Clear it (elevated):"
                  << "\n    gflags.exe -i " << name << " -ust"
                  << "\n  or delete the whole entry:"
                  << "\n    reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
                     "\\Image File Execution Options\\"
                  << name << "\" /f"
                  << "\n\n  Set GE_ALLOW_HEAP_DIAGNOSTICS=1 if the flags are deliberate.";
}

#endif // _WIN32
