# How to use Paths + Settings APIs

This is the practical “how do I use it” guide for the standardized APIs introduced for:
- **Runtime apps/games** (Engine)
- **Editor global** (per-user, shared across all projects)
- **Project shared** (stored in the project, committable)
- **Project per-user** (stored outside the project, keyed by workspace)

## Runtime (Engine) paths

Header: `Engine/Include/Core/StandardPaths.h`

### Get standard OS directories

```cpp
#include "Core/StandardPaths.h"

std::filesystem::path userData = GameEngine::StandardPaths::UserDataRoot("My Game");
std::filesystem::path cache    = GameEngine::StandardPaths::UserCacheRoot("My Game");
std::filesystem::path logs     = GameEngine::StandardPaths::UserLogsRoot("My Game");
std::filesystem::path saves    = GameEngine::StandardPaths::GameSavesRoot("My Game");
```

### Platform behavior (current implementation)
- **Windows**
  - `UserDataRoot(app)` → `%APPDATA%\<App>`
  - `UserCacheRoot(app)` → `%LOCALAPPDATA%\<App>`
  - `UserLogsRoot(app)` → `%APPDATA%\<App>\Logs`
  - `GameSavesRoot(app)` → `<Documents>\My Games\<App>` (Documents resolved via Known Folder API)
- **macOS**
  - `UserDataRoot(app)` → `~/Library/Application Support/<App>`
  - `UserCacheRoot(app)` → `~/Library/Caches/<App>`
  - `UserLogsRoot(app)` → `~/Library/Application Support/<App>/Logs`
  - `GameSavesRoot(app)` → `~/Library/Application Support/<App>/Saves`
- **Linux**
  - `UserDataRoot(app)` → `$XDG_DATA_HOME/<App>` (fallback `~/.local/share/<App>`)
  - `UserCacheRoot(app)` → `$XDG_CACHE_HOME/<App>` (fallback `~/.cache/<App>`)
  - `UserLogsRoot(app)` → `<UserDataRoot>/Logs`
  - `GameSavesRoot(app)` → `<UserDataRoot>/saves`

Notes:
- `app` is sanitized via `PathUtils::SanitizeForFolderName()` to keep folder names filesystem-safe across platforms.

## Engine-resolved workspace/project roots

Header: `Engine/Include/Core/Engine.h`

The engine resolves these during `EngineCore::Initialize()` and makes them available:

```cpp
#include "Core/Engine.h"

auto& engine = GameEngine::EngineCore::GetInstance();
std::filesystem::path workspaceRoot = engine.GetWorkspaceRoot();
std::filesystem::path assetRoot     = engine.GetResolvedAssetRoot();
std::filesystem::path dbFile        = engine.GetAuthoritativeAssetDbFile();
std::filesystem::path dbCacheRoot   = engine.GetAssetDbCacheRoot();
```

Use these instead of re-deriving project root from `assetRoot.parent_path()` in Editor/tools.

## Editor paths (global vs project)

Header: `Apps/Editor/Include/Editor/EditorPaths.h`

### Editor-global paths (per-user)

```cpp
#include "Editor/EditorPaths.h"

GameEngine::Editor::EditorGlobalPaths g = GameEngine::Editor::GetEditorGlobalPaths();

// Examples:
// g.userDataRoot        -> <UserData>/GameEngine/Editor
// g.userCacheRoot       -> <UserCache>/GameEngine   (historical layout preserved)
// g.preferencesFile     -> <UserData>/GameEngine/Editor/Preferences.json
// g.userAssetsRoot      -> <UserData>/GameEngine/Editor/EditorAssets
// g.installAssetsRoot   -> <exe>/Assets (Win/Linux) or <bundle>/Resources/Assets (macOS)
```

### Project/workspace paths

```cpp
#include "Editor/EditorPaths.h"

GameEngine::Editor::EditorProjectPaths p = GameEngine::Editor::GetCurrentEditorProjectPaths();

// Examples:
// p.projectRoot          -> resolved workspace root
// p.projectCacheRoot     -> <ProjectRoot>/.Cache
// p.projectEditorRoot    -> <ProjectRoot>/.Editor
// p.thumbnailsRoot       -> <ProjectRoot>/.Editor/Thumbnails
// p.projectSettingsFile  -> <ProjectRoot>/.Editor/ProjectSettings.json
// p.userProjectSettingsFile -> <EditorUserDataRoot>/Projects/<WorkspaceId>/UserSettings.json
```

## Editor settings (preferences + project settings)

Headers:
- `Apps/Editor/Include/Editor/Settings/SettingsStore.h`

### Editor preferences (global)

```cpp
#include "Editor/Settings/SettingsStore.h"

auto prefs = GameEngine::Editor::OpenEditorPreferences();
std::string err;
prefs.Load(&err); // missing file is OK; you still get an empty store

prefs.SetBool("ui.enableVsync", true);
prefs.SetDouble("ui.textAaScale", 1.25);

prefs.Save(&err);
```

### Project shared settings (stored in the project)

```cpp
#include "Editor/Settings/SettingsStore.h"
#include "Core/Engine.h"

auto& engine = GameEngine::EngineCore::GetInstance();
auto settings = GameEngine::Editor::OpenProjectSettings(engine.GetWorkspaceRoot());

settings.Load();
settings.SetString("render.pipeline", "ForwardPBR");
settings.Save();
```

### Project per-user settings (stored outside the project)

```cpp
#include "Editor/Settings/SettingsStore.h"
#include "Core/Engine.h"

auto& engine = GameEngine::EngineCore::GetInstance();
auto userSettings = GameEngine::Editor::OpenUserProjectSettings(engine.GetWorkspaceRoot());

userSettings.Load();
userSettings.SetString("ui.layoutPreset", "MyLayout");
userSettings.Save();
```

### Atomicity / safety
`SettingsStore::Save()` writes to a temp file next to the destination and then renames into place (best-effort atomic replace). This reduces corruption risk on crash/kill during saves.

## OS Documents path helper (when you need “Documents”)

Header: `Engine/Include/Core/Application.h` (`PathUtils`)

```cpp
#include "Core/Application.h"

std::filesystem::path docs = GameEngine::PathUtils::GetUserDocumentsDirectory();
```

Platform behavior (current implementation):
- **Windows**: Known Folder Documents (fallback `%USERPROFILE%\\Documents`)
- **macOS**: `~/Documents`
- **Linux**: reads `XDG_DOCUMENTS_DIR` from `user-dirs.dirs` when available (fallback `~/Documents`)

