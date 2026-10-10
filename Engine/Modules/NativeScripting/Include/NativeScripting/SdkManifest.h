#pragma once

// SdkManifest — load the staged NativeScripting SDK manifest into a NativeBuildConfig.
//
// The editor builds hot-reloadable user-script DLLs from an SDK staged next to the
// running executable (NOT the engine source tree — we ship the exe). GameEngineSDK.cmake
// generates <exe>/SDK/nativescripting/manifest.txt with the engine build interface
// (include dir, Engine import lib, compile defs, SDK entry source, ComponentScanner) as
// paths RELATIVE to the SDK root, plus dev-tool absolute paths for cmake + dotnet.
//
// This fills only the engine-interface fields of NativeBuildConfig. The caller sets the
// project-specific SourceDir / BuildDir / ActiveDir.

#include "NativeScripting/NativeBuildConfig.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// Load <sdkRoot>/nativescripting/manifest.txt into outConfig (engine-interface fields:
// CMakeExe, Config, IncludeDirs, EngineImportLib, CompileDefinitions, SdkEntrySource,
// DotnetExe, ComponentScannerDll, DetectBase; plus EditorImportLib/EditorIncludeDirs
// when the editor's SDK staged them). Relative manifest paths are resolved
// against sdkRoot. The user DLL's build config is mapped from the engine's config to a
// CRT-compatible standalone config (engine Debug → Debug /MDd; anything else → Release
// /MD — DebugFast uses the release CRT). Returns false with outError set when the
// manifest is missing or the essential build inputs (cmake / import lib / include dir)
// are absent. A missing dotnet/scanner is tolerated (the build falls back to macros-only).
bool LoadSdkManifest(const std::filesystem::path& sdkRoot,
                     NativeBuildConfig& outConfig,
                     std::string& outError);

// Resolve the dotnet executable for THIS machine: `recordedPath` (the SDK
// manifest's dev-machine absolute path) when it exists, else $DOTNET_ROOT,
// else $PATH, else the platform's standard install locations. Empty when
// nothing resolves — callers own the "is dotnet required here" decision.
// This is the single dotnet discovery the engine shares (SDK manifest
// loading, packaged C# compile preflight, and — via DotnetHostCommand() —
// every compile-server / script-build child process).
std::filesystem::path ResolveDotnetExecutable(const std::string& recordedPath);

} // namespace NativeScripting
} // namespace GameEngine
