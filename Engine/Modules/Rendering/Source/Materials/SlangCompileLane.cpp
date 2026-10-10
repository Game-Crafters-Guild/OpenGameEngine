#include "SlangCompileLane.h"

#include "Logger/Logger.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace GameEngine { namespace Rendering { namespace SlangLane {

namespace
{
namespace fs = std::filesystem;

constexpr uint32_t kSlangcTimeoutMs = 60000u;

std::string GetEnvString(const char* name)
{
#ifdef _WIN32
    // getenv reads the CRT's startup snapshot; tests toggle the lane in-process
    // via _putenv_s, which GetEnvironmentVariable sees live.
    char buf[1024];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return {};
    return std::string(buf, n);
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

std::string SlangcPathFromEnv()
{
    return GetEnvString("GE_SLANGC");
}

std::string Trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

#ifdef _WIN32

// Standard Windows argv quoting: quote when needed, double backslash runs that
// precede a quote or the closing quote.
void AppendQuotedArg(std::wstring& cmd, const std::wstring& arg)
{
    if (!cmd.empty())
        cmd += L' ';
    const bool needsQuotes =
        arg.empty() || arg.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuotes)
    {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t backslashes = 0;
    for (const wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
        {
            cmd.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
            cmd += L'"';
            continue;
        }
        cmd.append(backslashes, L'\\');
        backslashes = 0;
        cmd += c;
    }
    cmd.append(backslashes * 2, L'\\');
    cmd += L'"';
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Run a tool with stdout+stderr captured into one stream. Returns false on
// spawn/timeout failure (outExitCode untouched); tool failures return true
// with the tool's exit code.
bool RunToolCaptured(const std::string& exePath, const std::vector<std::string>& args,
                     int& outExitCode, std::string& outOutput, std::string* outError)
{
    std::wstring cmd;
    AppendQuotedArg(cmd, Widen(exePath));
    for (const auto& a : args)
        AppendQuotedArg(cmd, Widen(a));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        if (outError)
            *outError = "SlangLane: CreatePipe failed (err=" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    // The read end stays ours only; leaking it into the child would keep the
    // pipe open after child exit and hang the drain loop.
    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};

    const BOOL created = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(writePipe);
    if (!created)
    {
        ::CloseHandle(readPipe);
        if (outError)
            *outError = "SlangLane: CreateProcess failed for '" + exePath +
                        "' (err=" + std::to_string(::GetLastError()) + ")";
        return false;
    }

    // Drain until EOF (child exit closes its end); the child can't outlive the
    // drain by more than the wait timeout below.
    outOutput.clear();
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0)
        outOutput.append(buf, buf + n);
    ::CloseHandle(readPipe);

    const DWORD wait = ::WaitForSingleObject(pi.hProcess, kSlangcTimeoutMs);
    bool ok = true;
    if (wait != WAIT_OBJECT_0)
    {
        ::TerminateProcess(pi.hProcess, 1u);
        if (outError)
            *outError = "SlangLane: '" + exePath + "' timed out after " +
                        std::to_string(kSlangcTimeoutMs) + "ms";
        ok = false;
    }
    else
    {
        DWORD code = 1;
        ::GetExitCodeProcess(pi.hProcess, &code);
        outExitCode = static_cast<int>(code);
    }
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return ok;
}

#else // !_WIN32

bool RunToolCaptured(const std::string& exePath, const std::vector<std::string>&, int&,
                     std::string&, std::string* outError)
{
    if (outError)
        *outError = "SlangLane: subprocess runner is Windows-only in the spike (exe: " +
                    exePath + ")";
    return false;
}

#endif // _WIN32

// Probe `slangc -v` once per resolved path (the version prints to stderr).
bool ProbeVersion(const std::string& slangcPath, std::string& outVersion, std::string* outError)
{
    static std::mutex s_Mutex;
    static std::string s_CachedPath;
    static std::string s_CachedVersion;
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (slangcPath == s_CachedPath && !s_CachedVersion.empty())
    {
        outVersion = s_CachedVersion;
        return true;
    }

    int exitCode = 1;
    std::string output;
    if (!RunToolCaptured(slangcPath, {"-v"}, exitCode, output, outError))
        return false;
    const std::string version = Trim(output);
    if (exitCode != 0 || version.empty())
    {
        if (outError)
            *outError = "SlangLane: '" + slangcPath + " -v' failed (exit " +
                        std::to_string(exitCode) + "): " + version;
        return false;
    }
    s_CachedPath = slangcPath;
    s_CachedVersion = version;
    outVersion = version;
    return true;
}

// Parse a Makefile-style depfile ("target: dep dep ...") into dependency
// paths. slangc escapes ':' as "\:", spaces as "\ ", and '\' as "\\"; a
// trailing '\' continues the line.
std::vector<std::string> ParseDepfile(const std::string& text)
{
    std::vector<std::string> deps;
    std::string token;
    bool pastTarget = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size())
        {
            const char next = text[i + 1];
            if (next == '\\' || next == ':' || next == ' ')
            {
                token += next;
                ++i;
                continue;
            }
            if (next == '\n' || (next == '\r' && i + 2 < text.size() && text[i + 2] == '\n'))
            {
                // Line continuation: acts as a separator.
                i += (next == '\r') ? 2 : 1;
                if (!token.empty() && pastTarget)
                    deps.push_back(token);
                token.clear();
                continue;
            }
            token += c;
            continue;
        }
        if (c == ':' && !pastTarget)
        {
            // Unescaped colon ends the make target; deps follow.
            pastTarget = true;
            token.clear();
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (!token.empty() && pastTarget)
                deps.push_back(token);
            token.clear();
            continue;
        }
        token += c;
    }
    if (!token.empty() && pastTarget)
        deps.push_back(token);
    return deps;
}

