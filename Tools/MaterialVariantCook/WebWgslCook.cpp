#include "WebWgslCook.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace GameEngine::Tools
{
namespace
{
namespace fs = std::filesystem;

// The interpreter that runs shadercook.py. Overridable so a host whose default
// python3 is not the one with the toolchain can point at the right one.
const char* PythonInterpreter()
{
    if (const char* env = std::getenv("GE_PYTHON"))
        return env;
    return "python3";
}

bool WriteTextFile(const fs::path& path, const std::string& text, std::string& outError)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        outError = "cannot write " + path.string();
        return false;
    }
    out << text;
    return true;
}

bool ReadBytes(const fs::path& path, std::vector<uint8_t>& out, std::string& outError)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        outError = "cannot read " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// One stage through shadercook.py. The stage is inferred from the extension,
// so the scratch file must carry .vert / .frag.
bool CookStage(const WebWgslCookRequest& request, const std::string& stageKey,
               const std::string& source, const char* extension,
               std::vector<uint8_t>& outWgsl, std::string& outError)
{
    const std::string stem = request.DebugName + "_" + stageKey;
    const fs::path sourcePath = request.ScratchDir / (stem + extension);
    if (!WriteTextFile(sourcePath, source, outError))
        return false;

    std::vector<std::string> arguments;
    arguments.push_back(request.ShaderCookScript.string());
    arguments.emplace_back("--out-dir");
    arguments.push_back(request.ScratchDir.string());
    for (const std::string& define : request.Defines)
    {
        arguments.emplace_back("-D");
        arguments.push_back(define);
    }
    for (const fs::path& root : request.IncludeRoots)
    {
        arguments.emplace_back("-I");
        arguments.push_back(root.string());
    }
    arguments.push_back(sourcePath.string());

    // RunProcessCaptured spawns the interpreter directly and merges its stderr
    // into the captured output: no shell parses this line, and the chain's
    // rejections (glslc, naga, tint) all arrive on stderr, where a cook failure
    // is only actionable with that text attached.
    const std::string interpreter = PythonInterpreter();
    const ShellProcessResult run = RunProcessCaptured(interpreter, arguments);
    if (run.exitCode != 0)
    {
        // Name the composed source: the chain's diagnostics carry line numbers
        // into a file that only exists in the scratch dir. Name the interpreter
        // too: a failure with no captured output is the interpreter never
        // starting, and GE_PYTHON is the override that fixes that.
        outError = "WGSL cook failed for stage '" + stageKey + "' (" + sourcePath.string()
                   + "), interpreter '" + interpreter + "' exit "
                   + std::to_string(run.exitCode) + ":\n" + run.output;
        return false;
    }
    return ReadBytes(request.ScratchDir / (stem + ".wgsl"), outWgsl, outError);
}

} // namespace

bool CookWebWgslIntoPackage(const WebWgslCookRequest& request, std::string& outError)
{
    std::vector<uint8_t> vertexWgsl;
    std::vector<uint8_t> fragmentWgsl;
    if (!CookStage(request, "vs", request.VertexSource, ".vert", vertexWgsl, outError))
        return false;
    if (!CookStage(request, "fs", request.FragmentSource, ".frag", fragmentWgsl, outError))
        return false;

    // SPIR-V: the package is rewritten with its own SPIR-V chunks intact and
    // the freshly cooked WGSL added beside them.
    Rendering::ShaderPackage pkg{};
    if (!Rendering::LoadShaderPkg(request.PackagePath.string(), Rendering::ShaderSourceKind::SpirV,
                                  pkg, &outError))
        return false;

    std::unordered_map<std::string, std::vector<uint8_t>> wgslBytes;
    wgslBytes["vs"] = std::move(vertexWgsl);
    wgslBytes["fs"] = std::move(fragmentWgsl);
    return Rendering::SaveShaderPkg(request.PackagePath, pkg.meta, pkg.stageBytes,
                                    pkg.cacheInfoJson, &outError, wgslBytes);
}

namespace
{
// One worker: claims the next unclaimed request until none is left.
void CookBatchWorker(const std::vector<WebWgslCookRequest>& requests, std::atomic<size_t>& next,
                     std::vector<std::string>& errors)
{
    for (size_t index = next.fetch_add(1); index < requests.size(); index = next.fetch_add(1))
    {
        const WebWgslCookRequest& request = requests[index];
        std::string error;
        std::error_code ec;
        if (CookWebWgslIntoPackage(request, error))
            fs::remove_all(request.ScratchDir, ec);
        else
            errors[index] = error.empty() ? std::string("WGSL cook failed") : std::move(error);
    }
}
} // namespace

std::vector<std::string> CookWebWgslBatch(const std::vector<WebWgslCookRequest>& requests, size_t jobs)
{
    std::vector<std::string> errors(requests.size());
    std::atomic<size_t> next{0};
    const size_t workerCount = std::min(std::max<size_t>(jobs, 1), std::max<size_t>(requests.size(), 1));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i)
        workers.emplace_back(CookBatchWorker, std::cref(requests), std::ref(next), std::ref(errors));
    for (std::thread& worker : workers)
        worker.join();
    return errors;
}

} // namespace GameEngine::Tools
