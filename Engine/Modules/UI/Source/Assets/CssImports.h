#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {
class AssetManager;
}

namespace GameEngine::UI {

/// One top-level `@import "x.css";` / `@import url(x.css);` statement.
/// [Begin, End) covers the statement text including its terminating ';'.
struct CssImportStatement
{
    size_t Begin = 0;
    size_t End = 0;
    std::string Target;
};

/// Top-level @import statements in source order. Statements inside comments,
/// strings or rule blocks are ignored, as are statements with no target.
std::vector<CssImportStatement> FindTopLevelCssImports(std::string_view cssText);

/// Resolves an @import target: relative to the importing file first, then
/// through @p assets using the importer's source (empty when not found).
std::filesystem::path ResolveCssImportPath(const std::filesystem::path& importerPath,
                                           std::string_view rawImport,
                                           const AssetManager* assets);

/// Absolute, lexically normal, case-folded on Windows; identifies a stylesheet
/// file for @import cycle detection and stylesheet caches.
std::string NormalizeStylesheetPathKey(const std::filesystem::path& path);

/// Inlines every @import of @p cssText recursively. Unresolvable, unreadable
/// and cyclic imports are dropped with a warning. @p outImportFiles receives
/// every transitively imported file.
void ExpandCssImports(const std::filesystem::path& importerPath,
                      std::string_view cssText,
                      const AssetManager* assets,
                      std::string& outExpanded,
                      std::vector<std::filesystem::path>& outImportFiles);

/// The file list of ExpandCssImports without building the expanded text.
void CollectCssImportFiles(const std::filesystem::path& importerPath,
                           std::string_view cssText,
                           const AssetManager* assets,
                           std::vector<std::filesystem::path>& outImportFiles);

} // namespace GameEngine::UI
