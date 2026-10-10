#include "Engine/Build/PlayerSourcePreparation.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_set>

namespace GameEngine
{

namespace fs = std::filesystem;

std::vector<fs::path> CollectCustomPlayerSources(const fs::path& assetRoot)
{
    std::vector<fs::path> sources;
    std::error_code ec;
    const fs::path sourceDir = assetRoot / "Source";
    if (!fs::is_directory(sourceDir, ec))
        return sources;
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
    {
        const auto extension = entry.path().extension();
        if (entry.is_regular_file() && (extension == ".cpp" || extension == ".h"))
            sources.push_back(entry.path());
    }
    std::sort(sources.begin(), sources.end());
    return sources;
}

bool PreparePlayerSources(const fs::path& templateDir,
                          const fs::path& generatedSourceDir,
                          const std::vector<fs::path>& customSources,
                          std::vector<fs::path>& sources,
                          std::string& error)
{
    sources.clear();
    error.clear();
    try
    {
        std::unordered_set<std::string> paths;
        bool customApplication = false;
        for (const auto& source : customSources)
        {
            if (paths.insert(fs::weakly_canonical(source).generic_string()).second)
                sources.push_back(source);
            const auto filename = source.filename();
            customApplication |= filename == "main.cpp" || filename == "PlayerApplication.cpp";
        }
        if (customApplication)
            return true;

        const auto manifestPath = templateDir / "DesktopSources.txt";
        std::ifstream manifest(manifestPath);
        if (!manifest)
        {
            error = "Desktop Player source manifest not found: " + manifestPath.string() +
                    ". Rebuild or install a matching Editor SDK.";
            return false;
        }

        // Validate the complete list before writing anything. It is shared with
        // Apps/Player's desktop CMake target; Web sources are a separate target.
        std::vector<fs::path> templates;
        std::unordered_set<std::string> templatePaths;
        std::string line;
        while (std::getline(manifest, line))
        {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#')
                continue;
            line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            const fs::path relative(line);
            const bool unsafe = relative.has_root_path() || line.find('\\') != std::string::npos ||
                line.find(':') != std::string::npos || line.find(';') != std::string::npos ||
                std::any_of(relative.begin(), relative.end(), [](const fs::path& part) {
                    return part == ".." || part == ".";
                });
            if (unsafe || (relative.extension() != ".cpp" && relative.extension() != ".h"))
            {
                error = "Invalid desktop Player source '" + line + "' in " + manifestPath.string();
                return false;
            }
            if (!fs::is_regular_file(templateDir / relative))
            {
                error = "Desktop Player template source not found: " + (templateDir / relative).string();
                return false;
            }
            if (templatePaths.insert(relative.generic_string()).second)
                templates.push_back(relative);
        }
        if (manifest.bad() || templates.empty())
        {
            error = "Could not read a nonempty desktop Player source list: " + manifestPath.string();
            return false;
        }

        auto readBytes = [](const fs::path& path, std::string& bytes) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !in.bad();
        };
        for (const auto& relative : templates)
        {
            const auto source = templateDir / relative;
            const auto destination = generatedSourceDir / relative;
            std::string sourceBytes, destinationBytes;
            if (!readBytes(source, sourceBytes))
            {
                error = "Failed to read desktop Player template: " + source.string();
                return false;
            }
            if (!readBytes(destination, destinationBytes) || sourceBytes != destinationBytes)
            {
                fs::create_directories(destination.parent_path());
                fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
            }
            if (paths.insert(fs::weakly_canonical(destination).generic_string()).second)
                sources.push_back(destination);
        }
        return true;
    }
    catch (const fs::filesystem_error& e)
    {
        error = "Failed to prepare desktop Player sources: " + std::string(e.what());
        return false;
    }
}

} // namespace GameEngine
