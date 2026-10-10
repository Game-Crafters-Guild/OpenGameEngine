#include "Rendering/ShaderGraph/SgNodeReflector.h"

#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace GameEngine::ShaderGraph
{
namespace
{

void ReflectHelperFile(const std::filesystem::path& path, SgNodeLibraryIndex& index)
{
    const SgParsedFile parsed = ParseShaderGraphFile(path);
    if (parsed.TagBlock.empty() && parsed.Body.empty())
        return;

    std::string source = parsed.TagBlock;
    if (!parsed.Body.empty())
    {
        if (!source.empty())
            source.push_back('\n');
        source += parsed.Body;
    }

    std::istringstream tagStream(parsed.TagBlock);
    std::string line;
    std::vector<std::pair<std::string, std::string>> fileTags;
    while (std::getline(tagStream, line))
    {
        const auto attrs = ParseTagAttributes(line);
        if (attrs.empty())
            continue;
        const std::string kind = attrs[0].first == "kind" ? attrs[0].second : "";
        if (kind == "category")
        {
            std::string category = FindTagAttribute(attrs, "category").value_or("");
            if (category.empty())
                category = FindTagAttribute(attrs, "arg0").value_or("");
            if (!category.empty())
                fileTags.emplace_back("category", category);
            continue;
        }
        for (const auto& [k, v] : attrs)
            fileTags.emplace_back(k, v);
    }

    std::istringstream lineStream(source);
    size_t lineIndex = 0;
    while (std::getline(lineStream, line))
    {
        ++lineIndex;
        const auto attrs = ParseTagAttributes(line);
        if (attrs.empty())
            continue;
        const std::string kind = attrs[0].first == "kind" ? attrs[0].second : "";
        if (kind != "sgnode")
            continue;

        std::string typeId = FindTagAttribute(attrs, "sgnode").value_or("");
        if (typeId.empty())
            typeId = FindTagAttribute(attrs, "arg0").value_or("");

        auto def = ParseHelperNodeFromSource(path, source, typeId, fileTags);
        if (def.TypeId.empty() || def.Overloads.empty())
            continue;

        auto existing = index.NodesByType.find(def.TypeId);
        if (existing != index.NodesByType.end())
        {
            for (auto& o : def.Overloads)
                existing->second.Overloads.push_back(std::move(o));
        }
        else
        {
            index.NodesByType.emplace(def.TypeId, std::move(def));
        }
    }
}

void GlobReflect(const std::filesystem::path& root, SgNodeLibraryIndex& index)
{
    if (root.empty() || !std::filesystem::exists(root))
        return;

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec))
    {
        const auto& entry = *it;
        if (entry.is_directory(ec))
        {
            const std::string name = entry.path().filename().string();
            if (name == ".Cache" || name == ".cache" || name == "Generated" ||
                name == "Library" || name == "Temp" || name == "node_modules")
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file(ec) || entry.path().extension() != ".glsl")
            continue;
        ReflectHelperFile(entry.path(), index);
    }
}

} // namespace

SgNodeLibraryIndex SgNodeReflector::BuildIndex(const std::filesystem::path& engineNodesRoot,
                                                const std::filesystem::path& projectNodesRoot)
{
    SgNodeLibraryIndex index;
    RegisterPseudoNodes(index);
    GlobReflect(engineNodesRoot, index);
    GlobReflect(projectNodesRoot, index);
    return index;
}

bool SgNodeReflector::WriteCache(const std::filesystem::path& cachePath, const SgNodeLibraryIndex& index)
{
    try
    {
        std::filesystem::create_directories(cachePath.parent_path());
        nlohmann::json j;
        j["version"] = 1;
        nlohmann::json nodes = nlohmann::json::array();
        for (const auto& [typeId, def] : index.NodesByType)
        {
            nlohmann::json n;
            n["typeId"] = typeId;
            n["displayName"] = def.DisplayName;
            n["category"] = def.Category;
            n["sourceFile"] = def.SourceFile;
            nodes.push_back(std::move(n));
        }
        j["nodes"] = std::move(nodes);
        std::ofstream out(cachePath);
        out << j.dump(2);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SgNodeReflector::TryLoadCache(const std::filesystem::path& cachePath,
                                    const std::filesystem::path& engineNodesRoot,
                                    const std::filesystem::path& projectNodesRoot,
                                    SgNodeLibraryIndex& outIndex)
{
    if (!std::filesystem::exists(cachePath))
    {
        outIndex = BuildIndex(engineNodesRoot, projectNodesRoot);
        WriteCache(cachePath, outIndex);
        return true;
    }

    try
    {
        std::ifstream in(cachePath);
        nlohmann::json j;
        in >> j;
        (void)j;
    }
    catch (...)
    {
    }

    outIndex = BuildIndex(engineNodesRoot, projectNodesRoot);
    return true;
}

} // namespace GameEngine::ShaderGraph
