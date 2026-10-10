#include "CssImports.h"

#include "UI/StyleUtil.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace GameEngine::UI {

namespace {

constexpr int kMaxImportDepth = 32;
constexpr std::string_view kImportKeyword = "@import";
constexpr std::string_view kUrlFunction = "url(";

// Index of the first unquoted @p terminator at or after @p from, or npos.
size_t FindUnquoted(std::string_view text, size_t from, char terminator)
{
    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = from; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '"' && !inSingle)
            inDouble = !inDouble;
        else if (c == '\'' && !inDouble)
            inSingle = !inSingle;
        else if (!inSingle && !inDouble && c == terminator)
            return i;
    }
    return std::string_view::npos;
}

// Strips one pair of matching quotes; false when the opening quote is unclosed.
bool UnquoteView(std::string_view& text)
{
    if (text.empty() || (text.front() != '"' && text.front() != '\''))
        return true;
    const char quote = text.front();
    text.remove_prefix(1);
    const size_t end = text.find(quote);
    if (end == std::string_view::npos)
        return false;
    text = text.substr(0, end);
    return true;
}

// Target of `@import "x";`, `@import 'x';` or `@import url(x);`; empty when malformed.
std::string ParseImportTarget(std::string_view statement)
{
    statement = TrimWhitespaceView(statement);
    if (!StartsWithIgnoreCase(statement, kImportKeyword))
        return {};
    statement = TrimWhitespaceView(statement.substr(kImportKeyword.size()));
    if (!statement.empty() && statement.back() == ';')
        statement = TrimWhitespaceView(statement.substr(0, statement.size() - 1));
    if (statement.empty())
        return {};

    if (statement.front() == '"' || statement.front() == '\'')
    {
        if (!UnquoteView(statement))
            return {};
        return std::string(statement);
    }

    if (!StartsWithIgnoreCase(statement, kUrlFunction))
        return {};
    statement.remove_prefix(kUrlFunction.size());
    const size_t close = FindUnquoted(statement, 0, ')');
    if (close == std::string_view::npos)
        return {};
    std::string_view inner = TrimWhitespaceView(statement.substr(0, close));
    if (!UnquoteView(inner))
        return {};
    return std::string(TrimWhitespaceView(inner));
}

bool ExpandRecursive(const std::filesystem::path& importerPath,
                     std::string_view cssText,
                     const AssetManager* assets,
                     std::string* outExpanded,
                     std::vector<std::filesystem::path>& outImportFiles,
                     std::unordered_set<std::string>& stack,
                     int depth)
{
    if (depth > kMaxImportDepth)
        return false;

    if (outExpanded)
    {
        outExpanded->clear();
        outExpanded->reserve(cssText.size());
    }

    size_t copied = 0;
    for (const CssImportStatement& statement : FindTopLevelCssImports(cssText))
    {
        if (outExpanded)
            outExpanded->append(cssText.substr(copied, statement.Begin - copied));
        copied = statement.End;

        const std::filesystem::path resolved = ResolveCssImportPath(importerPath, statement.Target, assets);
        if (resolved.empty())
        {
            Logger::Log::Warning("UI: CSS @import target '{}' not found (from '{}')", statement.Target, importerPath.string());
            continue;
        }

        const std::string key = NormalizeStylesheetPathKey(resolved);
        if (stack.contains(key))
        {
            Logger::Log::Warning("UI: CSS @import cycle detected (skipping): '{}' imports '{}'", importerPath.string(), resolved.string());
            continue;
        }

        String imported;
        if (!ReadFileTextShared(resolved, imported))
        {
            Logger::Log::Warning("UI: Failed to read imported stylesheet '{}' (from '{}')", resolved.string(), importerPath.string());
            continue;
        }

        std::string expandedChild;
        std::vector<std::filesystem::path> childFiles;
        stack.insert(key);
        const bool ok = ExpandRecursive(resolved, imported, assets, outExpanded ? &expandedChild : nullptr,
                                        childFiles, stack, depth + 1);
        stack.erase(key);
        if (!ok)
            continue;

        if (outExpanded)
        {
            outExpanded->push_back('\n');
            outExpanded->append(expandedChild);
            outExpanded->push_back('\n');
        }
        outImportFiles.push_back(resolved);
        outImportFiles.insert(outImportFiles.end(), childFiles.begin(), childFiles.end());
    }

    if (outExpanded)
        outExpanded->append(cssText.substr(copied));
    return true;
}

} // namespace

