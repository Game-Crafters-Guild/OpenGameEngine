#include "Assets/Packages/PackagesIndex.h"

#include "AssetCore/SharedFileRead.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine
{

bool TryLoadPackagesIndex(const std::filesystem::path& indexFile,
                          PackagesIndex& out,
                          std::string& outError)
{
    out = PackagesIndex{};
    outError.clear();

    std::error_code ec;
    if (!std::filesystem::exists(indexFile, ec))
        return true; // no packages staged — the quiet path

    String text;
    if (!ReadFileTextShared(indexFile, text))
    {
        outError = "packages.index (" + indexFile.generic_string() + "): cannot open file";
        return false;
    }

    const nlohmann::json doc =
        nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("packages") ||
        !doc["packages"].is_array())
    {
        outError = "packages.index (" + indexFile.generic_string() +
                   "): not a JSON object with a 'packages' array";
        return false;
    }

    for (const auto& node : doc["packages"])
    {
        if (!node.is_object() || !node.contains("name") || !node["name"].is_string() ||
            !node.contains("alias") || !node["alias"].is_string())
        {
            outError = "packages.index (" + indexFile.generic_string() +
                       "): every entry needs string 'name' and 'alias' fields";
            return false;
        }
        PackagesIndexEntry entry;
        entry.Name = node["name"].get<std::string>();
        entry.Alias = node["alias"].get<std::string>();
        if (node.contains("version") && node["version"].is_string())
            entry.Version = node["version"].get<std::string>();
        if (node.contains("priority") && node["priority"].is_number_integer())
            entry.Priority = node["priority"].get<int32_t>();
        if (node.contains("nativeModules"))
        {
            if (!node["nativeModules"].is_array())
            {
                outError = "packages.index (" + indexFile.generic_string() +
                           "): 'nativeModules' must be an array of strings";
                return false;
            }
            for (const auto& mod : node["nativeModules"])
            {
                if (!mod.is_string())
                {
                    outError = "packages.index (" + indexFile.generic_string() +
                               "): 'nativeModules' must be an array of strings";
                    return false;
                }
                entry.NativeModules.push_back(mod.get<std::string>());
            }
        }
        out.Packages.push_back(std::move(entry));
    }
    return true;
}

bool SavePackagesIndex(const std::filesystem::path& indexFile, const PackagesIndex& index)
{
    nlohmann::json packages = nlohmann::json::array();
    for (const PackagesIndexEntry& entry : index.Packages)
    {
        nlohmann::json node = nlohmann::json::object();
        node["name"] = entry.Name;
        node["alias"] = entry.Alias;
        node["version"] = entry.Version;
        node["priority"] = entry.Priority;
        if (!entry.NativeModules.empty())
            node["nativeModules"] = entry.NativeModules;
        packages.push_back(std::move(node));
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["packages"] = std::move(packages);

    std::error_code ec;
    std::filesystem::create_directories(indexFile.parent_path(), ec);
    std::ofstream out(indexFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << doc.dump(2) << '\n';
    return out.good();
}

} // namespace GameEngine
