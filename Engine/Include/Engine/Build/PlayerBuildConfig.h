#pragma once

#include <string_view>

namespace GameEngine {

/// One configuration the packaged Player can be built in, with the
/// properties the build pipeline branches on.
///
/// The Player compiles from a generated CMake project
/// (Apps/Player/GeneratedCMakeLists.txt.in) that declares no configurations of
/// its own, so on a multi-config generator its set is CMake's default four. The
/// engine's `DebugFast` is appended to CMAKE_CONFIGURATION_TYPES by the repo
/// root CMakeLists and exists only in the engine's own build tree: asking the
/// Player project for it fails the build with MSB8013, so it is not a Player
/// configuration and never belongs in a build request.
///
/// This table is the single authority. The settings UI, the build request, the
/// CRT-flavour check and the NativeAOT gate all read it, so a configuration
/// cannot mean one thing where it is shown and another where it is built.
///
/// CRT flavour is the only property of the Player's configuration that has to
/// match the engine's. Whether engine classes carry their debug-only members
/// is GE_DEBUG_INSTRUMENTATION, which the SDK's generated find_package config
/// republishes from the staged engine libs rather than deriving from the
/// Player's configuration, so a Release Player links a DebugFast engine with
/// the same layouts.
struct PlayerBuildConfigInfo
{
    std::string_view Name;
    /// Links the MSVC debug CRT (/MDd, _ITERATOR_DEBUG_LEVEL=2). A player built
    /// against the other flavour from the runtime DLLs beside it links cleanly
    /// — LNK2038 never crosses a DLL import boundary — and then dies at static
    /// init on the first cross-module STL access.
    bool DebugCrt;
    /// Compiled with optimizations: the shipping-artifact shape. Gates the
    /// NativeAOT script publish and the package-source routing that goes with
    /// it, so a "ship with symbols" configuration produces the same artifact
    /// shape as a plain release one.
    bool ShipOptimized;
    /// Ships separate debug symbols when the requested configuration includes them.
    bool DebugSymbols;
};

inline constexpr PlayerBuildConfigInfo kPlayerBuildConfigs[] = {
    {"Debug", true, false, true},
    {"Release", false, true, false},
    {"RelWithDebInfo", false, true, true},
    {"MinSizeRel", false, true, false},
};

/// The table entry for `config`, or nullptr when the Player project cannot
/// build a configuration by that name.
constexpr const PlayerBuildConfigInfo* FindPlayerBuildConfig(std::string_view config)
{
    for (const PlayerBuildConfigInfo& info : kPlayerBuildConfigs)
    {
        if (info.Name == config)
            return &info;
    }
    return nullptr;
}

constexpr bool IsSupportedPlayerBuildConfig(std::string_view config)
{
    return FindPlayerBuildConfig(config) != nullptr;
}

/// Only "Debug" links the debug CRT. An unsupported name answers false: it
/// never reaches a compiler, and the build refuses it before the flavour of its
/// runtime DLLs can matter.
constexpr bool PlayerBuildConfigUsesDebugCrt(std::string_view config)
{
    const PlayerBuildConfigInfo* info = FindPlayerBuildConfig(config);
    return info != nullptr && info->DebugCrt;
}

/// Whether this configuration produces a shipping artifact — the question the
/// NativeAOT gate asks. Keyed on the property, not on the name "Release", so a
/// RelWithDebInfo or MinSizeRel package ships the same AOT scripts a Release
/// one does instead of silently falling back to loose managed assemblies.
constexpr bool IsShipOptimizedPlayerBuildConfig(std::string_view config)
{
    const PlayerBuildConfigInfo* info = FindPlayerBuildConfig(config);
    return info != nullptr && info->ShipOptimized;
}

/// Whether the requested Player configuration ships separate debug symbols.
constexpr bool PlayerBuildConfigIncludesDebugSymbols(std::string_view config)
{
    const PlayerBuildConfigInfo* info = FindPlayerBuildConfig(config);
    return info != nullptr && info->DebugSymbols;
}

/// Whether this translation unit links the MSVC debug CRT.
///
/// The preprocessor cannot name a build configuration — DebugFast uses the
/// release CRT, so MSVC auto-defines neither `_DEBUG` nor `NDEBUG`, and no
/// `/DNDEBUG` is added anywhere in the build — but it always answers which CRT
/// is linked, and that is the property a Player configuration has to match.
#if defined(_DEBUG)
inline constexpr bool kHostUsesDebugCrt = true;
#else
inline constexpr bool kHostUsesDebugCrt = false;
#endif

/// The Player configuration to build when the project has not chosen one: the
/// most ship-optimized configuration sharing this editor's CRT flavour, so the
/// default is always a configuration this editor can actually package. Read by
/// both the settings UI and the build request — the configuration shown is the
/// configuration built.
constexpr std::string_view DefaultPlayerBuildConfig()
{
    return kHostUsesDebugCrt ? std::string_view("Debug") : std::string_view("Release");
}

} // namespace GameEngine
