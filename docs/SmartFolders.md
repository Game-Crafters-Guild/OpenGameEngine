# Smart Folders

Smart Folders are virtual folders that automatically display assets matching specific filter criteria. They provide a powerful way to organize and quickly access files across your project without moving them from their original locations.

## Creating a Smart Folder

1. Right-click on the **Smart Folders** section in the Assets panel tree view
2. Select **New Smart Folder** from the context menu
3. A new smart folder will appear with the default name "New Smart Folder"
4. Click on the smart folder to select it and configure its settings in the Inspector panel

## Smart Folder Settings

When a smart folder is selected, the Inspector panel displays the following settings:

### Name
The display name of the smart folder. This appears in the Assets panel tree view.

### Location
The root directory to search for matching files:
- **(All Assets)**: Searches all files in the project's Assets folder
- **Specific folder**: Limits the search to a specific directory and its subdirectories (up to 3 levels deep)

### Match
How multiple filters are combined:
- **Any filter (OR)**: Files matching *any* of the filters will be included
- **All filters (AND)**: Only files matching *all* filters will be included

### Global
Controls where the smart folder is stored:
- **Off**: Stored per-project. Only appears when this project is open.
- **On**: Stored globally. Appears across all projects.

## Filters

Filters define which files appear in the smart folder.

### Adding Filters
Click the **Add Filter** button to add a new filter row.

### Filter Types

| Filter Type | Description | Example Values |
|------------|-------------|----------------|
| **File Type** | Match files by extension | `.png`, `.jpg`, `.cs`, `.mp3` |
| **Name Contains** | Match files whose name contains text | `player`, `texture`, `audio` |
| **Regex** | Match files using a regular expression | `^UI_.*`, `.*_diffuse$`, `player\d+` |

### Removing Filters
Click the **×** button on the right side of a filter row to remove it.

## Using Smart Folders

- **View Contents**: Click on a smart folder in the tree view to display its matching files in the grid/list view
- **Open Files**: Double-click files in the smart folder view to open them
- **Drag and Drop**: Files can be dragged from smart folders to other panels

## Undo Support

Smart folder operations support undo/redo:
- **Create**: Undo removes the created smart folder
- **Delete**: Undo restores the deleted smart folder with all its settings
- **Edit**: Undo reverts changes to smart folder settings

Use **Cmd+Z** (macOS) or **Ctrl+Z** (Windows/Linux) to undo.

## Examples

### All Textures
- **Match**: Any filter (OR)
- **Filters**:
  - File Type: `.png`
  - File Type: `.jpg`
  - File Type: `.tga`

### All Audio Files
- **Match**: Any filter (OR)
- **Filters**:
  - File Type: `.mp3`
  - File Type: `.wav`
  - File Type: `.ogg`

### Player Scripts
- **Match**: All filters (AND)
- **Filters**:
  - File Type: `.cs`
  - Name Contains: `Player`

### UI Textures
- **Match**: All filters (AND)
- **Location**: `Assets/Textures`
- **Filters**:
  - Regex: `^UI_.*`

## Settings

In **Edit > Preferences > Assets**, you can configure:
- **Smart Folders At Top**: When enabled, smart folders appear at the top of the Assets tree. When disabled, they appear at the bottom.

## Storage Locations

- **Project smart folders**: Stored in `<ProjectRoot>/.Editor/SmartFolders.json`
- **Global smart folders**: Stored in `~/Library/Application Support/GameEngine/Editor/SmartFolders.json` (macOS) or `%APPDATA%/GameEngine/Editor/SmartFolders.json` (Windows)

## Technical Notes

Smart folders use the **AssetRegistry** for file lookup, which provides:
- **Faster performance**: Uses pre-indexed asset data instead of filesystem scanning
- **Consistent results**: Respects `.assetignore` rules and only shows registered assets
- **Accurate metadata**: Access to asset type, dependencies, and other registry information

If the AssetRegistry is unavailable, smart folders fall back to direct filesystem scanning.
