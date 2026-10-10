#pragma once

#include <string_view>

namespace GameEngine {

/// Stem of the AOT project BuildPipeline generates for a ship-optimized build
/// (<name>.csproj). MSBuild names the assembly, and with it the published
/// native library, after the project file.
inline constexpr std::string_view kNativeAotProjectName = "GameEngine.Scripts.AOT";

/// The library `dotnet publish -r <rid> /p:NativeLib=Shared` writes into the
/// publish directory for that project. ILCompiler names it
/// $(TargetName)$(NativeBinaryExt): there is no "lib" prefix on any platform.
#if defined(_WIN32)
inline constexpr std::string_view kNativeAotPublishedLibraryName = "GameEngine.Scripts.AOT.dll";
#elif defined(__APPLE__)
inline constexpr std::string_view kNativeAotPublishedLibraryName = "GameEngine.Scripts.AOT.dylib";
#else
inline constexpr std::string_view kNativeAotPublishedLibraryName = "GameEngine.Scripts.AOT.so";
#endif

/// The .NET runtime identifier of this host, the only target the NativeAOT
/// publish builds for (ILCompiler does not cross-compile between OSes).
#if defined(_WIN32)
#  if defined(_M_ARM64)
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "win-arm64";
#  else
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "win-x64";
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "osx-arm64";
#  else
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "osx-x64";
#  endif
#else
#  if defined(__aarch64__)
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "linux-arm64";
#  else
inline constexpr std::string_view kNativeAotRuntimeIdentifier = "linux-x64";
#  endif
#endif

} // namespace GameEngine
