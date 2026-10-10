#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "EditorContextMenu/EditorContextMenu.h"

namespace GameEngine::Editor {

// Context-menu command tokens for the six "Create code asset" entries. Shared so the
// descriptor table (which drives the menu rows) and the EditorContextMenu dispatch switch
// agree on the ids without duplicating the literals.
inline constexpr std::uint32_t kCmdCreateGameSystem = 0x3016;
inline constexpr std::uint32_t kCmdCreateEntitySystem = 0x3017;
inline constexpr std::uint32_t kCmdCreateComponent = 0x3018;
inline constexpr std::uint32_t kCmdCreateCppComponent = 0x3050;
inline constexpr std::uint32_t kCmdCreateCppGameSystem = 0x3051;
inline constexpr std::uint32_t kCmdCreateCppEntitySystem = 0x3052;

// One "Create code asset" entry (a C# or C++ component/system template). This table is the
// single source of truth driving the directory-menu rows, the command-id dispatch, and the
// file-creation handler. Add a new code-asset type by adding a row plus a MakeBody function.
struct CodeAssetDescriptor
{
    EditorContextMenu::DirectoryAction Action;
    std::uint32_t CommandId;     // matches one of the kCmdCreate* tokens above
    const char* MenuPath;        // e.g. "Create/C#/Component"
    int Priority;                // menu sort priority (smaller sorts higher)
    const char* BaseName;        // unique-stem seed, e.g. "NewComponent"
    const char* Extension;       // ".cs" / ".h"
    const char* UndoLabel;       // user-visible undo-stack label
    std::string (*MakeBody)(const std::string& resolvedStem); // generates the file contents
};

// Declaration order == menu order == dispatch order. Defined in AssetsBrowserController.cpp so
// MakeBody can reach the file-local C#-identifier sanitizer without exporting it.
const CodeAssetDescriptor* GetCodeAssetDescriptors(std::size_t& outCount);

} // namespace GameEngine::Editor