std::string CanonicalAbs(const fs::path& p)
{
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec)
        abs = p;
    return abs.lexically_normal().string();
}

bool MapStageName(const std::string& stageKey, std::string& outSlangStage)
{
    if (stageKey == "vs") { outSlangStage = "vertex"; return true; }
    if (stageKey == "fs") { outSlangStage = "fragment"; return true; }
    if (stageKey == "cs") { outSlangStage = "compute"; return true; }
    if (stageKey == "gs") { outSlangStage = "geometry"; return true; }
    if (stageKey == "ms") { outSlangStage = "mesh"; return true; }
    return false;
}

} // namespace

bool Enabled()
{
    const std::string v = GetEnvString("GE_SHADER_SLANG_LANE");
    return !v.empty() && v != "0";
}

bool EnsureAvailable(std::string* outError)
{
    if (!Enabled())
    {
        if (outError)
            *outError = ".slang sources require the Slang dev lane: set GE_SHADER_SLANG_LANE=1 "
                        "and GE_SLANGC=<path to slangc.exe>";
        return false;
    }
    const std::string slangc = SlangcPathFromEnv();
    if (slangc.empty())
    {
        if (outError)
            *outError = "SlangLane: GE_SHADER_SLANG_LANE is set but GE_SLANGC is not — point it "
                        "at slangc.exe (the binary is NOT vendored; SlangCompileLane.h records "
                        "the expected Slang version and its download)";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(fs::path(slangc), ec))
    {
        if (outError)
            *outError = "SlangLane: GE_SLANGC does not exist: " + slangc;
        return false;
    }
    std::string version;
    return ProbeVersion(slangc, version, outError);
}

std::string KeyDiscriminator()
{
    std::string version;
    std::string err;
    if (!ProbeVersion(SlangcPathFromEnv(), version, &err))
        return "slang:unavailable"; // EnsureAvailable() already failed the compile loudly
    return "slang:" + version;
}

