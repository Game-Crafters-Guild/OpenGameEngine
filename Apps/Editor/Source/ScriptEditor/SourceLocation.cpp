#include "ScriptEditor/SourceLocation.h"

#include <cctype>
#include <string>

namespace GameEngine::Editor
{
namespace
{

constexpr std::string_view kCSharpExtension = ".cs";

bool IsPathDelimiter(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == '(' || c == ')' ||
           c == '[' || c == ']' || c == '{' || c == '}';
}

size_t FindPathStart(std::string_view text, size_t pathEnd)
{
    size_t start = pathEnd;
    while (start > 0 && !IsPathDelimiter(text[start - 1]))
        --start;
    return start;
}

// Reads the digits at `cursor` and advances past them; nothing when there are none.
std::optional<size_t> ReadNumber(std::string_view text, size_t& cursor)
{
    if (cursor >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cursor])))
        return std::nullopt;
    size_t value = 0;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
        value = value * 10 + static_cast<size_t>(text[cursor++] - '0');
    return value;
}

void SkipSpaces(std::string_view text, size_t& cursor)
{
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
        ++cursor;
}

// Reads "line<separator>column" after an opening ':' or '(' at `cursor`.
void ReadLineAndColumn(std::string_view text, size_t cursor, char separator, SourceLocation& location)
{
    ++cursor;
    const std::optional<size_t> line = ReadNumber(text, cursor);
    if (!line)
        return;
    location.Line = *line;

    if (cursor >= text.size() || text[cursor] != separator)
        return;
    ++cursor;
    if (separator == ',')
        SkipSpaces(text, cursor);
    if (const std::optional<size_t> column = ReadNumber(text, cursor))
        location.Column = *column;
}

} // namespace

std::optional<SourceLocation> FindSourceLocationInLogLine(std::string_view text)
{
    const size_t extensionPos = text.rfind(kCSharpExtension);
    if (extensionPos == std::string_view::npos)
        return std::nullopt;

    const size_t pathEnd = extensionPos + kCSharpExtension.size();
    const size_t pathStart = FindPathStart(text, extensionPos);

    SourceLocation location;
    location.Path = std::filesystem::path(std::string(text.substr(pathStart, pathEnd - pathStart)));

    size_t cursor = pathEnd;
    SkipSpaces(text, cursor);
    if (cursor < text.size() && text[cursor] == ':')
        ReadLineAndColumn(text, cursor, ':', location);
    else if (cursor < text.size() && text[cursor] == '(')
        ReadLineAndColumn(text, cursor, ',', location);

    return location;
}

} // namespace GameEngine::Editor
