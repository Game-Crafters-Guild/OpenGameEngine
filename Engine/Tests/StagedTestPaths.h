#pragma once

// Staged test-fixture resolution. Test fixtures are staged into the build root
// by the StageTestAssets target (Tests/CMakeLists.txt), mirroring their
// repo-relative paths (Assets/..., Engine/Tests/Fixtures/...,
// Engine/Modules/Rendering/Shaders/...). Tests resolve fixtures against
// StagedRoot() — derived from the executable's location — and must never climb
// from the CWD or __FILE__ into the source tree: that works on a dev machine
// and breaks the moment the build output is relocated.
//
// One rule for where a test reads from:
//
//   DATA reads go staged. Anything a test opens because the engine itself would
//   open it at runtime — assets, scenes, shader packages, the adapter templates
//   a MaterialBuildContext consumes — resolves exe-anchored through StagedRoot(),
//   staged by StageTestAssets (Tests/CMakeLists.txt) or ge_stage_test_asset
//   (cmake/OutputLayout.cmake). Reading the repo instead lets a suite pass on a
//   build output that does not actually carry the content.
//
//   SOURCE-TEXT reads go to the build input. A test whose subject is the
//   authored text a developer edits — a shader-source or source-text contract,
//   asserting that a .glsl, .cpp or .css says a particular thing — reads the
//   source tree, and only through a compile definition whose name says it is a
//   source root: GE_RENDERER_REPO_ROOT, GE_REPO_SOURCE_DIR, GE_EDITOR_SOURCE_DIR,
//   RENDERING_SOURCE_DIR, CBT_SHADER_SOURCE_DIR, GE_VULKAN_BACKEND_SOURCE_DIR,
//   and their siblings. A mirror would put a build step between the edited file
//   and the assertion, and it only covers roots somebody remembered to stage.
//   No runtime code path may use such a definition — they exist for tests.
//
//   __FILE__ is never an anchor, for either kind. Under ccache (base_dir) clang
//   emits it relative to the build directory, so a path climbed from it follows
//   the process CWD: right when the CWD happens to be the build root, silently
//   empty everywhere else.
//
// Header-only and dependency-free on purpose: module test exes (e.g. the Graph
// module's) do not link Engine, so this cannot use PathUtils.

#include <filesystem>
#include <iterator>
#include <string>

#if defined(_WIN32)
// Deliberately NOT <windows.h>: it injects a global GUID type that collides
// with GameEngine::GUID in test files that use the engine namespace. Declare
// the one import this header needs instead, with exactly the SDK's types
// (HMODULE is HINSTANCE__* under STRICT, the SDK default; DWORD is unsigned
// long), so a translation unit that also includes <windows.h> sees a
// redeclaration rather than a conflicting extern "C" overload. Defining
// NO_STRICT changes HMODULE to void* and turns this back into C2733.
struct HINSTANCE__;
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(
    HINSTANCE__* module, wchar_t* filename, unsigned long size);
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace GameEngine::TestPaths {

inline std::filesystem::path ExecutableDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[4096];
    const unsigned long len =
        GetModuleFileNameW(nullptr, buffer, static_cast<unsigned long>(std::size(buffer)));
    if (len == 0 || len >= std::size(buffer))
        return {};
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(buffer.data(), ec);
    return (ec ? std::filesystem::path(buffer.data()) : canonical).parent_path();
#else
    char buffer[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0)
        return {};
    buffer[len] = '\0';
    return std::filesystem::path(buffer).parent_path();
#endif
}

// Build root containing the staged fixture mirror. Test executables live at
// <build>/bin/<Config>/Tests (canonical) or <build>/bin/<Config>.
inline std::filesystem::path StagedRoot()
{
    std::filesystem::path dir = ExecutableDirectory();
    if (dir.empty())
        return {};
    if (dir.filename() == "Tests")
        dir = dir.parent_path(); // <build>/bin/<Config>
    return dir.parent_path().parent_path(); // <build>/bin -> <build>
}

// Adapter / surface GLSL templates, staged from Engine/Modules/Rendering/Shaders.
// This is the directory a MaterialBuildContext::AdapterShaderDir wants.
inline std::filesystem::path StagedRenderingShadersDir()
{
    return StagedRoot() / "Engine" / "Modules" / "Rendering" / "Shaders";
}

// Shipped engine content (Materials/, RenderPipelines/, ...), staged from repo Assets/.
inline std::filesystem::path StagedEngineAssetsDir()
{
    return StagedRoot() / "Assets";
}

} // namespace GameEngine::TestPaths
