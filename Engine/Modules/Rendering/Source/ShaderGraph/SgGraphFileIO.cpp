#include "Rendering/ShaderGraph/SgGraphFileIO.h"

#include "Rendering/ShaderGraph/SgGraphCompiler.h"
#include "Rendering/ShaderGraph/SgNodeReflector.h"
#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>
#include <mutex>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace GameEngine::ShaderGraph
{
namespace
{

std::mutex g_SharedLibraryMutex;
SgNodeLibraryIndex g_SharedLibrary;
std::filesystem::path g_SharedLibraryProjectRoot;
bool g_SharedLibraryValid = false;
std::filesystem::path g_RegisteredEngineGraphNodesRoot;

} // namespace

const SgNodeLibraryIndex& GetSharedNodeLibraryIndex(const std::filesystem::path& projectNodesRoot)
{
    std::lock_guard<std::mutex> lock(g_SharedLibraryMutex);
    if (g_SharedLibraryValid && g_SharedLibraryProjectRoot == projectNodesRoot)
        return g_SharedLibrary;

    g_SharedLibrary = SgNodeReflector::BuildIndex(GetEngineGraphNodesRoot(), projectNodesRoot);
    g_SharedLibraryProjectRoot = projectNodesRoot;
    g_SharedLibraryValid = true;
    return g_SharedLibrary;
}

void InvalidateSharedNodeLibraryIndex()
{
    std::lock_guard<std::mutex> lock(g_SharedLibraryMutex);
    g_SharedLibraryValid = false;
    g_SharedLibrary.NodesByType.clear();
    g_SharedLibrary.IncludePaths.clear();
    g_SharedLibraryProjectRoot.clear();
}

SgGraphFile LoadGraphFile(const std::filesystem::path& path)
{
    SgGraphFile file;
    const SgParsedFile parsed = ParseShaderGraphFile(path);
    file.TagBlock = parsed.TagBlock;
    file.Body = parsed.Body;
    file.Document = ParseGraphDocumentFromTags(parsed.TagBlock);
    return file;
}

std::string SerializeGraphFileText(const SgGraphDocument& doc, const std::string& body)
{
    std::string serialized = SerializeGraphDocumentTags(doc);
    if (!body.empty() && body[0] != '\n')
        serialized += '\n';
    serialized += body;
    return serialized;
}

bool WriteGraphFileTextIfChanged(const std::filesystem::path& path, const std::string& text)
{
    try
    {
        // Text-mode read matches the text-mode write below (CRLF-normalized on
        // Windows), so an unchanged file compares equal on every platform.
        std::ifstream in(path);
        if (in)
        {
            const std::string existing((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            if (existing == text)
                return true;
        }

        std::ofstream out(path);
        out << text;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SaveGraphFile(const std::filesystem::path& path, const SgGraphDocument& doc, const std::string& body)
{
    return WriteGraphFileTextIfChanged(path, SerializeGraphFileText(doc, body));
}

bool SaveCompiledGraphFile(const std::filesystem::path& path, const SgGraphDocument& doc,
                           const SgNodeLibraryIndex& library)
{
    const SgCompileResult compiled = SgGraphCompiler::Compile(doc, library);
    if (!compiled.Success)
        return false;
    return SaveGraphFile(path, doc, compiled.Body);
}

namespace
{

std::filesystem::path GetExecutableDirectoryForGraphNodes()
{
#if defined(__APPLE__)
    char exePath[PATH_MAX]{};
    uint32_t size = static_cast<uint32_t>(sizeof(exePath));
    if (_NSGetExecutablePath(exePath, &size) != 0)
        return {};
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(exePath, ec);
    return ec ? std::filesystem::path(exePath).parent_path() : canonical.parent_path();
#elif defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};
    return std::filesystem::path(buffer).parent_path();
#else
    return {};
#endif
}

// The install-assets layout rule, as PathUtils::InstallAssetsRootFor states it: inside a macOS
// app bundle (<App>.app/Contents/MacOS) the root is Contents/Resources/Assets, and <exe>/Assets
// everywhere else. This is a copy, not the owner: GameEngineRendering is linked standalone by its
// own test executables and cannot call PathUtils. A host that can registers the owner's answer
// through SetEngineGraphNodesRoot, and this copy is then never asked. ShaderGraphNodesRootTests
// pins it to the owner's answer.
std::filesystem::path InstallAssetsRootForGraphNodes(const std::filesystem::path& exeDir)
{
#if defined(__APPLE__)
    if (exeDir.filename() == "MacOS" && exeDir.parent_path().filename() == "Contents")
        return exeDir.parent_path() / "Resources" / "Assets";
#endif
    return exeDir / "Assets";
}

} // namespace

void SetEngineGraphNodesRoot(std::filesystem::path root)
{
    std::lock_guard<std::mutex> lock(g_SharedLibraryMutex);
    if (g_RegisteredEngineGraphNodesRoot == root)
        return;
    g_RegisteredEngineGraphNodesRoot = std::move(root);
    // A library indexed before the root was known is empty; rebuild on next use.
    g_SharedLibraryValid = false;
    g_SharedLibrary.NodesByType.clear();
    g_SharedLibrary.IncludePaths.clear();
    g_SharedLibraryProjectRoot.clear();
}

std::filesystem::path GetEngineGraphNodesRoot()
{
    if (!g_RegisteredEngineGraphNodesRoot.empty())
        return g_RegisteredEngineGraphNodesRoot;
    // Anchored to the executable directory, never derived from __FILE__ or the working
    // directory — a source-tree path is absent the moment the build output is shipped or
    // relocated, and a relative __FILE__ (clang through a ccache base_dir) resolves against
    // whatever directory the process happens to be in.
    const std::filesystem::path exeDir = GetExecutableDirectoryForGraphNodes();
    if (exeDir.empty())
        return {};

    const std::filesystem::path nodesRoot =
        (InstallAssetsRootForGraphNodes(exeDir) / "Shaders" / "Graph" / "Nodes").lexically_normal();
    std::error_code ec;
    return std::filesystem::exists(nodesRoot, ec) ? nodesRoot : std::filesystem::path{};
}

std::filesystem::path GetDefaultNodeLibraryCachePath(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".cache" / "shadergraph" / "nodes.json";
}

} // namespace GameEngine::ShaderGraph