std::vector<CssImportStatement> FindTopLevelCssImports(std::string_view cssText)
{
    std::vector<CssImportStatement> statements;

    bool inBlockComment = false;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    int braceDepth = 0;

    size_t i = 0;
    while (i < cssText.size())
    {
        const char c = cssText[i];

        if (!inSingleQuote && !inDoubleQuote && i + 1 < cssText.size())
        {
            if (!inBlockComment && c == '/' && cssText[i + 1] == '*')
            {
                inBlockComment = true;
                i += 2;
                continue;
            }
            if (inBlockComment && c == '*' && cssText[i + 1] == '/')
            {
                inBlockComment = false;
                i += 2;
                continue;
            }
        }
        if (inBlockComment)
        {
            ++i;
            continue;
        }

        if (c == '"' && !inSingleQuote)
        {
            inDoubleQuote = !inDoubleQuote;
            ++i;
            continue;
        }
        if (c == '\'' && !inDoubleQuote)
        {
            inSingleQuote = !inSingleQuote;
            ++i;
            continue;
        }
        if (inSingleQuote || inDoubleQuote)
        {
            ++i;
            continue;
        }

        if (c == '{')
            ++braceDepth;
        else if (c == '}')
            braceDepth = std::max(0, braceDepth - 1);

        if (braceDepth == 0 && c == '@' && i + kImportKeyword.size() < cssText.size() &&
            StartsWithIgnoreCase(cssText.substr(i), kImportKeyword))
        {
            const size_t semicolon = FindUnquoted(cssText, i, ';');
            if (semicolon != std::string_view::npos)
            {
                std::string target = ParseImportTarget(cssText.substr(i, semicolon - i + 1));
                if (!target.empty())
                {
                    statements.push_back({i, semicolon + 1, std::move(target)});
                    i = semicolon + 1;
                    continue;
                }
            }
        }

        ++i;
    }

    return statements;
}

std::filesystem::path ResolveCssImportPath(const std::filesystem::path& importerPath,
                                           std::string_view rawImport,
                                           const AssetManager* assets)
{
    if (rawImport.empty())
        return {};

    const std::filesystem::path importPath(UIUtil::NormalizeCssUrlPath(std::string(rawImport)));
    if (importPath.is_absolute())
        return importPath;

    // CSS semantics: relative to the importing file first.
    std::error_code ec;
    const std::filesystem::path relative = (importerPath.parent_path() / importPath).lexically_normal();
    if (std::filesystem::exists(relative, ec) && !ec)
        return relative;

    if (!assets)
        return {};

    const std::filesystem::path resolved = assets->ResolveAssetPathFromReference(importPath, importerPath).lexically_normal();
    if (std::filesystem::exists(resolved, ec) && !ec)
        return resolved;
    return {};
}

std::string NormalizeStylesheetPathKey(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::absolute(path, ec);
    if (ec)
        normalized = path;
    std::string key = normalized.lexically_normal().string();
#ifdef _WIN32
    for (char& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return key;
}

void ExpandCssImports(const std::filesystem::path& importerPath,
                      std::string_view cssText,
                      const AssetManager* assets,
                      std::string& outExpanded,
                      std::vector<std::filesystem::path>& outImportFiles)
{
    std::unordered_set<std::string> stack{NormalizeStylesheetPathKey(importerPath)};
    (void)ExpandRecursive(importerPath, cssText, assets, &outExpanded, outImportFiles, stack, 0);
}

void CollectCssImportFiles(const std::filesystem::path& importerPath,
                           std::string_view cssText,
                           const AssetManager* assets,
                           std::vector<std::filesystem::path>& outImportFiles)
{
    std::unordered_set<std::string> stack{NormalizeStylesheetPathKey(importerPath)};
    (void)ExpandRecursive(importerPath, cssText, assets, nullptr, outImportFiles, stack, 0);
}

} // namespace GameEngine::UI
