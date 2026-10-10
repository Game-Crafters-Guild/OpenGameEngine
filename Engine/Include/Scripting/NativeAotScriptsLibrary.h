#pragma once

#include <string_view>

namespace GameEngine {

/// File name of the NativeAOT-compiled C# gameplay library in a packaged game.
///
/// The build pipeline stages it next to the Player executable (Contents/MacOS
/// inside a mac bundle). Its presence there is what makes the Player skip
/// CoreCLR, and ManagedSystemBridge loads it by this name.
#if defined(_WIN32)
inline constexpr std::string_view kNativeAotScriptsLibraryName = "GameEngine.Scripts.native.dll";
#elif defined(__APPLE__)
inline constexpr std::string_view kNativeAotScriptsLibraryName = "GameEngine.Scripts.native.dylib";
#else
inline constexpr std::string_view kNativeAotScriptsLibraryName = "GameEngine.Scripts.native.so";
#endif

} // namespace GameEngine
