#pragma once

// Shader Errors row model: turns the engine's shader-compile failure log into
// the rows the panel lists. Owns the diagnostic-line parse, the project-vs-
// engine classification, the displayed path, and the row order — one place, so
// what a row is SORTED by and what it SHOWS can never drift apart.

#include "Engine/Rendering/ShaderCompileErrorLog.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

// One list row: a parsed diagnostic line of a failed material compile.
struct ShaderErrorRow
{
    std::string MaterialName;
    std::string File;    // absolute or authored path; empty when unparsed
    size_t Line = 0;
    std::string Message; // diagnostic text (or the raw line when unparsed)

    // File rendered for display: project-relative when it sits under the open
    // project, otherwise the path as-is.
    std::string DisplayPath;

    // The failure belongs to project content — the author's own work — rather
    // than shipped engine/editor content.
    bool FromProject = false;
};

/// Parse one compiler diagnostic of the shape the attribution work guarantees:
///   c:/.../my_surface.glsl:45: error: ';' expected
/// Returns false for context lines (Source:, Defines:, dump paths, ...).
bool ParseShaderDiagnosticLine(std::string_view line, std::string& outFile, size_t& outLine,
                               std::string& outMessage);

/// True when `path` names a file inside the open project (never for an empty or
/// relative path — an authored reference like "Surfaces/standard_pbr.glsl" names
/// no mount on its own).
bool IsProjectOwnedPath(const std::filesystem::path& path,
                        const std::filesystem::path& projectRoot);

/// The roots error paths display against. ProjectRoot is the open project;
/// EditorAssetsRoots are the shipped editor-assets trees (install root and,
/// on macOS, the user-writable mirror the shader watcher actually reads).
struct ShaderErrorPathRoots
{
    std::filesystem::path ProjectRoot;
    std::vector<std::filesystem::path> EditorAssetsRoots;
};

/// Display rendering of a diagnostic path: project-relative when it sits under
/// the project, "editor:<rel>" under an editor-assets root, as-is otherwise.
/// One spelling for the Shader Errors panel and the material inspector's error
/// block, so an author never reads a machine-local staging path in either.
std::string ShaderErrorDisplayPath(const std::filesystem::path& path,
                                   const ShaderErrorPathRoots& roots);

/// Rows for every entry in the failure log, in display order.
///
/// Project rows come first. Shipped engine/editor content that fails the
/// engine's own validation is not the author's problem and must never bury the
/// file they just saved — with six engine rows above it, the feature that
/// reveals the panel is defeated by the panel's own contents. Ordering is
/// stable, so within each group the log's first-failure-seen order survives.
std::vector<ShaderErrorRow> BuildShaderErrorRows(
    const std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry>& entries,
    const ShaderErrorPathRoots& roots);

} // namespace GameEngine
