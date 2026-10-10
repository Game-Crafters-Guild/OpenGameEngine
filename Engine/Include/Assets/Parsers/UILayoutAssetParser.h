#pragma once

#include "Assets/ParserRegistry.h"
#include "UI/Assets/UILayoutAsset.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace GameEngine
{

// UI Layout Asset Parser - handles .xml (sniffed), .uxml, .xaml.
class UILayoutAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::UILayout; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".xml", ".uxml", ".xaml"}; }

    bool CanParse(const std::filesystem::path& filePath) const override
    {
        // For .xml, sniff the content to decide whether this is a UI layout XML vs generic XML.
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

        if (ext == ".xml")
        {
            // Read a small prefix and try to detect the first element tag.
            std::ifstream in(filePath, std::ios::binary);
            if (!in.is_open())
            {
                return false;
            }

            std::string buf;
            buf.resize(8192);
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            buf.resize(static_cast<size_t>(in.gcount()));

            // Strip UTF-8 BOM if present.
            if (buf.size() >= 3 &&
                static_cast<unsigned char>(buf[0]) == 0xEF &&
                static_cast<unsigned char>(buf[1]) == 0xBB &&
                static_cast<unsigned char>(buf[2]) == 0xBF)
            {
                buf.erase(0, 3);
            }

            auto lower = [](std::string s)
            {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            };

            // Very small non-validating scan: skip whitespace, comments, and XML declarations,
            // then read the first start tag name.
            size_t i = 0;
            auto skipWs = [&]()
            {
                while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
                    ++i;
            };

            for (int steps = 0; steps < 64; ++steps)
            {
                skipWs();
                if (i >= buf.size())
                    return false;

                if (buf.compare(i, 4, "<!--") == 0)
                {
                    const size_t end = buf.find("-->", i + 4);
                    if (end == std::string::npos)
                        return false;
                    i = end + 3;
                    continue;
                }
                if (buf.compare(i, 2, "<?") == 0)
                {
                    const size_t end = buf.find("?>", i + 2);
                    if (end == std::string::npos)
                        return false;
                    i = end + 2;
                    continue;
                }
                if (buf.compare(i, 9, "<!DOCTYPE") == 0 || buf.compare(i, 9, "<!doctype") == 0)
                {
                    const size_t end = buf.find('>', i + 2);
                    if (end == std::string::npos)
                        return false;
                    i = end + 1;
                    continue;
                }

                if (buf[i] != '<')
                {
                    // Not a tag; skip one char and continue.
                    ++i;
                    continue;
                }

                // Found '<'
                ++i;
                if (i >= buf.size())
                    return false;

                if (buf[i] == '/' || buf[i] == '!' || buf[i] == '?')
                {
                    // Closing tag or other markup; skip until next '>' and continue.
                    const size_t end = buf.find('>', i);
                    if (end == std::string::npos)
                        return false;
                    i = end + 1;
                    continue;
                }

                // Parse tag name [A-Za-z0-9_:-]+ until whitespace or '>' or '/'.
                const size_t nameStart = i;
                while (i < buf.size())
                {
                    const char c = buf[i];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/')
                        break;
                    ++i;
                }
                if (i <= nameStart)
                    return false;

                std::string name = lower(buf.substr(nameStart, i - nameStart));

                // Heuristic: treat known UI root/control element names as UILayout.
                // (This is intentionally small; we can expand as new controls are added.)
                if (name == "ui" || name == "uielement" || name == "label" || name == "button" ||
                    name == "checkbox" || name == "toggle" || name == "slider" || name == "foldout" ||
                    name == "accordion" || name == "accordionitem" || name == "dropdown")
                {
                    return true;
                }

                return false;
            }

            return false;
        }

        // For .uxml/.xaml, extension match is sufficient.
        return AssetParser::CanParse(filePath);
    }

    AssetParseResult Parse(const AssetMetadata& metadata [[maybe_unused]],
                           [[maybe_unused]] AssetManager& assetManager) override
    {
        auto asset = std::make_shared<UILayoutAsset>(metadata.Guid, metadata.Path);
        return AssetParseResult(asset);
    }

    std::string GetName() const override { return "UILayoutAssetParser"; }

    int GetPriority() const override { return 100; }

    // Walk the layout XML and emit UILayoutStyle edges for every
    // `style="path/to.css"` attribute, plus `Other` edges for every
    // `layout="path/to.uxml"` sub-layout reference. References are
    // path-form (relative to the same mount); the registry resolves them
    // at query time.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
