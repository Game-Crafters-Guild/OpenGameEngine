#pragma once

// P3 prebuilt native package binaries — fingerprint-keyed lookup of a shipped
// module DLL so consumers of a native package never need the author's
// toolchain. Layout inside a package (manifest `prebuilt` dir, conventionally
// "Binaries/"):
//
//   <prebuilt>/<platform>-<fingerprint8>/<Module>.dll     (.dylib on macOS, .so on Linux)
//   <prebuilt>/<platform>-<fingerprint8>/engine_abi.txt   (optional marker)
//
// The module file name is PrebuiltModuleFileName on every platform: no "lib"
// prefix, the host's module extension. A toolchain that names its outputs
// otherwise (CMake gives a MODULE library "lib<Name>.so" on macOS and Linux)
// is renamed to it when staged, never probed under a second name.
//
// <platform> is e.g. "windows-x64"; <fingerprint8> is
// ToolchainFingerprintShortHash of the GE_ToolchainFingerprint the DLL was
// built with — compiler + CRT + STL identity, so Debug and Release CRT
// binaries land in different dirs by construction. The optional engine_abi
// marker pins the exact engine the DLL was compiled against; when present it
// must match the host's ABI digest for the module. Absent a marker, the DLL's
// own exported fingerprint handshake (LoadModule) remains the final gate.

#include "NativeScripting/NativeScriptingABI.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// Stable 8-hex-char hash over the compared fingerprint fields (padding
// excluded). Two binaries share a prebuilt dir iff their toolchains are
// load-compatible under the LoadModule handshake.
std::string ToolchainFingerprintShortHash(const GE_ToolchainFingerprint& fingerprint);

// "<platform>-<arch>-<fingerprint8>", e.g. "windows-x64-1a2b3c4d".
std::string PrebuiltPlatformDirName(const GE_ToolchainFingerprint& fingerprint);

// "<moduleName><host module extension>", e.g. "Eztree.dll", "Eztree.dylib",
// "Eztree.so": the file name FindPrebuiltModule probes, and the name the engine
// build stages a prebuilt under (GePrebuiltStamp modulefile).
std::string PrebuiltModuleFileName(const std::string& moduleName);

struct PrebuiltModuleLookup
{
    std::filesystem::path Dll;         // empty when unusable — see Reason
    std::filesystem::path PlatformDir; // the dir that was probed
    std::string Reason;                // why the prebuilt is unusable (for the loud fallback log)
};

// Pure filesystem decision: probe <prebuiltRoot>/<platform-dir>/ for the
// host-fingerprint-matching module DLL and verify the optional engine_abi
// marker against `hostEngineAbiDigest`. Never loads anything.
PrebuiltModuleLookup FindPrebuiltModule(const std::filesystem::path& prebuiltRoot,
                                        const GE_ToolchainFingerprint& hostFingerprint,
                                        const std::string& moduleName,
                                        const std::string& hostEngineAbiDigest);

} // namespace NativeScripting
} // namespace GameEngine
