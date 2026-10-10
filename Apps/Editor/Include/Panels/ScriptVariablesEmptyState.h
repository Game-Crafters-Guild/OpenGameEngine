#pragma once

// The Script Editor opens shaders and native source as well as C#, and it reports an empty
// variable list for all of them — a .glsl has no fields to parse, not zero of them. So the
// inspector's empty state has to name the language of the file that produced it: telling a
// surface-shader author to write `public int myField` is advice from the wrong language.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

inline std::string EmptyScriptVariablesText(const std::filesystem::path& scriptPath)
{
    std::string ext = scriptPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".cs")
    {
        return "No serializable fields found.\n\nTo expose fields, use:\n  public int myField = 0;\nor\n"
               "  [SerializeField] private int myField;";
    }
    if (ext == ".glsl" || ext == ".hlsl")
        return "Surface shader — edit to change its parameters.";

    return "This file type has no inspectable fields.";
}

} // namespace GameEngine::Editor
