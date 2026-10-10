#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {

// Lightweight data-only node for parsed XML layout templates.
// Template trees are never rendered, laid out, or styled — they are purely
// declarative data carriers that CloneElement / CloneFromTemplate walk to
// create live UIElement subclass instances via the factory registry.
//
// Using a dedicated struct instead of UIElement avoids wasting memory on
// vtables, atomic instance counters, event handlers, style deltas, and
// dozens of other fields that template nodes never use.
struct UITemplateNode
{
    // Canonical lowercase tag name from XML (e.g. "button", "dockablepanel").
    std::string TagName;

    // Element id (from "id" or "name" XML attribute).
    std::string Id;

    // CSS classes (from "class" XML attribute, split on whitespace).
    std::vector<std::string> Classes;

    // All authored XML attributes (lowercase keys, string values).
    std::unordered_map<std::string, std::string> Attributes;

    // Raw inline style text (from "style" XML attribute).
    std::string InlineStyle;

    // Child template nodes.
    std::vector<std::unique_ptr<UITemplateNode>> Children;

    // Convenience: check if a class is present.
    bool HasClass(const std::string& cls) const
    {
        for (const auto& c : Classes)
            if (c == cls) return true;
        return false;
    }

    // Convenience: find an attribute value by lowercase key.
    const std::string* FindAttribute(const std::string& nameLower) const
    {
        auto it = Attributes.find(nameLower);
        return it != Attributes.end() ? &it->second : nullptr;
    }
};

} // namespace GameEngine

