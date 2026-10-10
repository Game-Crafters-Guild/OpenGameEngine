#pragma once

#include <memory>
#include <string>
#include "UI/UIElement.h"
#include "UI/UITemplateNode.h"

namespace GameEngine {
namespace UIParsing {

// Wrapper around pugixml (when available) with a graceful fallback.
class XMLParser {
public:
    // Parse an XML string into a UIElement tree. Returns false on parse error.
    static bool ParseLayoutFromString(const std::string& xmlText,
                                      std::unique_ptr<UIElement>& outRoot);

    // Parse an XML string with an explicit source name (used for diagnostics like file:line).
    static bool ParseLayoutFromString(const std::string& xmlText,
                                      std::unique_ptr<UIElement>& outRoot,
                                      const std::string& sourceName);

    // Parse a file on disk into a UIElement tree. Returns false on IO or parse error.
    static bool ParseLayoutFromFile(const std::string& path,
                                    std::unique_ptr<UIElement>& outRoot);

    // Parse an XML string into a lightweight UITemplateNode tree. Template trees
    // are purely declarative data carriers (no control factories are invoked).
    // They can later be instantiated into live UIElement controls by the UI
    // system, avoiding duplication of control-internal children during cloning
    // and hot reload.
    static bool ParseLayoutTemplateFromString(const std::string& xmlText,
                                              std::unique_ptr<UITemplateNode>& outRoot);

    static bool ParseLayoutTemplateFromString(const std::string& xmlText,
                                              std::unique_ptr<UITemplateNode>& outRoot,
                                              const std::string& sourceName);

    static bool ParseLayoutTemplateFromFile(const std::string& path,
                                            std::unique_ptr<UITemplateNode>& outRoot);
};

} // namespace UIParsing
} // namespace GameEngine

