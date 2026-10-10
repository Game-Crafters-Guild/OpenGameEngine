#pragma once

#include <filesystem>

namespace GameEngine {

/**
 * @brief Configuration for scripting paths and behavior
 *
 * All paths should be absolute and stable for the lifetime of the ScriptManager.
 * This struct is intentionally in its own header so lightweight consumers
 * (e.g., the Player application) can set scripting config without pulling in
 * the full ScriptManager dependency chain (CoreCLR, FileWatcher, JobSystem, etc.).
 */
struct ScriptsConfig
{
    std::filesystem::path workspaceRoot;        // Workspace/project root
    std::filesystem::path scriptsRoot;          // Where C# sources live (e.g. Assets or Scripts)
    std::filesystem::path assembliesRoot;       // Where compiled script assemblies are written
    std::filesystem::path generatedProjectRoot; // Where auto-generated .csproj is placed; empty = workspaceRoot

    // Packaged-game mode: absolute path to the STAGED script assembly
    // (game.config scriptAssemblyPath resolved against the game content root).
    // When set, ScriptManager loads exactly this assembly and NEVER attempts a
    // runtime compile — no project generation, no dotnet probe, no build. A
    // shipped game has no sources and no toolchain; compiling there is a bug.
    std::filesystem::path prebuiltAssemblyPath;

    // POSITIVE packaged-game signal, detected from the shipped layout
    // (game.config + staged Assets/.assetmanifest — see PackagedGameLayout)
    // before engine init. Hard-disables every ScriptManager compile and
    // project-generation path even when prebuiltAssemblyPath is empty (e.g. an
    // incomplete package whose staged assembly is missing must error, never
    // fall back to the dev compile pipeline).
    bool packagedMode = false;

    bool disableClr = false;                 // Skip CoreCLR initialization entirely
    bool enableHotReload = true;             // Enable file watching + hot-reload
    bool enableAsyncHotReload = false;       // Use async hot-reload pipeline (Editor sets true)
    bool enableAutoProjectGeneration = true; // Generate ephemeral project when none found
    // When true, ScriptManager::Initialize will not block application startup waiting for
    // scripts/editor assemblies to compile/load. Instead it will continue startup and
    // attempt compilation/loading in the background.
    bool deferInitialLoad = false;
};

} // namespace GameEngine
