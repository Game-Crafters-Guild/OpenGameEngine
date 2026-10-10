# Paths and Settings (Standardized APIs)

This doc describes the **standard path conventions** and **settings persistence** APIs for:
- **Runtime apps/games** (Engine-level)
- **Editor (global)** data (shared across all projects for a user)
- **Project (shared)** settings (stored in the project)
- **Project (per-user)** settings (stored outside the project, keyed by workspace)

## Scopes and what belongs where

### Runtime (Engine)
- **Use when**: game/app code needs OS-correct user directories (saves, user data, caches).
- **API**: `GameEngine::StandardPaths` (paths-only).

### Editor global (Editor, per-user)
- **Use when**: editor UI preferences, editor-wide caches, editor-provided assets.
- **API**: `GameEngine::Editor::GetEditorGlobalPaths()`.
- **Convention**:
  - `<UserData>/GameEngine/Editor` for editor data (preferences, editor assets, etc.)
  - `<UserCache>/GameEngine` for editor caches (**historical layout preserved**)

### Project shared (Editor, per-project, committable)
- **Use when**: project settings intended to be shared with the team.
- **Convention**: `<ProjectRoot>/.Editor/ProjectSettings.json`

### Project per-user (Editor, per-project, per-user)
- **Use when**: settings that vary per user/machine (layout, local paths, etc.)
- **Convention**: `<EditorUserDataRoot>/Projects/<WorkspaceId>/UserSettings.json`

## Engine APIs

### OS-correct roots
Files:
- `Engine/Include/Core/StandardPaths.h`
- `Engine/Source/Core/StandardPaths.cpp`

Key functions:
- `StandardPaths::UserDataRoot(appName)`
- `StandardPaths::UserCacheRoot(appName)`
- `StandardPaths::UserLogsRoot(appName)`
- `StandardPaths::GameSavesRoot(appName)`

Notes:
- `appName` is sanitized via `PathUtils::SanitizeForFolderName()` to remain filesystem-safe.
- Windows save-games use the common convention:
  - `<Documents>/My Games/<AppName>`

### Workspace/project roots
`EngineCore` exposes resolved paths computed during `EngineCore::Initialize()`:
- `EngineCore::GetWorkspaceRoot()`
- `EngineCore::GetResolvedAssetRoot()`
- `EngineCore::GetAuthoritativeAssetDbFile()`
- `EngineCore::GetAssetDbCacheRoot()`

This avoids re-deriving project root from `assetRoot.parent_path()` at call sites.

## Editor APIs

Files:
- `Apps/Editor/Include/Editor/EditorPaths.h`
- `Apps/Editor/Source/EditorPaths.cpp`

### Editor global paths
`EditorGlobalPaths` includes:
- `userDataRoot`, `userCacheRoot`, `userLogsRoot`
- `installAssetsRoot`, `userAssetsRoot`
- `preferencesFile`, `projectsRoot`, `defaultProjectRoot`

### Project paths
`EditorProjectPaths` includes:
- `projectRoot` / `workspaceRoot`
- `projectCacheRoot` (`<ProjectRoot>/.Cache`)
- `projectEditorRoot` (`<ProjectRoot>/.Editor`)
- `thumbnailsRoot` (`<ProjectRoot>/.Editor/Thumbnails`)
- `projectSettingsFile` (`<ProjectRoot>/.Editor/ProjectSettings.json`)
- `userProjectSettingsFile` (`<EditorUserDataRoot>/Projects/<WorkspaceId>/UserSettings.json`)

## Settings persistence (Editor)

Files:
- `Apps/Editor/Include/Editor/Settings/SettingsStore.h`
- `Apps/Editor/Source/SettingsStore.cpp`

`SettingsStore` is a small JSON store that:
- Loads existing settings files (or initializes empty settings when missing)
- Saves **atomically** (temp file + rename) to reduce corruption risk
- Ensures a `schemaVersion` field exists

Convenience constructors:
- `OpenEditorPreferences()` → `<EditorUserDataRoot>/Preferences.json`
- `OpenProjectSettings(workspaceRoot)` → `<ProjectRoot>/.Editor/ProjectSettings.json`
- `OpenUserProjectSettings(workspaceRoot)` → `<EditorUserDataRoot>/Projects/<WorkspaceId>/UserSettings.json`

## Current on-disk conventions preserved
- **Editor cache root**: `<UserCache>/GameEngine` (kept for backward compatibility)
- **Project cache**: `<ProjectRoot>/.Cache/...`
- **Project editor root**: `<ProjectRoot>/.Editor/...`

## Player writable files

The Player never writes inside its install directory or app bundle, so a signed
macOS bundle stays valid after the game runs. Its writable files sit in per-user
directories named after the game: the `gameName` in `game.config`, or `My Game`
when the configuration is missing, unreadable or names none.

**Log.** Each run writes `game-<process id>.log` under
`StandardPaths::UserLogsRoot(gameName)`, so two copies of a game running at once
keep separate logs:

| Platform | Log directory |
|---|---|
| Windows | `%APPDATA%/<GameName>/Logs` |
| macOS | `~/Library/Application Support/<GameName>/Logs` |
| Linux | `$XDG_DATA_HOME/<GameName>/Logs` (`~/.local/share/<GameName>/Logs` when `XDG_DATA_HOME` is unset) |

The log records its own path at startup. Problems found while reading
`game.config` are in the log too, even though the file opens only after the
configuration is read.

**Texture bakes in a packaged game.** The export bakes textures into the game's
content (`Tex/` beside the game's assets, and `Packages/<alias>/Tex/` for each
package), and the packaged game reads them from there. It never cooks a texture
and writes no asset cache of its own.

**Derived data where the engine cooks.** The Editor and a Player run from a
development build cook textures and keep derived asset databases under the
host's asset cache root: `<ProjectRoot>/.Cache/AssetDatabase` for the Editor and
`<UserDataRoot>/.Cache/AssetDatabase` for a development Player
(`StandardPaths::UserDataRoot(gameName)`). A git or engine package that publishes
an `.assetmanifest` gets `<asset cache root>/Packages/<alias>/AssetDatabase`, with
its cooked textures in `<asset cache root>/Packages/<alias>/Tex`. A package
without an `.assetmanifest` keeps its asset metadata in memory and scans its
files each time it is mounted. Embedded and `file:` packages keep their derived
data in their own `.Cache`.

Derived data an older build left inside an entry of the git package cache is no
longer read; it is rebuilt in the locations above on first use. Delete it by
hand to reclaim the space.
