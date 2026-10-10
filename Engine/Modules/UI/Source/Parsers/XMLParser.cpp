#include "UI/Parsers/XMLParser.h"
#include "Rendering/Common/Utils.h"
#include "UI/Registration/ElementRegistration.h"
#include "Types/StringUtils.h"
#include "../UIAttributeAccess.h"
#include "Logger/Logger.h"

#if defined(GE_HAVE_PUGIXML) && GE_HAVE_PUGIXML
    #include <pugixml.hpp>
#endif

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>



namespace GameEngine { namespace UIParsing {

namespace {

thread_local const std::string* g_XmlTextPtr = nullptr;
thread_local const std::string* g_XmlSourceName = nullptr;
inline int ComputeLineNumber(const std::string& text, size_t offset)
{
    int ln = 1;
    size_t n = std::min<size_t>(offset, text.size());
    for (size_t i = 0; i < n; ++i) { if (text[i] == '\n') ++ln; }
    return ln;
}
static std::unordered_set<std::string> s_WarnedUnknownTags;

#if defined(GE_HAVE_PUGIXML) && GE_HAVE_PUGIXML
static std::unique_ptr<UIElement> BuildTreePugi(const pugi::xml_node& node) {
    using namespace UIRegistration;
    if (!node) return nullptr;
    std::string tag = node.name();
    std::string tagLower = ToLowerAscii(tag);

    // Create via factory if available; fallback to generic UIElement (preserve original case)
    std::unique_ptr<UIElement> out = ElementFactoryRegistry::Instance().Create(tagLower);
    if (!out) {
        // Emit one-time warning with file:line if available
        int line = 0;
        std::string source = g_XmlSourceName ? *g_XmlSourceName : std::string("(string)");
        ptrdiff_t o = node.offset_debug();
        if (o >= 0 && g_XmlTextPtr) {
            line = ComputeLineNumber(*g_XmlTextPtr, static_cast<size_t>(o));
        }
        std::string key = source + ":" + std::to_string(line) + ":" + tag;
        if (s_WarnedUnknownTags.insert(key).second) {
            if (line > 0)
                Logger::Log::Warning("UI XML: Unknown element tag '{}' in '{}' at line {} — using base UIElement", tag, source, line);
            else
                Logger::Log::Warning("UI XML: Unknown element tag '{}' in '{}' — using base UIElement", tag, source);
        }
        out.reset(new UIElement());
        // Still stamped with the authored tag: an unknown tag is an identity even without a
        // factory, so the element reconciles against its own tag on a hot-reload instead of
        // being rebuilt, and the log/inspector can name it.
        UIAttributeAccess::SetCreatedTag(*out, tag);
    }

    // attributes: id/name, class, and any others (preserve on element; also build lower-case map for binder)
    std::string idAttr, classAttr;
    std::unordered_map<std::string, std::string> attrsLower;
    for (auto attr : node.attributes()) {
        std::string name = attr.name();
        std::string value = attr.value();
        UIAttributeAccess::SetAuthoredAttribute(*out, name, value, false);
        std::string nLower = ToLowerAscii(name);
        attrsLower[nLower] = value;
        if (nLower == "id")           idAttr = value;
        else if (nLower == "name" && idAttr.empty()) idAttr = value; // alias: name -> id
        else if (nLower == "class")   classAttr = value;
        else if (nLower == "style")
        {
            if (value.find(':') != std::string::npos)
                UIAttributeAccess::SetInlineStyleAttribute(*out, value);
        }
    }
    if (!idAttr.empty()) out->SetId(idAttr);
    // Split classes on spaces
    {
        size_t start = 0;
        while (start < classAttr.size()) {
            size_t end = classAttr.find(' ', start);
            std::string cls = classAttr.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
            if (!cls.empty()) out->AddClass(cls);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    // Collect inner text (pcdata & cdata)
    std::string innerText;
    for (auto child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            innerText += child.value();
        }
    }

    // Apply registered attribute bindings (includes defaults and Text())
    ElementFactoryRegistry::Instance().ApplyAttributes(*out, attrsLower, innerText);

    // Recurse for child elements
    for (auto child : node.children()) {
        if (child.type() == pugi::node_element) {
            if (auto c = BuildTreePugi(child)) out->AddChild(std::move(c));
        }
    }
    return out;
}

static std::string TrimCopy(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Template parse: construct a lightweight UITemplateNode tree. No UIElement
// instances are created — just plain data carriers. Tags are canonicalized to
// the registry's canonical lowercase form, and inner text is stored as a "text"
// attribute (unless an explicit text attribute already exists).
static std::unique_ptr<UITemplateNode> BuildTreePugiTemplate(const pugi::xml_node& node)
{
    using namespace UIRegistration;
    if (!node)
        return nullptr;

    const std::string rawTag = node.name();
    const std::string tagLower = ToLowerAscii(rawTag);
    ElementFactoryRegistry& reg = ElementFactoryRegistry::Instance();

    auto out = std::make_unique<UITemplateNode>();
    out->TagName = reg.CanonicalTagLower(tagLower);

    // Parse XML attributes into the template node.
    std::string classAttr;
    bool hasExplicitTextAttr = false;
    for (auto attr : node.attributes())
    {
        std::string name = attr.name();
        std::string value = attr.value();
        const std::string nLower = ToLowerAscii(name);

        // Store every attribute (lowercase key).
        out->Attributes[nLower] = value;

        if (nLower == "id")
            out->Id = value;
        else if (nLower == "name" && out->Id.empty())
            out->Id = value; // alias: name -> id
        else if (nLower == "class")
            classAttr = value;
        else if (nLower == "style")
        {
            if (value.find(':') != std::string::npos)
                out->InlineStyle = value;
        }
        else if (nLower == "text")
            hasExplicitTextAttr = true;
    }

    // Split classes on whitespace.
    {
        size_t start = 0;
        while (start < classAttr.size())
        {
            while (start < classAttr.size() && std::isspace(static_cast<unsigned char>(classAttr[start])))
                ++start;
            if (start >= classAttr.size())
                break;
            size_t end = start;
            while (end < classAttr.size() && !std::isspace(static_cast<unsigned char>(classAttr[end])))
                ++end;
            std::string cls = classAttr.substr(start, end - start);
            if (!cls.empty())
                out->Classes.push_back(std::move(cls));
            start = end;
        }
    }

    // Collect inner text (pcdata & cdata) and store as a "text" attribute when present.
    std::string innerText;
    for (auto child : node.children())
    {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
            innerText += child.value();
    }
    innerText = TrimCopy(innerText);
    if (!innerText.empty() && !hasExplicitTextAttr)
        out->Attributes["text"] = innerText;

    // Best-effort unknown tag warning (one-time per source:line:tag).
    if (!reg.HasFactory(out->TagName))
    {
        int line = 0;
        std::string source = g_XmlSourceName ? *g_XmlSourceName : std::string("(string)");
        ptrdiff_t o = node.offset_debug();
        if (o >= 0 && g_XmlTextPtr)
            line = ComputeLineNumber(*g_XmlTextPtr, static_cast<size_t>(o));
        std::string key = source + ":" + std::to_string(line) + ":" + rawTag;
        if (s_WarnedUnknownTags.insert(key).second)
        {
            if (line > 0)
                Logger::Log::Warning("UI XML (template): Unknown element tag '{}' in '{}' at line {} — will instantiate as base UIElement", rawTag, source, line);
            else
                Logger::Log::Warning("UI XML (template): Unknown element tag '{}' in '{}' — will instantiate as base UIElement", rawTag, source);
        }
    }

    // Recurse for child elements.
    for (auto child : node.children())
    {
        if (child.type() == pugi::node_element)
        {
            if (auto c = BuildTreePugiTemplate(child))
                out->Children.push_back(std::move(c));
        }
    }

    return out;
}
#endif
} // anonymous namespace



bool XMLParser::ParseLayoutFromString(const std::string& xmlText,
                                      std::unique_ptr<UIElement>& outRoot) {
#if defined(GE_HAVE_PUGIXML) && GE_HAVE_PUGIXML
    pugi::xml_document doc;
    pugi::xml_parse_result ok = doc.load_string(xmlText.c_str());
    if (!ok) return false;
    pugi::xml_node root = doc.document_element();
    if (!root) return false;
    const std::string defaultSource = "(string)";
    const std::string* prevText = g_XmlTextPtr;
    const std::string* prevSource = g_XmlSourceName;
    g_XmlTextPtr = &xmlText;
    g_XmlSourceName = &defaultSource;
    outRoot = BuildTreePugi(root);
    g_XmlTextPtr = prevText;
    g_XmlSourceName = prevSource;
    return (bool)outRoot;
#else
    // Fallback: construct a single root element tagged "UI" when pugixml is unavailable
    outRoot.reset(new UIElement());
    return true;
#endif
}

bool XMLParser::ParseLayoutFromString(const std::string& xmlText,
                                      std::unique_ptr<UIElement>& outRoot,
                                      const std::string& sourceName) {
#if defined(GE_HAVE_PUGIXML) && GE_HAVE_PUGIXML
    pugi::xml_document doc;
    pugi::xml_parse_result ok = doc.load_string(xmlText.c_str());
    if (!ok) return false;
    pugi::xml_node root = doc.document_element();
    if (!root) return false;
    const std::string* prevText = g_XmlTextPtr;
    const std::string* prevSource = g_XmlSourceName;
    g_XmlTextPtr = &xmlText;
    g_XmlSourceName = &sourceName;
    outRoot = BuildTreePugi(root);
    g_XmlTextPtr = prevText;
    g_XmlSourceName = prevSource;
    return (bool)outRoot;
#else
    // Fallback: construct a single root element tagged "UI" when pugixml is unavailable
    outRoot.reset(new UIElement());
    return true;
#endif
}

bool XMLParser::ParseLayoutFromFile(const std::string& path,
                                    std::unique_ptr<UIElement>& outRoot) {
    // Read all
    std::ifstream in(path);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    return ParseLayoutFromString(text, outRoot, path);
}

bool XMLParser::ParseLayoutTemplateFromString(const std::string& xmlText,
                                              std::unique_ptr<UITemplateNode>& outRoot)
{
    const std::string defaultSource = "(string)";
    return ParseLayoutTemplateFromString(xmlText, outRoot, defaultSource);
}

bool XMLParser::ParseLayoutTemplateFromString(const std::string& xmlText,
                                              std::unique_ptr<UITemplateNode>& outRoot,
                                              const std::string& sourceName)
{
#if defined(GE_HAVE_PUGIXML) && GE_HAVE_PUGIXML
    pugi::xml_document doc;
    pugi::xml_parse_result ok = doc.load_string(xmlText.c_str());
    if (!ok)
        return false;
    pugi::xml_node root = doc.document_element();
    if (!root)
        return false;
    const std::string* prevText = g_XmlTextPtr;
    const std::string* prevSource = g_XmlSourceName;
    g_XmlTextPtr = &xmlText;
    g_XmlSourceName = &sourceName;
    outRoot = BuildTreePugiTemplate(root);
    g_XmlTextPtr = prevText;
    g_XmlSourceName = prevSource;
    return (bool)outRoot;
#else
    outRoot = std::make_unique<UITemplateNode>();
    outRoot->tagName = "uielement";
    (void)xmlText;
    (void)sourceName;
    return true;
#endif
}

bool XMLParser::ParseLayoutTemplateFromFile(const std::string& path,
                                            std::unique_ptr<UITemplateNode>& outRoot)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    return ParseLayoutTemplateFromString(text, outRoot, path);
}

}} // namespace GameEngine::UIParsing

