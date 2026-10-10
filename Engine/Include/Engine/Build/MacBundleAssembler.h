#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {

struct BuildSettings;

/// Assembles a macOS `.app` for a packaged game using the "prebuilt-inject"
/// strategy (Unity-style): copy the prebuilt Player.app template shipped in the
/// Editor SDK, inject the project's compiled content, rewrite branding, then
/// ad-hoc re-sign. There is no per-game player recompile — the engine binary,
/// MoltenVK and Frameworks dylibs come from the template, which is already a
/// working, relocatable bundle. Managed assemblies do not: a CoreCLR game's set is
/// staged whole by InjectRuntime.
///
/// Lifecycle, driven by BuildPipeline for the "Mac" target:
///   1. PrepareBundle()  — copy template -> <staging>/<GameName>.app, scrub stale
///      per-project content and the template's managed assemblies. The pipeline
///      then writes assets + game.config into Contents/Resources (its content
///      root; Contents/MacOS holds code only).
///   2. InjectRuntime()  — managed C# assemblies (the game's scripts and the
///      engine assemblies the Player loads), the app icon and Info.plist
///      branding. Does NOT sign. The pipeline then stages the native modules (the
///      project's and every enabled package's runtime module) into the content
///      root through the same steps as every other target.
///   3. MoveNativeModulesIntoFrameworks() — moves those modules' Mach-O images
///      into Contents/Frameworks, the bundle's code location.
///   4. Codesign()       — final mutation; ad-hoc (or a supplied identity).
class MacBundleAssembler
{
public:
    /// Copy the SDK's prebuilt Player.app template into bundlePath and remove any
    /// stale per-project content and the template's Contents/Resources/Managed so
    /// the bundle starts generic. Ensures
    /// Contents/MacOS exists. Returns false (with errors) if the template is
    /// missing or the copy fails.
    static bool PrepareBundle(const std::filesystem::path& sdkTemplateApp,
                              const std::filesystem::path& bundlePath,
                              std::vector<std::string>& errors);

    /// Inject managed C# assemblies (from scriptScratchDir, produced by
    /// CompileScripts) into Contents/Resources/Managed together with the engine
    /// assemblies the Player loads (StageManagedRuntimeAssemblies, sourced from the
    /// engine managed directory of settings.runtimeDepsPath; a missing one fails),
    /// and the app icon + Info.plist branding. game.config and assets are written
    /// into the bundle by the pipeline before this runs. Does not sign.
    static bool InjectRuntime(const BuildSettings& settings,
                              const std::filesystem::path& bundlePath,
                              const std::filesystem::path& scriptScratchDir,
                              const std::filesystem::path& icnsPath,
                              std::vector<std::string>& errors,
                              std::vector<std::string>& warnings);

    /// Move every native module staged under the bundle's content root into
    /// Contents/Frameworks, where codesign signs it with the bundle: the project's
    /// (Contents/Resources/NativeScripts) to Contents/Frameworks, each package's
    /// (Contents/Resources/Packages/<alias>/NativeScripts) to
    /// Contents/Frameworks/Packages/<alias>. Those are the roots the packaged
    /// Player loads modules from; each record is rewritten relative to its root,
    /// so the Player reaches the moved image. The image's @rpath is confined to
    /// the bundle: rpaths outside it (the build machine's SDK the editor linked
    /// against) are deleted and @executable_path/../Frameworks, where libEngine
    /// ships, is added. Returns false with errors when a recorded image is
    /// missing, would replace a file already in Contents/Frameworks, or cannot be
    /// edited.
    static bool MoveNativeModulesIntoFrameworks(const std::filesystem::path& bundlePath,
                                                std::vector<std::string>& errors);

    /// Ad-hoc (or identity) re-sign the finished bundle. Must be the LAST mutation
    /// — injection invalidates the template's signature, and Apple Silicon refuses
    /// to launch a bundle whose code was edited after signing. An empty identity
    /// signs ad-hoc ("-").
    static bool Codesign(const std::filesystem::path& bundlePath,
                         const std::string& identity,
                         std::vector<std::string>& errors);
};

} // namespace GameEngine
