#include "Assets/Packages/ProjectPackagesManifest.h"

#include "AssetCore/SharedFileRead.h"

#include "Assets/Packages/PackageManifest.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

// Shared parse-mutate-rewrite plumbing for the manifest edit helpers. JSON
// comments do not survive a rewrite (nlohmann strips them on parse).
bool LoadManifestDoc(const std::filesystem::path& manifestFile,
                     nlohmann::json& outDoc,
                     std::string& outError)
{
    String text;
    if (!ReadFileTextShared(manifestFile, text))
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): cannot open file";
        return false;
    }

    outDoc = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (outDoc.is_discarded() || !outDoc.is_object())
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): not a JSON object";
        return false;
    }
    return true;
}

bool SaveManifestDoc(const std::filesystem::path& manifestFile,
                     const nlohmann::json& doc,
                     std::string& outError)
{
    std::ofstream out(manifestFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): cannot write file";
        return false;
    }
    out << doc.dump(2) << '\n';
    if (!out.good())
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): write failed";
        return false;
    }
    return true;
}

} // namespace

bool TryLoadProjectPackagesManifest(const std::filesystem::path& manifestFile,
                                    ProjectPackagesManifest& out,
                                    std::string& outError)
{
    out = ProjectPackagesManifest{};
    outError.clear();

    std::error_code ec;
    if (!std::filesystem::exists(manifestFile, ec))
        return true; // no manifest = zero packages, zero noise

    String text;
    if (!ReadFileTextShared(manifestFile, text))
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): cannot open file";
        return false;
    }

    const nlohmann::json doc =
        nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false,
                              /*ignore_comments=*/true);
    if (doc.is_discarded())
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): not valid JSON";
        return false;
    }
    if (!doc.is_object())
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() + "): root must be a JSON object";
        return false;
    }

    if (doc.contains("dependencies"))
    {
        const auto& deps = doc["dependencies"];
        if (!deps.is_object())
        {
            outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                       "): field 'dependencies': must be an object of name -> spec";
            return false;
        }
        for (const auto& [name, specNode] : deps.items())
        {
            if (!IsValidPackageName(name))
            {
                outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                           "): field 'dependencies': '" + name + "' is not a valid package name";
                return false;
            }
            if (!specNode.is_string() || specNode.get<std::string>().empty())
            {
                outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                           "): field 'dependencies." + name + "': spec must be a non-empty string";
                return false;
            }
            out.Dependencies.emplace(name, specNode.get<std::string>());
        }
    }

    if (doc.contains("disabled"))
    {
        const auto& disabled = doc["disabled"];
        if (!disabled.is_array())
        {
            outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                       "): field 'disabled': must be an array of package names";
            return false;
        }
        for (const auto& item : disabled)
        {
            if (!item.is_string())
            {
                outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                           "): field 'disabled': must be an array of package names";
                return false;
            }
            out.Disabled.insert(item.get<std::string>());
        }
    }

    return true;
}

bool SetPackageDisabledInProjectManifest(const std::filesystem::path& manifestFile,
                                         const std::string& packageName,
                                         bool disabled,
                                         std::string& outError)
{
    outError.clear();

    nlohmann::json doc = nlohmann::json::object();
    std::error_code ec;
    const bool manifestExists = std::filesystem::exists(manifestFile, ec);
    if (ec)
    {
        outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                   "): cannot inspect file";
        return false;
    }
    if (manifestExists)
    {
        if (!LoadManifestDoc(manifestFile, doc, outError))
            return false;
    }
    else if (!disabled)
    {
        // Enabling a package when no opt-out manifest exists is already the
        // desired state; keep the project free of an unnecessary empty file.
        return true;
    }
    else
    {
        std::filesystem::create_directories(manifestFile.parent_path(), ec);
        if (ec)
        {
            outError = "Packages/manifest.json (" + manifestFile.generic_string() +
                       "): cannot create parent directory";
            return false;
        }
    }

    // Rebuild the array without the package, then append when disabling —
    // dedupes historical duplicates as a side effect.
    nlohmann::json filtered = nlohmann::json::array();
    if (doc.contains("disabled") && doc["disabled"].is_array())
    {
        for (const auto& item : doc["disabled"])
        {
            if (item.is_string() && item.get<std::string>() == packageName)
                continue;
            filtered.push_back(item);
        }
    }
    if (disabled)
        filtered.push_back(packageName);

    if (filtered.empty())
        doc.erase("disabled");
    else
        doc["disabled"] = std::move(filtered);

    return SaveManifestDoc(manifestFile, doc, outError);
}

bool AddPackageToProjectManifest(const std::filesystem::path& manifestFile,
                                 const std::string& packageName,
                                 const std::string& spec,
                                 std::string& outError)
{
    outError.clear();
    if (!IsValidPackageName(packageName))
    {
        outError = "'" + packageName + "' is not a valid package name";
        return false;
    }
    if (spec.empty())
    {
        outError = "package '" + packageName + "': empty spec";
        return false;
    }

    nlohmann::json doc = nlohmann::json::object();
    std::error_code ec;
    if (std::filesystem::exists(manifestFile, ec))
    {
        if (!LoadManifestDoc(manifestFile, doc, outError))
            return false;
    }
    else
    {
        std::filesystem::create_directories(manifestFile.parent_path(), ec);
    }

    if (doc.contains("dependencies") && doc["dependencies"].is_object() &&
        doc["dependencies"].contains(packageName))
    {
        outError = "package '" + packageName + "' is already declared in Packages/manifest.json";
        return false;
    }
    if (!doc.contains("dependencies") || !doc["dependencies"].is_object())
        doc["dependencies"] = nlohmann::json::object();
    doc["dependencies"][packageName] = spec;

    return SaveManifestDoc(manifestFile, doc, outError);
}

bool RemovePackageFromProjectManifest(const std::filesystem::path& manifestFile,
                                      const std::string& packageName,
                                      std::string& outError)
{
    outError.clear();

    nlohmann::json doc;
    if (!LoadManifestDoc(manifestFile, doc, outError))
        return false;

    if (!doc.contains("dependencies") || !doc["dependencies"].is_object() ||
        !doc["dependencies"].contains(packageName))
    {
        outError = "package '" + packageName + "' is not declared in Packages/manifest.json";
        return false;
    }
    doc["dependencies"].erase(packageName);
    if (doc["dependencies"].empty())
        doc.erase("dependencies");

    if (doc.contains("disabled") && doc["disabled"].is_array())
    {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& item : doc["disabled"])
        {
            if (item.is_string() && item.get<std::string>() == packageName)
                continue;
            filtered.push_back(item);
        }
        if (filtered.empty())
            doc.erase("disabled");
        else
            doc["disabled"] = std::move(filtered);
    }

    return SaveManifestDoc(manifestFile, doc, outError);
}

bool SetPackageSpecInProjectManifest(const std::filesystem::path& manifestFile,
                                     const std::string& packageName,
                                     const std::string& newSpec,
                                     std::string& outError)
{
    outError.clear();
    if (newSpec.empty())
    {
        outError = "package '" + packageName + "': empty spec";
        return false;
    }

    nlohmann::json doc;
    if (!LoadManifestDoc(manifestFile, doc, outError))
        return false;

    if (!doc.contains("dependencies") || !doc["dependencies"].is_object() ||
        !doc["dependencies"].contains(packageName))
    {
        outError = "package '" + packageName + "' is not declared in Packages/manifest.json";
        return false;
    }
    doc["dependencies"][packageName] = newSpec;

    return SaveManifestDoc(manifestFile, doc, outError);
}

} // namespace GameEngine
