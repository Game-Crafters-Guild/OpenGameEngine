#pragma once

#include <string>
#include <string_view>
#include <typeinfo>
#include "Types/StringId.h"
#include "Types/StringUtils.h"
#include "UI/UIElement.h"
#include "UI/Registration/ElementRegistration.h"

namespace GameEngine
{
class UIElement;

// Internal-only attribute access for parsers, hot reload, and selector matching.
// This header is intentionally kept in Source/ to avoid exposing it as public API.
// Attribute names are normalized to lowercase for case-insensitive matching.
struct UIAttributeAccess
{
    static void SetAuthoredAttribute(UIElement& el, std::string_view name, std::string_view value, bool markDirty = false);
    static const std::string* FindAuthoredAttribute(const UIElement& el, std::string_view name);
    static const UIElement::AttributeMap& GetAuthoredAttributes(const UIElement& el);
    static void SetInlineStyleAttribute(UIElement& el, std::string_view inlineStyle);
    static void SetSelectorAttribute(UIElement& el, std::string_view name, std::string_view value, bool markDirty = true);
    // Stamp the canonical tag identity. ElementFactoryRegistry::Create is the normal caller;
    // it is the only site that knows which tag an element is being created as.
    static void SetCreatedTagId(UIElement& el, StringId tagId) { el.m_TagId = tagId; }

    // Stamp identity AND display name from a tag the registry cannot name for itself: an
    // unknown XML tag, or a proxy for a type registered from outside C++. Both are elements
    // whose tag is real and authored even though no C++ class carries it.
    static void SetCreatedTag(UIElement& el, std::string_view displayTag)
    {
        el.m_TagName = std::string(displayTag);
        el.m_TagId = UIRegistration::ElementFactoryRegistry::Instance().CanonicalTagId(
            ToLowerAscii(displayTag));
    }

    // Human-readable type name for debug logging and XML export.
    // Prefers m_TagName (carried by elements the registry cannot name) over factory lookup.
    static std::string GetDebugTypeName(const UIElement& el)
    {
        if (!el.m_TagName.empty())
            return el.m_TagName;
        std::string tag = UIRegistration::ElementFactoryRegistry::Instance().GetTagForType(typeid(el));
        if (!tag.empty())
            return tag;
        return "UIElement";
    }
};
} // namespace GameEngine

