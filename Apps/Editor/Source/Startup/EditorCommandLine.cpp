#include "Startup/EditorCommandLine.h"

#include <string>
#include <string_view>
#include <system_error>
#include <cstdlib>

namespace GameEngine::Editor::Startup
{
namespace
{
// A TCP port the debug server can actually bind: 1-65535, fully consumed by the
// integer parse (so "80x" is rejected rather than read as 80).
std::optional<std::uint16_t> ParsePort(const char* text)
{
    if (!text || !*text)
        return std::nullopt;

    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || (end && *end != '\0'))
        return std::nullopt;
    if (parsed <= 0 || parsed > 65535)
        return std::nullopt;

    return static_cast<std::uint16_t>(parsed);
}
} // namespace

EditorCommandLineArgs ParseEditorCommandLine(int argc, char** argv)
{
    namespace fs = std::filesystem;

    EditorCommandLineArgs out{};

    fs::path rawProject;
    fs::path rawLog;
    bool hasProject = false;
    bool hasLog = false;

    fs::path rawUiReplayScenario;
    fs::path rawUiReplayLog;
    bool hasUiReplayScenario = false;
    bool hasUiReplayLog = false;
    std::optional<std::uint64_t> exitAfterFrames;
    fs::path rawBenchRgCsv;
    bool hasBenchRgCsv = false;
    std::optional<std::uint16_t> debugPort;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i] ? std::string_view(argv[i]) : std::string_view();

        // --project <path>
        if (arg == "--project" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            rawProject = fs::path(argv[i]);
            hasProject = !rawProject.empty();
            continue;
        }

        // --project=<path>
        if (arg.rfind("--project=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("--project=").size());
            if (!value.empty())
            {
                rawProject = fs::path(std::string(value));
                hasProject = !rawProject.empty();
            }
            continue;
        }

        // -logfile <path>
        if (arg == "-logfile" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            rawLog = fs::path(argv[i]);
            hasLog = !rawLog.empty();
            continue;
        }

        // -logfile=<path>
        if (arg.rfind("-logfile=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("-logfile=").size());
            if (!value.empty())
            {
                rawLog = fs::path(std::string(value));
                hasLog = !rawLog.empty();
            }
            continue;
        }

        // --ui-replay <path>
        if (arg == "--ui-replay" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            rawUiReplayScenario = fs::path(argv[i]);
            hasUiReplayScenario = !rawUiReplayScenario.empty();
            continue;
        }

        // --ui-replay=<path>
        if (arg.rfind("--ui-replay=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("--ui-replay=").size());
            if (!value.empty())
            {
                rawUiReplayScenario = fs::path(std::string(value));
                hasUiReplayScenario = !rawUiReplayScenario.empty();
            }
            continue;
        }

        // --ui-replay-log <path>
        if (arg == "--ui-replay-log" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            rawUiReplayLog = fs::path(argv[i]);
            hasUiReplayLog = !rawUiReplayLog.empty();
            continue;
        }

        // --ui-replay-log=<path>
        if (arg.rfind("--ui-replay-log=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("--ui-replay-log=").size());
            if (!value.empty())
            {
                rawUiReplayLog = fs::path(std::string(value));
                hasUiReplayLog = !rawUiReplayLog.empty();
            }
            continue;
        }

        // --bench-rg-csv <path>
        if (arg == "--bench-rg-csv" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            rawBenchRgCsv = fs::path(argv[i]);
            hasBenchRgCsv = !rawBenchRgCsv.empty();
            continue;
        }

        // --bench-rg-csv=<path>
        if (arg.rfind("--bench-rg-csv=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("--bench-rg-csv=").size());
            if (!value.empty())
            {
                rawBenchRgCsv = fs::path(std::string(value));
                hasBenchRgCsv = !rawBenchRgCsv.empty();
            }
            continue;
        }

        // --exit-after-frames <N>
        if (arg == "--exit-after-frames" && i + 1 < argc && argv[i + 1])
        {
            ++i;
            const char* v = argv[i];
            if (v && *v)
            {
                char* end = nullptr;
                const unsigned long long n = std::strtoull(v, &end, 10);
                if (end != v)
                    exitAfterFrames = static_cast<std::uint64_t>(n);
            }
            continue;
        }

        // --exit-after-frames=<N>
        if (arg.rfind("--exit-after-frames=", 0) == 0)
        {
            std::string_view value = arg.substr(std::string_view("--exit-after-frames=").size());
            if (!value.empty())
            {
                std::string tmp(value);
                char* end = nullptr;
                const unsigned long long n = std::strtoull(tmp.c_str(), &end, 10);
                if (end != tmp.c_str())
                    exitAfterFrames = static_cast<std::uint64_t>(n);
            }
            continue;
        }

        // --session-label <text> / --session-label=<text>
        // Free text, taken verbatim; the parser never invents or normalizes it,
        // because a mangled label is worse than no label.
        if (arg == "--session-label")
        {
            // Same guard as --debug-port: do not let a missing value swallow the
            // next flag. A label that must start with '-' has the =form.
            const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
            const char* value = (next && next[0] != '-') ? next : nullptr;
            if (value)
                ++i;
            if (value && *value)
            {
                out.sessionLabel = std::string(value);
                continue;
            }
            // Warn rather than run unlabelled: an editor whose label silently went
            // missing is exactly the ambiguous session this flag exists to remove.
            out.unrecognizedArgs.emplace_back(std::string(arg) + " <missing value>");
            continue;
        }

        if (arg.rfind("--session-label=", 0) == 0)
        {
            // Verbatim, including any leading '-': the value is a human label, not a flag.
            std::string_view value = arg.substr(std::string_view("--session-label=").size());
            if (!value.empty())
            {
                out.sessionLabel = std::string(value);
                continue;
            }
            out.unrecognizedArgs.emplace_back(arg);
            continue;
        }

        // --debug-port <N> / --debug-port=<N>
        // A malformed or out-of-range value is NOT silently dropped to the
        // default port — it falls through to unrecognizedArgs so the caller
        // warns, because the default is a port another editor may already own.
        if (arg == "--debug-port")
        {
            // Only claim the next token as the value if it is not itself a
            // flag: "--debug-port --project X" must not swallow --project.
            const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
            const char* value = (next && next[0] != '-') ? next : nullptr;
            if (value)
                ++i;
            if (const std::optional<std::uint16_t> parsed = ParsePort(value))
            {
                debugPort = parsed;
                continue;
            }
            // Name the value in the warning: "--debug-port" alone and
            // "--debug-port 0" fail for different reasons, and the developer
            // needs to know WHICH port they did not get.
            out.unrecognizedArgs.emplace_back(std::string(arg) + " " +
                                             (value ? value : "<missing value>"));
            continue;
        }

        if (arg.rfind("--debug-port=", 0) == 0)
        {
            const std::string value(arg.substr(std::string_view("--debug-port=").size()));
            if (const std::optional<std::uint16_t> parsed = ParsePort(value.c_str()))
            {
                debugPort = parsed;
                continue;
            }
            out.unrecognizedArgs.emplace_back(arg);
            continue;
        }

        // Unknown dash-prefixed argument. Engine flags are parsed separately by
        // ParseEngineArgs from the same argv, so they are not "unknown" here.
        if (arg.size() > 1 && arg.front() == '-' && arg.rfind("--engine-", 0) != 0)
        {
            out.unrecognizedArgs.emplace_back(arg);
        }
    }

    if (hasProject)
    {
        std::error_code ec;
        fs::path abs = fs::absolute(rawProject, ec);
        fs::path normalized = ec ? rawProject : abs;
        ec.clear();

        fs::path canon = fs::weakly_canonical(normalized, ec);
        if (!ec)
        {
            normalized = canon;
        }

        out.projectRoot = normalized.lexically_normal();
    }

    if (hasLog)
    {
        std::error_code ec;
        fs::path p = rawLog;

        if (!p.is_absolute())
        {
            // If the user also provided a project, interpret logfile relative to it (more predictable).
            fs::path base;
            if (out.projectRoot.has_value())
            {
                base = *out.projectRoot;
            }
            else
            {
                base = fs::current_path(ec);
                if (ec)
                {
                    ec.clear();
                }
            }

            if (!base.empty())
            {
                p = (base / p).lexically_normal();
            }
        }

        fs::path abs = fs::absolute(p, ec);
        out.logFile = (ec ? p : abs).lexically_normal();
    }

    // Helper: resolve a path relative to project root (if provided) or CWD.
    auto resolvePath = [&](const fs::path& raw) -> fs::path
    {
        std::error_code ec;
        fs::path p = raw;
        if (!p.is_absolute())
        {
            fs::path base;
            if (out.projectRoot.has_value())
                base = *out.projectRoot;
            else
                base = fs::current_path(ec);
            if (!base.empty())
                p = (base / p).lexically_normal();
        }

        fs::path abs = fs::absolute(p, ec);
        fs::path normalized = (ec ? p : abs).lexically_normal();
        ec.clear();
        fs::path canon = fs::weakly_canonical(normalized, ec);
        if (!ec)
            normalized = canon.lexically_normal();
        return normalized;
    };

    if (hasUiReplayScenario)
    {
        out.uiReplayScenario = resolvePath(rawUiReplayScenario);
    }
    if (hasUiReplayLog)
    {
        // Do not canonicalize to existing file; ensure the path is absolute/normalized only.
        std::error_code ec;
        fs::path p = rawUiReplayLog;
        if (!p.is_absolute())
        {
            fs::path base;
            if (out.projectRoot.has_value())
                base = *out.projectRoot;
            else
                base = fs::current_path(ec);
            if (!base.empty())
                p = (base / p).lexically_normal();
        }
        fs::path abs = fs::absolute(p, ec);
        out.uiReplayLogFile = (ec ? p : abs).lexically_normal();
    }

    out.exitAfterFrames = exitAfterFrames;
    out.debugPort = debugPort;

    if (hasBenchRgCsv)
    {
        // Write path: resolve relative-to-project (if --project supplied) or cwd, but do NOT
        // canonicalize against existing files — the CSV file likely doesn't exist yet.
        std::error_code ec;
        fs::path p = rawBenchRgCsv;
        if (!p.is_absolute())
        {
            fs::path base;
            if (out.projectRoot.has_value())
                base = *out.projectRoot;
            else
                base = fs::current_path(ec);
            if (!base.empty())
                p = (base / p).lexically_normal();
        }
        fs::path abs = fs::absolute(p, ec);
        out.benchRgCsv = (ec ? p : abs).lexically_normal();
    }

    return out;
}
} // namespace GameEngine::Editor::Startup

