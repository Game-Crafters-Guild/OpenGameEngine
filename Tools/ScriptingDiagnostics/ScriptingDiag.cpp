#include "Scripting/ScriptingABI.h"
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <fstream>
#include <string>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif


#include <thread>
#include <chrono>

#include <atomic>

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string FindTestAssembly()
{
    auto cwd = std::filesystem::current_path();
    std::vector<std::filesystem::path> candidates = {
        cwd / "DomainRoutingTest.dll",
        cwd / "build" / "DomainRoutingTest.dll",
        cwd / "Tests" / "ManagedTestAssemblies" / "DomainRoutingTest" / "bin" / "Debug" / "net10.0" / "DomainRoutingTest.dll",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p.string();
    }
    return std::string();
}

static void PrintPathMask(int mask)
{
    std::printf("PathMask: 0x%02X\n", mask);
    if (mask < 0) return;
    struct { int bit; const char* name; } bits[] = {
        { 0x01, "Typed Query" },
        { 0x02, "Typed Invoke" },
        { 0x04, "Wrapper Query" },
        { 0x08, "Wrapper Invoke" },
        { 0x10, "Reflection Query" },
        { 0x20, "Reflection Invoke" },


    };
    for (auto& b : bits) {
        std::printf("  [%c] %s\n", (mask & b.bit) ? 'x' : ' ', b.name);
    }
}

static void PrintPerfCounters()
{
    long long packed = GE_DebugGetPerfCountersPacked64();
    if (packed == -1) {
        std::printf("PerfCounters: unavailable (-1)\n");
        return;
    }
    int dq = (int)( packed        & 0xFFFF);
    int rq = (int)((packed >> 16) & 0xFFFF);


    int di = (int)((packed >> 32) & 0xFFFF);
    int ri = (int)((packed >> 48) & 0xFFFF);
    std::printf("PerfCounters (DQ, RQ, DI, RI): %d, %d, %d, %d\n", dq, rq, di, ri);
}

// Diagnostics callback for reload events
static std::atomic<int> g_reloadFailed{0};
static std::string g_lastReason;
static void GE_CDECL Diag_OnReload(GE_ReloadStage stage, const char* reasonUtf8, void* userData)
{
    (void)userData;
    if (stage == GE_Reload_Failed) {
        g_reloadFailed.fetch_add(1);
        g_lastReason = reasonUtf8 ? std::string(reasonUtf8) : std::string();
    }
}


int main(int argc, char** argv)
{
    bool poke = false;
    bool preloadBad = false;
    std::string badPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: ScriptingDiag [--poke] [--preload-bad [path]]\n");
            std::printf("  --poke         Perform a minimal Query+Invoke to bump perf counters before printing them.\n");
            std::printf("  --preload-bad  Attempt GE_PreloadAssemblyContext with an invalid path (or the provided path) and report rc + diagnostics.\n");
            return 0;
        } else if (std::strcmp(argv[i], "--poke") == 0) {

            poke = true;
        } else if (std::strcmp(argv[i], "--preload-bad") == 0) {
            preloadBad = true;
            if (i + 1 < argc && argv[i+1][0] != '-') { badPath = argv[++i]; }
        }
    }


    // ABI version
    uint32_t abi = GE_ScriptingGetAbiVersion();

    std::printf("ABI: 0x%08X (major=%u minor=%u)\n", abi, (abi >> 16) & 0xFFFF, abi & 0xFFFF);

    // Capabilities mask
    uint32_t caps = 0;
    GE_Result rcCap = GE_Capabilities(&caps);
    std::printf("Capabilities rc=%d mask=0x%08X\n", (int)rcCap, caps);
#ifdef _WIN32
    {
        auto cwd = std::filesystem::current_path();
        auto p = cwd / "Managed" / "HotReload" / "bin" / "Debug" / "net10.0" / "GameEngine.HotReload.dll";
        if (std::filesystem::exists(p)) {
            ::SetEnvironmentVariableA("GE_HRM_PATH", p.string().c_str());
            std::printf("[Diag] GE_HRM_PATH=%s\n", p.string().c_str());
        }
    }
#endif


    if (preloadBad) {
        GE_DiagnosticsSink sink{}; sink.onCompilationEvent = nullptr; sink.onReloadEvent = &Diag_OnReload; sink.userData = nullptr;
        GE_SubscribeDiagnostics(&sink);
        const char* p = badPath.empty() ? "This/Path/Does/Not/Exist/Invalid.dll" : badPath.c_str();
        GE_Result rc = GE_PreloadAssemblyContext(p, (uint32_t)std::strlen(p));
        std::printf("[PRELOAD_BAD] path='%s' rc=%d\n", p, (int)rc);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::printf("[PRELOAD_BAD] reloadFailedCount=%d reason='%s'\n", g_reloadFailed.load(), g_lastReason.c_str());
        GE_UnsubscribeDiagnostics(&sink);
    }


    // Path mask and perf counters (before poke)
    int mask = GE_DebugGetPathMask();
    std::printf("GE_DebugGetPathMask rc=%d\n", mask < 0 ? -1 : 0);
    PrintPathMask(mask);

    if (poke) {
        std::printf("[POKE] Attempting minimal Query+Invoke...\n");
        std::string asmPath = FindTestAssembly();
        if (asmPath.empty()) {
            std::printf("[POKE] DomainRoutingTest.dll not found in expected locations.\n");
        } else {
            auto bytes = ReadAll(asmPath);
            GE_PackageEntry entry{}; entry.assembly.data = bytes.data(); entry.assembly.length = (uint32_t)bytes.size();
            GE_DomainHandle dom = 0ULL;
            if (GE_ScriptsDomainCreateFromPackage(&entry, 1, GE_Pkg_Collectible, &dom) == GE_Result_Ok) {
                const char* fq = "GameEngine.Scripts.ScriptsEntryPoint.Reset";
                uint64_t tok = 0ULL; int32_t out = 0;
                if (GE_QueryExport(dom, fq, (uint32_t)std::strlen(fq), &tok) == GE_Result_Ok) {
                    GE_InvokeByToken(dom, tok, &out);
                }
            } else {
                std::printf("[POKE] Failed to create domain from %s\n", asmPath.c_str());
            }
        }
    }

    PrintPerfCounters();

    return 0;
}

