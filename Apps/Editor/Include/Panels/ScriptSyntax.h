#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

/// What the Script Editor highlights a file as. C-family sources without their
/// own lexer (C++, HLSL-adjacent headers) fall back to the C# rules.
enum class ScriptLanguage : uint8_t
{
    CSharp,
    Glsl,
};

/// Language by file extension: shader sources (.glsl, .vert, .frag, .comp, …)
/// are GLSL; everything else is C#.
ScriptLanguage ScriptLanguageForPath(const std::filesystem::path& path);

enum class SyntaxTokenType : uint8_t
{
    Keyword,
    String,
    Comment,
    Number,
    Type,          // built-in types, and GLSL built-in variables/functions
    Identifier,
    Operator,
    Preprocessor,  // a whole `#...` line
    Annotation,    // an `@tag` inside a comment — the surface-shader declarations
    Default
};

struct SyntaxToken
{
    SyntaxTokenType Type;
    std::size_t Start;   // byte offset into the text
    std::size_t Length;  // bytes
};

/// Highlighting tokenizer: byte ranges in `text`, non-overlapping, ascending. Whitespace
/// yields no token. Strings honour backslash escapes; comments and preprocessor lines
/// extend to end of line; block comments to `*/` (or end of text).
void TokenizeScript(ScriptLanguage language, const std::string& text, std::vector<SyntaxToken>& tokens);

} // namespace GameEngine::Editor