bool CompileStage(const StageInput& in, std::vector<uint32_t>& outWords,
                  std::string& outDiagnostics, std::vector<std::string>& outModuleDeps)
{
    outWords.clear();
    outModuleDeps.clear();

    std::string laneErr;
    if (!EnsureAvailable(&laneErr))
    {
        outDiagnostics = laneErr;
        return false;
    }

    std::string slangStage;
    if (!MapStageName(in.stageKey, slangStage))
    {
        outDiagnostics = "SlangLane: unsupported stage key '" + in.stageKey + "'";
        return false;
    }

    std::error_code ec;
    fs::create_directories(in.scratchDir, ec);

    // Unique scratch base: concurrent compiles (same or different keys) must
    // not collide. Output bytes are path-independent (no -g: slangc emits no
    // OpLine/OpString), so unique temp names cannot leak into the .spv.
    static std::atomic<uint64_t> s_Counter{0};
    std::ostringstream base;
#ifdef _WIN32
    base << "slang_" << ::GetCurrentProcessId() << "_" << s_Counter.fetch_add(1);
#else
    base << "slang_" << s_Counter.fetch_add(1);
#endif
    const fs::path spvPath = in.scratchDir / (base.str() + ".spv");
    const fs::path depPath = in.scratchDir / (base.str() + ".d");
    fs::path srcArg = in.sourcePath;
    fs::path inlineTemp;
    if (!in.inlineSource.empty())
    {
        inlineTemp = in.scratchDir / (base.str() + ".slang");
        std::ofstream f(inlineTemp, std::ios::binary);
        f.write(in.inlineSource.data(),
                static_cast<std::streamsize>(in.inlineSource.size()));
        if (!f.good())
        {
            outDiagnostics = "SlangLane: failed writing inline source to " + inlineTemp.string();
            return false;
        }
        f.close();
        srcArg = inlineTemp;
    }

    std::vector<std::string> args;
    args.push_back(srcArg.string());
    args.push_back("-target");
    args.push_back("spirv");
    args.push_back("-profile");
    args.push_back(in.targetSpirv16 ? "spirv_1_6" : "spirv_1_5");
    // Locked spike flag set (shader-authoring-story §10.1): -O0 AND
    // -preserve-params keep declaration-driven interfaces (Gate-1 trap T2 —
    // DCE'd resources desync the reflected descriptor layouts from the PSO);
    // column-major matches the engine's GLSL/std140 convention (trap D1).
    args.push_back("-matrix-layout-column-major");
    args.push_back("-O0");
    args.push_back("-preserve-params");
    args.push_back("-entry");
    args.push_back("main");
    args.push_back("-stage");
    args.push_back(slangStage);
    args.push_back("-o");
    args.push_back(spvPath.string());
    args.push_back("-depfile");
    args.push_back(depPath.string());
    for (const auto& d : in.includeDirs)
    {
        args.push_back("-I");
        args.push_back(d.string());
    }
    for (const auto& d : in.defines)
    {
        if (!d.empty())
            args.push_back("-D" + d);
    }

    auto cleanup = [&]() {
        std::error_code rmEc;
        fs::remove(spvPath, rmEc);
        fs::remove(depPath, rmEc);
        if (!inlineTemp.empty())
            fs::remove(inlineTemp, rmEc);
    };

    int exitCode = 1;
    std::string output;
    std::string runErr;
    if (!RunToolCaptured(SlangcPathFromEnv(), args, exitCode, output, &runErr))
    {
        outDiagnostics = runErr;
        cleanup();
        return false;
    }
    if (exitCode != 0)
    {
        outDiagnostics = "slangc exited with code " + std::to_string(exitCode) + ":\n" + output;
        cleanup();
        return false;
    }

    // Read the emitted SPIR-V.
    {
        std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
        if (!f.is_open())
        {
            outDiagnostics = "SlangLane: slangc succeeded but no output at " + spvPath.string();
            cleanup();
            return false;
        }
        const std::streamsize size = f.tellg();
        if (size <= 0 || (size % 4) != 0)
        {
            outDiagnostics = "SlangLane: emitted SPIR-V has invalid size " +
                             std::to_string(size) + " at " + spvPath.string();
            cleanup();
            return false;
        }
        outWords.resize(static_cast<size_t>(size) / 4u);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(outWords.data()), size);
        if (!f.good())
        {
            outDiagnostics = "SlangLane: failed reading emitted SPIR-V at " + spvPath.string();
            outWords.clear();
            cleanup();
            return false;
        }
    }

    // Module deps for include-hash revalidation. The root source's bytes are
    // already in the cache key (a root edit changes the key itself), so filter
    // it — and the inline scratch file MUST be filtered: it is deleted below,
    // and an unreadable include path would mark the cache entry permanently
    // stale.
    {
        std::ifstream f(depPath, std::ios::binary);
        if (f.is_open())
        {
            std::string text((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            const std::string rootA = CanonicalAbs(srcArg);
            const std::string rootB = CanonicalAbs(in.sourcePath);
            for (const auto& dep : ParseDepfile(text))
            {
                const std::string canonical = CanonicalAbs(fs::path(dep));
                if (canonical == rootA || canonical == rootB)
                    continue;
                outModuleDeps.push_back(canonical);
            }
        }
        else
        {
            // Fail loud, not silent: missing deps would skip revalidation and
            // serve stale SPIR-V after a module edit.
            Logger::Log::Warning(
                "SlangLane: depfile missing at {} — imported-module edits will "
                "not invalidate this cache entry", depPath.string());
        }
    }

    cleanup();
    return true;
}

}}} // namespace GameEngine::Rendering::SlangLane
