#include "Panels/ShaderErrorRows.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace GameEngine
{
namespace
{

bool IsSpaceChar(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view TrimView(std::string_view s)
{
    while (!s.empty() && IsSpaceChar(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && IsSpaceChar(s.back()))
        s.remove_suffix(1);
    return s;
}

// The project-relative rendering of `path`, or an empty string when it does not
// sit under `projectRoot`. Single source for both the display text and the
// project/engine classification.
std::string ProjectRelativeDisplay(const std::filesystem::path& path,
                                   const std::filesystem::path& projectRoot)
{
    if (path.empty() || projectRoot.empty() || !path.is_absolute())
        return {};
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, projectRoot, ec);
    if (ec || rel.empty())
        return {};
    // A relative path that climbs out of the root is outside the project.
    if (rel.native().find(std::filesystem::path("..").native()) != std::string::npos)
        return {};
    return rel.generic_string();
}

} // namespace

bool ParseShaderDiagnosticLine(std::string_view line, std::string& outFile, size_t& outLine,
                               std::string& outMessage)
{
    line = TrimView(line);
    if (!line.empty() && line.front() == '-')
        line = TrimView(line.substr(1));
    if (line.empty())
        return false;

    static constexpr std::string_view kExts[] = {".glsl:", ".vert:", ".frag:",
                                                 ".comp:", ".geom:", ".hlsl:"};
    std::string lower(line);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (std::string_view ext : kExts)
    {
        const size_t at = lower.find(ext);
        if (at == std::string::npos)
            continue;
        const size_t fileEnd = at + ext.size() - 1; // index of the ':' after the extension
        size_t p = fileEnd + 1;
        size_t lineNo = 0;
        const size_t digitsStart = p;
        while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p])))
        {
            lineNo = lineNo * 10 + static_cast<size_t>(line[p] - '0');
            ++p;
        }
        if (p == digitsStart)
            continue; // "foo.glsl:" without a line number — not a diagnostic
        std::string_view rest = line.substr(p);
        if (!rest.empty() && rest.front() == ':')
            rest.remove_prefix(1);
        rest = TrimView(rest);
        for (std::string_view sev : {std::string_view("error:"), std::string_view("warning:")})
        {
            if (rest.size() >= sev.size()
                && std::equal(sev.begin(), sev.end(), rest.begin(),
                              [](char a, char b)
                              { return std::tolower(static_cast<unsigned char>(b)) == a; }))
            {
                rest = TrimView(rest.substr(sev.size()));
                break;
            }
        }
        outFile = std::string(line.substr(0, fileEnd));
        outLine = lineNo;
        outMessage = std::string(rest);
        return true;
    }
    return false;
}

bool IsProjectOwnedPath(const std::filesystem::path& path,
                        const std::filesystem::path& projectRoot)
{
    return !ProjectRelativeDisplay(path, projectRoot).empty();
}

std::string ShaderErrorDisplayPath(const std::filesystem::path& path,
                                   const ShaderErrorPathRoots& roots)
{
    const std::string projectRel = ProjectRelativeDisplay(path, roots.ProjectRoot);
    if (!projectRel.empty())
        return projectRel;
    // Shipped editor content displays with its mount alias, the same spelling
    // CSS uses to pin the mount ("editor:Icons/foo.png"), instead of wherever
    // this machine staged or mirrored the tree.
    for (const std::filesystem::path& editorRoot : roots.EditorAssetsRoots)
    {
        const std::string editorRel = ProjectRelativeDisplay(path, editorRoot);
        if (!editorRel.empty())
            return "editor:" + editorRel;
    }
    return path.generic_string();
}

std::vector<ShaderErrorRow> BuildShaderErrorRows(
    const std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry>& entries,
    const ShaderErrorPathRoots& roots)
{
    const std::filesystem::path& projectRoot = roots.ProjectRoot;
    std::vector<ShaderErrorRow> rows;

    for (const Engine::Renderer::ShaderCompileErrorLog::Entry& entry : entries)
    {
        const size_t firstRowOfEntry = rows.size();
        for (const std::string& raw : entry.Errors)
        {
            // Build-service errors arrive as multi-line blobs (context header,
            // Source/Stage/Defines, then the compiler diagnostics) — parse per
            // line so each diagnostic gets its own row.
            size_t start = 0;
            while (start <= raw.size())
            {
                const size_t nl = raw.find('\n', start);
                const std::string_view lineView(raw.data() + start,
                                                (nl == std::string::npos ? raw.size() : nl) - start);
                ShaderErrorRow row;
                if (ParseShaderDiagnosticLine(lineView, row.File, row.Line, row.Message))
                {
                    row.MaterialName = entry.MaterialName;
                    rows.push_back(std::move(row));
                }
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
        }
        if (rows.size() == firstRowOfEntry)
        {
            // Nothing parseable (an unresolved surface reference, or a graph
            // diagnostic that names a node rather than a file:line) — keep every
            // diagnostic visible, one row each, rather than only the first.
            for (const std::string& raw : entry.Errors)
            {
                const std::string_view trimmed =
                    TrimView(std::string_view(raw).substr(0, raw.find('\n')));
                if (trimmed.empty())
                    continue;
                ShaderErrorRow row;
                row.MaterialName = entry.MaterialName;
                row.File = entry.SurfaceShaderPath;
                row.Message = std::string(trimmed);
                rows.push_back(std::move(row));
            }
            if (rows.size() == firstRowOfEntry)
            {
                ShaderErrorRow row;
                row.MaterialName = entry.MaterialName;
                row.File = entry.SurfaceShaderPath;
                row.Message = "shader compile failed (no diagnostics)";
                rows.push_back(std::move(row));
            }
        }

        // Classify every row this entry produced. The failing FILE decides when
        // it resolves; otherwise the failing MATERIAL does, which is what carries
        // an engine-internal material whose diagnostic names an engine include.
        const bool materialFromProject = IsProjectOwnedPath(entry.MaterialAssetPath, projectRoot);
        for (size_t i = firstRowOfEntry; i < rows.size(); ++i)
        {
            ShaderErrorRow& row = rows[i];
            const std::string rel = ProjectRelativeDisplay(row.File, projectRoot);
            row.DisplayPath = rel.empty() ? ShaderErrorDisplayPath(row.File, roots) : rel;
            row.FromProject = !rel.empty() || materialFromProject;
        }
    }

    std::stable_partition(rows.begin(), rows.end(),
                          [](const ShaderErrorRow& row) { return row.FromProject; });
    return rows;
}

} // namespace GameEngine
