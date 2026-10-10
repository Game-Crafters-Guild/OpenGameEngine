#include "Logger/Backtrace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace Logger
{

namespace
{

#if defined(__APPLE__) || defined(__linux__)

// backtrace_symbols lines on Apple look like:
//   "3   Editor                              0x0000000100001234 _ZN9GameEngine3BarEv + 36"
// On Linux they look like:
//   "./Editor(_ZN9GameEngine3BarEv+0x24) [0x100001234]"
// We best-effort parse a human-friendly {module, symbol} pair and demangle.
String DemangleIfCxx(const char* mangled)
{
    if (!mangled || !*mangled)
        return {};
    int status = 0;
    char* buf = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && buf)
    {
        String out(buf);
        std::free(buf);
        return out;
    }
    if (buf)
        std::free(buf);
    return String(mangled);
}

void ParseSymbolLineApple(const char* line, BacktraceFrame& out)
{
    // skip leading frame index and whitespace
    const char* p = line;
    while (*p && *p == ' ') ++p;
    while (*p && *p >= '0' && *p <= '9') ++p;
    while (*p && *p == ' ') ++p;

    // module name (non-space run)
    const char* moduleStart = p;
    while (*p && *p != ' ') ++p;
    if (p > moduleStart)
        out.Module.assign(moduleStart, p - moduleStart);
    while (*p && *p == ' ') ++p;

    // address
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        // already stored separately from backtrace() return
        while (*p && *p != ' ') ++p;
        while (*p && *p == ' ') ++p;
    }

    // symbol up to " + offset"
    const char* symStart = p;
    const char* plus = std::strstr(p, " + ");
    String mangled;
    if (plus)
        mangled.assign(symStart, plus - symStart);
    else
        mangled.assign(symStart);

    out.Symbol = DemangleIfCxx(mangled.c_str());
}

#endif

} // namespace

std::mutex& DbgHelpMutex()
{
    static std::mutex s_Mutex;
    return s_Mutex;
}

Vector<BacktraceFrame> CaptureBacktrace(int maxFrames, int skipFrames)
{
    Vector<BacktraceFrame> out;
    if (maxFrames <= 0)
        return out;
    if (skipFrames < 0)
        skipFrames = 0;

#if defined(__APPLE__) || defined(__linux__)
    // Add 2 extra slots so we can reliably skip CaptureBacktrace itself.
    const int alloc = std::min(maxFrames + skipFrames + 2, 256);
    void* addrs[256];
    int captured = ::backtrace(addrs, alloc);
    if (captured <= 0)
        return out;

    // Always skip this function's own frame.
    const int skip = std::min(captured, skipFrames + 1);
    const int start = skip;
    const int count = std::min(captured - start, maxFrames);
    if (count <= 0)
        return out;

    char** symbols = ::backtrace_symbols(addrs + start, count);
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        BacktraceFrame f;
        f.Address = reinterpret_cast<std::uintptr_t>(addrs[start + i]);

        if (symbols && symbols[i])
        {
#if defined(__APPLE__)
            ParseSymbolLineApple(symbols[i], f);
#else
            // Linux: fall back to raw symbol line for now.
            f.Symbol = symbols[i];
#endif
        }

        // Resolve module name via dladdr if not set yet.
        if (f.Module.empty())
        {
            Dl_info info{};
            if (dladdr(addrs[start + i], &info) && info.dli_fname)
            {
                const char* base = std::strrchr(info.dli_fname, '/');
                f.Module = base ? String(base + 1) : String(info.dli_fname);
            }
        }

        out.push_back(std::move(f));
    }
    if (symbols)
        std::free(symbols);
    return out;

#elif defined(_WIN32)
    // dbghelp is process-global and single-threaded; every call below (init,
    // SymFromAddr, SymGetLineFromAddr64) holds the shared mutex so concurrent
    // Error-log backtraces and the hang watchdog's stack walks serialize.
    std::lock_guard<std::mutex> dbghelpLock(DbgHelpMutex());

    static std::atomic<bool> s_symInitialized{false};
    HANDLE process = GetCurrentProcess();
    if (!s_symInitialized.exchange(true))
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
    }

    void* addrs[128];
    const int alloc = std::min(maxFrames + skipFrames + 1, 128);
    USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(skipFrames + 1), static_cast<DWORD>(alloc), addrs, nullptr);
    const int count = std::min<int>(captured, maxFrames);
    out.reserve(static_cast<size_t>(count));

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    symbol->MaxNameLen = 511;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (int i = 0; i < count; ++i)
    {
        BacktraceFrame f;
        f.Address = reinterpret_cast<std::uintptr_t>(addrs[i]);

        if (SymFromAddr(process, static_cast<DWORD64>(f.Address), nullptr, symbol))
            f.Symbol = symbol->Name;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD disp = 0;
        if (SymGetLineFromAddr64(process, static_cast<DWORD64>(f.Address), &disp, &line))
        {
            if (line.FileName)
                f.File = line.FileName;
            f.Line = static_cast<int>(line.LineNumber);
        }

        HMODULE mod = nullptr;
        if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addrs[i]), &mod))
        {
            char modName[MAX_PATH];
            if (GetModuleFileNameA(mod, modName, MAX_PATH))
            {
                const char* base = std::strrchr(modName, '\\');
                f.Module = base ? String(base + 1) : String(modName);
            }
        }

        out.push_back(std::move(f));
    }
    return out;

#else
    (void)maxFrames;
    (void)skipFrames;
    return out;
#endif
}

} // namespace Logger
