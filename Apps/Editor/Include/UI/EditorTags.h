#pragma once

#include <string>
#include <vector>

namespace GameEngine {

struct EditorTagDefinition {
    std::string Name;
    std::string Color;  // empty = no color (unfilled circle), otherwise e.g. "#3498db"
};

// Tag definitions (name + color) stored in project preferences (ProjectSettings or equivalent).
// Each project has its own tag list; Load/Save use key "tags" (JSON array of { "name", "color" }).
namespace EditorTags {

// Default tag set when none are saved (color names with hex colors only).
std::vector<EditorTagDefinition> GetDefaults();

// Load from current project preferences; returns defaults if no project, missing or invalid.
std::vector<EditorTagDefinition> Load();

// Save to current project preferences. Returns false if no project open.
bool Save(const std::vector<EditorTagDefinition>& tags);

} // namespace EditorTags

} // namespace GameEngine
