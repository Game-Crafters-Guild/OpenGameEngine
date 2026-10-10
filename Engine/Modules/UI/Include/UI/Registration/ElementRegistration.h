#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <type_traits>
#include <cmath>
#include <sstream>

#include "Types/StringId.h"
#include "Types/StringUtils.h"
#include "UI/UIElement.h"

namespace GameEngine { namespace UIRegistration {

// Generic Parse<T> with specializations for common types

template<typename T>
struct Parser {
    static T Parse(std::string_view s);
};

template<> inline bool Parser<bool>::Parse(std::string_view s) {
    std::string v = ToLowerAscii(s);
    return (v == "1" || v == "true" || v == "yes" || v == "on");
}

template<> inline int Parser<int>::Parse(std::string_view s) {
    return std::stoi(std::string(s));
}

template<> inline float Parser<float>::Parse(std::string_view s) {
    return std::stof(std::string(s));
}

template<> inline std::string Parser<std::string>::Parse(std::string_view s) {
    return std::string(s);
}

// Colors as uint32_t ARGB
// Accepts: named (subset), #RGB/#RRGGBB/#RRGGBBAA, 0xAARRGGBB, decimal ARGB, rgb()/rgba(), hsl()/hsla()
template<> inline uint32_t Parser<uint32_t>::Parse(std::string_view s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
    };
    auto clamp255 = [](int x)->int{ return x < 0 ? 0 : (x > 255 ? 255 : x); };
    auto toARGB = [&](int r,int g,int b,int a)->uint32_t{
        r = clamp255(r); g = clamp255(g); b = clamp255(b); a = clamp255(a);
        return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    };

    std::string v(s);
    auto lower = ToLowerAscii(v);

    // rgb/rgba
    auto parseFloat = [](const std::string& t)->std::optional<float>{
        try { return std::stof(t); } catch(...) { return std::nullopt; }
    };
    auto parseChannel = [&](const std::string& t)->std::optional<int>{
        if (!t.empty() && t.back()=='%') {
            auto f = parseFloat(t.substr(0, t.size()-1)); if (!f) return std::nullopt; float cl = std::max(0.0f, std::min(100.0f, *f)); return (int)std::lround(cl * 2.55f);
        }
        auto f = parseFloat(t); if (!f) return std::nullopt; float cl = std::max(0.0f, std::min(255.0f, *f)); return (int)std::lround(cl);
    };
    auto parseAlpha = [&](const std::string& t)->std::optional<int>{
        if (!t.empty() && t.back()=='%') { auto f = parseFloat(t.substr(0, t.size()-1)); if (!f) return std::nullopt; float cl = std::max(0.0f, std::min(100.0f, *f)); return (int)std::lround(cl * 2.55f); }
        auto f = parseFloat(t); if (!f) return std::nullopt; float cl = std::max(0.0f, std::min(1.0f, *f)); return (int)std::lround(cl * 255.0f);
    };
    auto splitArgs = [](std::string inner)->std::vector<std::string>{ for(char& c:inner){ if(c=='/') c=','; } std::vector<std::string> out; size_t start=0; bool any=false; for(size_t i=0;i<inner.size();++i){ if(inner[i]==','){ out.push_back(std::string(inner.begin()+start, inner.begin()+i)); start=i+1; any=true; } } out.push_back(inner.substr(start)); if(!any){ std::istringstream ss(inner); std::vector<std::string> w; std::string tok; while(ss>>tok) w.push_back(tok); return w; } for (auto& s: out){ size_t a=0; while(a<s.size() && (s[a]==' '||s[a]=='\t')) ++a; size_t b=s.size(); while(b> a && (s[b-1]==' '||s[b-1]=='\t')) --b; s = s.substr(a,b-a);} return out; };
    if (lower.rfind("rgb(",0)==0 || lower.rfind("rgba(",0)==0) {
        size_t l = lower.find('('), rparen = lower.rfind(')'); if (l!=std::string::npos && rparen!=std::string::npos && rparen>l) {
            auto args = splitArgs(lower.substr(l+1, rparen-l-1));
            if (args.size()>=3) {
                auto R = parseChannel(args[0]); auto G = parseChannel(args[1]); auto B = parseChannel(args[2]); int A = 255; if (args.size()>=4) { auto a = parseAlpha(args[3]); if (a) A = *a; }
                if (R && G && B) return toARGB(*R,*G,*B,A);
            }
        }
    }

    // hsl/hsla
    if (lower.rfind("hsl(",0)==0 || lower.rfind("hsla(",0)==0) {
        size_t l = lower.find('('), rparen = lower.rfind(')'); if (l!=std::string::npos && rparen!=std::string::npos && rparen>l) {
            auto args = splitArgs(lower.substr(l+1, rparen-l-1));
            if (args.size()>=3) {
                auto Hf = parseFloat(args[0]); if (!Hf) Hf = 0.0f; float H = std::fmod(std::max(0.0f,*Hf), 360.0f) / 360.0f;
                auto Sp = args[1]; auto Lp = args[2]; if (!Sp.empty() && Sp.back()=='%') Sp.pop_back(); if (!Lp.empty() && Lp.back()=='%') Lp.pop_back();
                auto Sopt = parseFloat(Sp); auto Lopt = parseFloat(Lp);
                if (Sopt && Lopt) {
                    float S = std::max(0.0f, std::min(1.0f, *Sopt/100.0f)); float L = std::max(0.0f, std::min(1.0f, *Lopt/100.0f));
                    float A = 1.0f; if (args.size()>=4) { auto a = parseAlpha(args[3]); if (a) A = (*a)/255.0f; }
                    auto hue2rgb = [](float p,float q,float t){ if(t<0) t+=1; if(t>1) t-=1; if(t<1.0f/6) return p+(q-p)*6*t; if(t<1.0f/2) return q; if(t<2.0f/3) return p+(q-p)*(2.0f/3 - t)*6; return p; };
                    float rf,gf,bf; if (S==0){ rf=gf=bf=L; } else { float q = L < 0.5f ? L * (1+S) : L + S - L*S; float p = 2*L - q; rf=hue2rgb(p,q,H+1.0f/3); gf=hue2rgb(p,q,H); bf=hue2rgb(p,q,H-1.0f/3); }
                    return toARGB((int)std::lround(rf*255.0f),(int)std::lround(gf*255.0f),(int)std::lround(bf*255.0f),(int)std::lround(A*255.0f));
                }
            }
        }
    }

    // Hex formats
    if (!v.empty() && v[0] == '#') {
        if (v.size() == 4) { uint32_t r8 = (hex(v[1]) << 4) | hex(v[1]); uint32_t g8 = (hex(v[2]) << 4) | hex(v[2]); uint32_t b8 = (hex(v[3]) << 4) | hex(v[3]); return (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8; }
        if (v.size() == 7) { uint32_t r8 = (hex(v[1]) << 4) | hex(v[2]); uint32_t g8 = (hex(v[3]) << 4) | hex(v[4]); uint32_t b8 = (hex(v[5]) << 4) | hex(v[6]); return (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8; }
        if (v.size() == 9) { uint32_t r8 = (hex(v[1]) << 4) | hex(v[2]); uint32_t g8 = (hex(v[3]) << 4) | hex(v[4]); uint32_t b8 = (hex(v[5]) << 4) | hex(v[6]); uint32_t a8 = (hex(v[7]) << 4) | hex(v[8]); return (a8 << 24) | (r8 << 16) | (g8 << 8) | b8; }
    }

    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        return static_cast<uint32_t>(std::stoul(v, nullptr, 16));
    }
    try {
        return static_cast<uint32_t>(std::stoul(v, nullptr, 10));
    } catch (...) {
        // Full CSS Level 4 named colors (ARGB) + transparent
        static std::unordered_map<std::string,uint32_t> m;
        if (m.empty()) {
            m["black"]=0xFF000000u; m["silver"]=0xFFC0C0C0u; m["gray"]=0xFF808080u; m["white"]=0xFFFFFFFFu; m["maroon"]=0xFF800000u; m["red"]=0xFFFF0000u; m["purple"]=0xFF800080u; m["fuchsia"]=0xFFFF00FFu;
            m["green"]=0xFF008000u; m["lime"]=0xFF00FF00u; m["olive"]=0xFF808000u; m["yellow"]=0xFFFFFF00u; m["navy"]=0xFF000080u; m["blue"]=0xFF0000FFu; m["teal"]=0xFF008080u; m["aqua"]=0xFF00FFFFu;
            m["orange"]=0xFFFFA500u; m["aliceblue"]=0xFFF0F8FFu; m["antiquewhite"]=0xFFFAEBD7u; m["aquamarine"]=0xFF7FFFD4u; m["azure"]=0xFFF0FFFFu; m["beige"]=0xFFF5F5DCu; m["bisque"]=0xFFFFE4C4u; m["blanchedalmond"]=0xFFFFEBCDu;
            m["blueviolet"]=0xFF8A2BE2u; m["brown"]=0xFFA52A2Au; m["burlywood"]=0xFFDEB887u; m["cadetblue"]=0xFF5F9EA0u; m["chartreuse"]=0xFF7FFF00u; m["chocolate"]=0xFFD2691Eu; m["coral"]=0xFFFF7F50u; m["cornflowerblue"]=0xFF6495EDu;
            m["cornsilk"]=0xFFFFF8DCu; m["crimson"]=0xFFDC143Cu; m["cyan"]=0xFF00FFFFu; m["darkblue"]=0xFF00008Bu; m["darkcyan"]=0xFF008B8Bu; m["darkgoldenrod"]=0xFFB8860Bu; m["darkgray"]=0xFFA9A9A9u; m["darkgreen"]=0xFF006400u;
            m["darkgrey"]=0xFFA9A9A9u; m["darkkhaki"]=0xFFBDB76Bu; m["darkmagenta"]=0xFF8B008Bu; m["darkolivegreen"]=0xFF556B2Fu; m["darkorange"]=0xFFFF8C00u; m["darkorchid"]=0xFF9932CCu; m["darkred"]=0xFF8B0000u; m["darksalmon"]=0xFFE9967Au;
            m["darkseagreen"]=0xFF8FBC8Fu; m["darkslateblue"]=0xFF483D8Bu; m["darkslategray"]=0xFF2F4F4Fu; m["darkslategrey"]=0xFF2F4F4Fu; m["darkturquoise"]=0xFF00CED1u; m["darkviolet"]=0xFF9400D3u; m["deeppink"]=0xFFFF1493u; m["deepskyblue"]=0xFF00BFFFu;
            m["dimgray"]=0xFF696969u; m["dimgrey"]=0xFF696969u; m["dodgerblue"]=0xFF1E90FFu; m["firebrick"]=0xFFB22222u; m["floralwhite"]=0xFFFFFAF0u; m["forestgreen"]=0xFF228B22u; m["gainsboro"]=0xFFDCDCDCu; m["ghostwhite"]=0xFFF8F8FFu;
            m["gold"]=0xFFFFD700u; m["goldenrod"]=0xFFDAA520u; m["greenyellow"]=0xFFADFF2Fu; m["honeydew"]=0xFFF0FFF0u; m["hotpink"]=0xFFFF69B4u; m["indianred"]=0xFFCD5C5Cu; m["indigo"]=0xFF4B0082u; m["ivory"]=0xFFFFFFF0u;
            m["khaki"]=0xFFF0E68Cu; m["lavender"]=0xFFE6E6FAu; m["lavenderblush"]=0xFFFFF0F5u; m["lawngreen"]=0xFF7CFC00u; m["lemonchiffon"]=0xFFFFFACDu; m["lightblue"]=0xFFADD8E6u; m["lightcoral"]=0xFFF08080u; m["lightcyan"]=0xFFE0FFFFu;
            m["lightgoldenrodyellow"]=0xFFFAFAD2u; m["lightgray"]=0xFFD3D3D3u; m["lightgreen"]=0xFF90EE90u; m["lightgrey"]=0xFFD3D3D3u; m["lightpink"]=0xFFFFB6C1u; m["lightsalmon"]=0xFFFFA07Au; m["lightseagreen"]=0xFF20B2AAu; m["lightskyblue"]=0xFF87CEFAu;
            m["lightslategray"]=0xFF778899u; m["lightslategrey"]=0xFF778899u; m["lightsteelblue"]=0xFFB0C4DEu; m["lightyellow"]=0xFFFFFFE0u; m["limegreen"]=0xFF32CD32u; m["linen"]=0xFFFAF0E6u; m["magenta"]=0xFFFF00FFu; m["mediumaquamarine"]=0xFF66CDAAu;
            m["mediumblue"]=0xFF0000CDu; m["mediumorchid"]=0xFFBA55D3u; m["mediumpurple"]=0xFF9370DBu; m["mediumseagreen"]=0xFF3CB371u; m["mediumslateblue"]=0xFF7B68EEu; m["mediumspringgreen"]=0xFF00FA9Au; m["mediumturquoise"]=0xFF48D1CCu; m["mediumvioletred"]=0xFFC71585u;
            m["midnightblue"]=0xFF191970u; m["mintcream"]=0xFFF5FFFAu; m["mistyrose"]=0xFFFFE4E1u; m["moccasin"]=0xFFFFE4B5u; m["navajowhite"]=0xFFFFDEADu; m["navy"]=0xFF000080u; m["oldlace"]=0xFFFDF5E6u; m["olivedrab"]=0xFF6B8E23u;
            m["orange"]=0xFFFFA500u; m["orangered"]=0xFFFF4500u; m["orchid"]=0xFFDA70D6u; m["palegoldenrod"]=0xFFEEE8AAu; m["palegreen"]=0xFF98FB98u; m["paleturquoise"]=0xFFAFEEEEu; m["palevioletred"]=0xFFDB7093u; m["papayawhip"]=0xFFFFEFD5u;
            m["peachpuff"]=0xFFFFDAB9u; m["peru"]=0xFFCD853Fu; m["pink"]=0xFFFFC0CBu; m["plum"]=0xFFDDA0DDu; m["powderblue"]=0xFFB0E0E6u; m["rebeccapurple"]=0xFF663399u; m["rosybrown"]=0xFFBC8F8Fu; m["royalblue"]=0xFF4169E1u;
            m["saddlebrown"]=0xFF8B4513u; m["salmon"]=0xFFFA8072u; m["sandybrown"]=0xFFF4A460u; m["seagreen"]=0xFF2E8B57u; m["seashell"]=0xFFFFF5EEu; m["sienna"]=0xFFA0522Du; m["silver"]=0xFFC0C0C0u; m["skyblue"]=0xFF87CEEBu;
            m["slateblue"]=0xFF6A5ACDu; m["slategray"]=0xFF708090u; m["slategrey"]=0xFF708090u; m["snow"]=0xFFFFFAFAu; m["springgreen"]=0xFF00FF7Fu; m["steelblue"]=0xFF4682B4u; m["tan"]=0xFFD2B48Cu; m["teal"]=0xFF008080u;
            m["thistle"]=0xFFD8BFD8u; m["tomato"]=0xFFFF6347u; m["turquoise"]=0xFF40E0D0u; m["violet"]=0xFFEE82EEu; m["wheat"]=0xFFF5DEB3u; m["white"]=0xFFFFFFFFu; m["whitesmoke"]=0xFFF5F5F5u; m["yellow"]=0xFFFFFF00u;
            m["yellowgreen"]=0xFF9ACD32u; m["transparent"]=0x00000000u;
        }
        auto itColor = m.find(lower);
        if (itColor != m.end()) return itColor->second;
        return 0xFF000000u; // fallback to black for visibility
    }
}

// ToString helpers for defaults
inline std::string ToString(bool v) { return v ? "true" : "false"; }
inline std::string ToString(int v) { return std::to_string(v); }
inline std::string ToString(float v) { return std::to_string(v); }
inline std::string ToString(const std::string& v) { return v; }
inline std::string ToString(uint32_t v) { return std::to_string(v); }

// Attribute handler (type-erased)
struct AttrHandler {
    // NamesLower includes primary name and aliases (all lower-case)
    std::vector<std::string> NamesLower;
    // Apply with string value; closure will dynamic_cast to the target type
    std::function<void(UIElement&, std::string_view)> Apply;
    // Optional default application (string-based), executed when attribute missing
    std::function<void(UIElement&)> ApplyDefault;
    // If true, this handler binds inner text content instead of an attribute
    bool IsText = false;
    // Who registered this handler. 0 is the engine. A handler is keyed by attribute NAME and
    // applied to any element whose attributes match, so one left behind by an unloaded owner
    // would keep applying to elements of other types — hence it carries the same stamp its
    // factory does, and is swept with it.
    std::uint64_t Owner = 0;
};

// Global registry for element factories and attribute handlers
class ElementFactoryRegistry {
public:
    using FactoryFn = std::function<std::unique_ptr<UIElement>()>;

    static ElementFactoryRegistry& Instance();

    // Register a factory for a tag. Returns false and changes NOTHING if the tag already
    // belongs to a different owner.
    //
    // That refusal is what makes engine registrations unremovable, and it takes BOTH halves
    // to be true. Refusing owner 0 in the sweep is not enough on its own: without this check
    // a caller could take ownership of a built-in simply by registering under its name, and
    // then sweep it under its own owner id. So a tag's owner is fixed by whoever registered
    // it first, and only that owner may re-register it: re-declaring a tag you already hold
    // is legal, taking one you do not is refused.
    bool RegisterFactory(std::string_view tagLower, FactoryFn fn, const std::type_info& typeInfo,
                         std::uint64_t owner = 0);
    std::unique_ptr<UIElement> Create(std::string_view tagLower) const;

    // Remove every factory, alias and attribute handler registered by one owner, and return
    // how many FACTORY keys went (canonical tags plus their aliases).
    //
    // Refuses owner 0 and returns 0: see RegisterFactory. Elements already created by those
    // factories are untouched — they keep the tag id they were stamped with, which is what
    // lets them survive until their type comes back.
    std::size_t UnregisterFactoriesOwnedBy(std::uint64_t owner);

    // Factory keys currently registered by one owner (canonical tags plus aliases).
    std::size_t CountFactoriesOwnedBy(std::uint64_t owner) const;

    // Attribute handlers currently registered by one owner. Test seam: the handler channel is
    // keyed by attribute name rather than by type, so "the factories went" is not evidence
    // that the handlers went with them.
    std::size_t CountAttrHandlersOwnedBy(std::uint64_t owner) const;

    // Register an alias tag that maps to an existing factory
    void RegisterFactoryAlias(std::string_view aliasLower, std::string_view existingLower);

    // Query whether a factory exists for this tag (tagLower or any registered alias).
    bool HasFactory(std::string_view tagLower) const;

    // Who holds this tag, or nullopt if nothing does. An optional rather than a 0-means-nobody
    // number because 0 is a real answer: the engine. This is what a refused registration needs
    // to say WHO refused it, which the return code alone cannot.
    std::optional<std::uint64_t> OwnerOfTag(std::string_view tagLower) const;

    // Resolve an input tag to its canonical tag (lowercase). For aliases registered
    // via TagAlias/RegisterFactoryAlias, this returns the owning type's canonical tag.
    // If unknown, returns the lowercased input.
    std::string CanonicalTagLower(std::string_view tagLower) const;

    // Type identity: resolve a tag to its registered type_info.
    const std::type_info* GetTypeInfo(std::string_view tagLower) const;

    // Reverse lookup: get the canonical tag string for a C++ type (for debug/export).
    // Returns empty string if the type was never registered.
    std::string GetTagForType(const std::type_info& ti) const;

    // Check whether a live element is of the type registered for tagLower.
    //
    // Identity is the TAG, not the C++ class: an element created through Create carries its
    // canonical tag id, and that is what is compared. RTTI cannot answer this question,
    // because a family of types registered from outside C++ necessarily shares one C++
    // proxy class and would collapse to one type_info — reconcile would then reuse a
    // <MyFoo/> for a <MyBar/> slot.
    //
    // Comparison is against the tag's canonical HASH rather than against a registered id, so
    // an element whose factory has since been unregistered still matches its own tag. That is
    // what lets an orphaned element survive a reconcile instead of being replaced (and it is
    // also why an unknown tag now reconciles against itself rather than being rebuilt).
    bool IsSameType(const UIElement& el, std::string_view tagLower) const;

    // The element's tag identity: its creation stamp, or the id registered for its C++ type
    // when it carries none (an element constructed directly rather than through Create).
    // This is the value CSS type-selector matching compares against.
    StringId GetElementTagId(const UIElement& el) const;

    // Deterministic StringId for a REGISTERED tag name (resolves aliases). Returns 0 if no
    // factory is registered for it — callers that need the identity of a tag whether or not
    // it is currently registered want CanonicalTagId.
    StringId GetTagId(std::string_view tagLower) const;

    // Deterministic StringId for any tag name, registered or not (resolves aliases). This is
    // the element identity domain: Create stamps this value, and IsSameType compares it.
    StringId CanonicalTagId(std::string_view tagLower) const;

    // Reverse lookup: deterministic StringId for a C++ type. Returns 0 if unregistered.
    StringId GetTagIdForType(const std::type_info& ti) const;

    // Attribute binding registration and application
    void RegisterAttrHandler(const std::shared_ptr<AttrHandler>& handler);

    // Apply all matching attribute handlers to element
    // - attrsLower: attribute map with lower-case keys
    // - innerText: raw inner text content (if any)
    //
    // This APPLIES attributes; it does not AUTHOR them. Every element builder stamps the
    // authored values onto the element (UIAttributeAccess::SetAuthoredAttribute) before
    // calling this, and callers must keep doing so: an element carrying no attribute map is a
    // state no document produces, and code that replays an element's authored state later —
    // ManagedElementProxy re-materialization — reads that map rather than a private copy.
    void ApplyAttributes(UIElement& el,
                         const std::unordered_map<std::string, std::string>& attrsLower,
                         const std::string& innerText) const;

private:
    // The two lookups above, taking a key the caller has ALREADY lowered. The public forms
    // lower and then delegate; a caller that needs both — Create, IsSameType — lowers once and
    // calls these, instead of paying a fresh allocation per lookup for a string it is holding.
    StringId CanonicalTagIdFromKey(const std::string& key) const;
    const std::type_info* GetTypeInfoFromKey(const std::string& key) const;

    struct TypeInfoPtrHash {
        size_t operator()(const std::type_info* ti) const noexcept { return ti->hash_code(); }
    };
    struct TypeInfoPtrEqual {
        bool operator()(const std::type_info* a, const std::type_info* b) const noexcept { return *a == *b; }
    };

    std::unordered_map<std::string, FactoryFn> m_Factories; // tagLower -> factory
    // Who registered each tag key, canonical and alias alike. An alias inherits its
    // canonical's owner, so a sweep cannot leave half a registration behind.
    std::unordered_map<std::string, std::uint64_t> m_OwnerByTag;
    // Maps any known tag (canonical or alias) to its canonical tag.
    std::unordered_map<std::string, std::string> m_CanonicalByLower;
    // type_info per canonical tag (not aliases — aliases share the same type_info via canonical lookup).
    std::unordered_map<std::string, const std::type_info*> m_TypeInfoByTag;
    // Reverse: type_info -> display tag (PascalCase, as registered by Registrar).
    std::unordered_map<const std::type_info*, std::string, TypeInfoPtrHash, TypeInfoPtrEqual> m_TagByTypeInfo;
    // Reverse: type_info -> StringId (cached at registration, avoids re-hashing at runtime).
    std::unordered_map<const std::type_info*, StringId, TypeInfoPtrHash, TypeInfoPtrEqual> m_TagIdByTypeInfo;
    std::unordered_map<std::string, std::vector<std::shared_ptr<AttrHandler>>> m_HandlersByName; // nameLower -> handlers
    std::vector<std::shared_ptr<AttrHandler>> m_AllHandlers; // for defaults/text
};

// Fluent Registrar API ------------------------------------------------------

template<class T>
class Registrar; // fwd

template<class T>
class AttrChain {
public:
    AttrChain(Registrar<T>& owner, std::shared_ptr<AttrHandler> handler)
        : m_Owner(owner), m_Handler(std::move(handler)) {}

    // Add aliases for this attribute
    AttrChain& Alias(std::string_view alias) {
        m_Handler->NamesLower.push_back(ToLowerAscii(alias));
        return *this;
    }

    // Register an alias tag name for the owning control type. This forwards to
    // Registrar<T>::TagAlias so call sites can fluently chain:
    //   Register<T>("Foo").Attr(...).TagAlias("foo");
    AttrChain& TagAlias(std::string_view alias)
    {
        m_Owner.TagAlias(alias);
        return *this;
    }

    // Default value if attribute (or any alias) is missing (stringified)
    template<typename V>
    AttrChain& Default(V value) {
        auto ph = m_Handler; // keep shared

        std::string dv = ToString(value);
        m_Handler->ApplyDefault = [ph, dv](UIElement& el){ ph->Apply(el, dv); };
        return *this;
    }

    // Start a new attribute binding (for convenience chaining)
    template<class Member>
    AttrChain Attr(std::string_view name, Member member) {
        return m_Owner.Attr(name, member);
    }

private:
    Registrar<T>& m_Owner;
    std::shared_ptr<AttrHandler> m_Handler;
    template<class U> friend class Registrar;
};


template<class T>
class Registrar {
public:
    // owner 0 is the engine; see ElementFactoryRegistry::RegisterFactory for why that makes
    // built-in registrations unremovable. Every attribute binding this registrar declares
    // inherits the same owner, so a sweep takes the type and its attributes together.
    explicit Registrar(std::string_view tag, std::uint64_t owner = 0)
        : m_TagLower(ToLowerAscii(tag)), m_Owner(owner) {
        ElementFactoryRegistry::Instance().RegisterFactory(m_TagLower, [](){ return std::make_unique<T>(); }, typeid(T), m_Owner);
    }

    // Alternate constructor allowing custom factory for non-default-constructible types
    template<class Factory>
    explicit Registrar(std::string_view tag, Factory factory, std::uint64_t owner = 0)
        : m_TagLower(ToLowerAscii(tag)), m_Owner(owner) {
        ElementFactoryRegistry::Instance().RegisterFactory(m_TagLower, [factory](){ return factory(); }, typeid(T), m_Owner);
    }


    // Bind attribute to a setter: void (Owner::*)(const V&)
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, void (Owner::*setter)(const V&)) {
        static_assert(std::is_base_of_v<Owner, T> || std::is_same_v<Owner, T>, "Owner must be T or a base of T");
        auto h = std::make_shared<AttrHandler>();
        h->NamesLower.push_back(ToLowerAscii(name));
        h->Apply = [setter](UIElement& el, std::string_view s){
            if (auto* p = dynamic_cast<T*>(&el)) {
                auto* b = static_cast<Owner*>(p);
                (b->*setter)( Parser<V>::Parse(s) );
            }
        };
        h->Owner = m_Owner;
        ElementFactoryRegistry::Instance().RegisterAttrHandler(h);
        return AttrChain<T>(*this, std::move(h));
    }

    // Bind attribute to a setter with inline default
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, void (Owner::*setter)(const V&), const V& defaultValue) {
        static_assert(std::is_base_of_v<Owner, T> || std::is_same_v<Owner, T>, "Owner must be T or a base of T");
        auto chain = Attr<Owner, V>(name, setter);
        chain.Default(defaultValue);
        return chain;
    }

    // Bind attribute to a field with inline default
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, V Owner::*field, const V& defaultValue) {
        static_assert(std::is_base_of_v<Owner, T> || std::is_same_v<Owner, T>, "Owner must be T or a base of T");
        auto chain = Attr<Owner, V>(name, field);
        chain.Default(defaultValue);
        return chain;
    }

    // Bind attribute directly to a field: V Owner::*
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, V Owner::*field) {
        static_assert(std::is_base_of_v<Owner, T> || std::is_same_v<Owner, T>, "Owner must be T or a base of T");
        auto h = std::make_shared<AttrHandler>();
        h->NamesLower.push_back(ToLowerAscii(name));
        h->Apply = [field](UIElement& el, std::string_view s){
            if (auto* p = dynamic_cast<T*>(&el)) {
                auto* b = static_cast<Owner*>(p);
                b->*field = Parser<V>::Parse(s);
                el.MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            }
        };
        h->Owner = m_Owner;
        ElementFactoryRegistry::Instance().RegisterAttrHandler(h);

        return AttrChain<T>(*this, std::move(h));
    }

    // Bind attribute to a setter by value: void (Owner::*)(V)
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, void (Owner::*setter)(V)) {
        static_assert(std::is_base_of_v<Owner, T> || std::is_same_v<Owner, T>, "Owner must be T or a base of T");
        auto h = std::make_shared<AttrHandler>();
        h->NamesLower.push_back(ToLowerAscii(name));
        h->Apply = [setter](UIElement& el, std::string_view s){
            if (auto* p = dynamic_cast<T*>(&el)) {
                auto* b = static_cast<Owner*>(p);
                (b->*setter)( Parser<V>::Parse(s) );
            }
        };
        h->Owner = m_Owner;
        ElementFactoryRegistry::Instance().RegisterAttrHandler(h);
        return AttrChain<T>(*this, std::move(h));
    }

    // Bind attribute to a setter by value with inline default
    template<class Owner, class V>
    AttrChain<T> Attr(std::string_view name, void (Owner::*setter)(V), const V& defaultValue) {
        auto chain = Attr<Owner, V>(name, setter);
        chain.Default(defaultValue);
        return chain;
    }


    // Bind inner text content to a setter: void (T::*)(const std::string&)
    AttrChain<T> Text(void (T::*setter)(const std::string&)) {
        auto h = std::make_shared<AttrHandler>();
        h->IsText = true;
        h->Apply = [setter](UIElement& el, std::string_view s){
            if (auto* p = dynamic_cast<T*>(&el)) {
                (p->*setter)(std::string(s));
            }
        };
        h->Owner = m_Owner;
        ElementFactoryRegistry::Instance().RegisterAttrHandler(h);
        return AttrChain<T>(*this, std::move(h));
    }

    // Register an alias tag name for this control type
    Registrar<T>& TagAlias(std::string_view alias) {
        ElementFactoryRegistry::Instance().RegisterFactoryAlias(ToLowerAscii(alias), m_TagLower);
        return *this;
    }



    const std::string& TagLower() const { return m_TagLower; }

private:
    std::string m_TagLower;
    std::uint64_t m_Owner = 0;
};

// Entry point for users

template<class T>
inline Registrar<T> Register(std::string_view tag) { return Registrar<T>(tag); }

// Register a type on behalf of an owner that can later be swept wholesale
// (ElementFactoryRegistry::UnregisterFactoriesOwnedBy). owner must be non-zero.
template<class T>
inline Registrar<T> RegisterOwned(std::string_view tag, std::uint64_t owner) { return Registrar<T>(tag, owner); }

// Helper for registering with a custom factory
template<class T, class Factory>
inline Registrar<T> RegisterWithFactory(std::string_view tag, Factory factory) {
    return Registrar<T>(tag, factory);
}



// The single entry point that defines the built-in tag set, and the only place those chains
// live. The FIRST call defines it and later calls do nothing; this module's dynamic
// initialisation normally makes that first call, but an earlier-ordered initialiser in
// another TU would, so no caller can assume which one it is — call it if you need the tags.
void RegisterBuiltInControls();

// How many times the tag set has been DEFINED. Test seam, and the only observable that a
// second definition moves: RegisterFactory assigns by key, so re-running the whole definition
// overwrites every entry with an equal one and leaves every registry count identical.
std::size_t BuiltInControlDefinitionCount();

}} // namespace GameEngine::UIRegistration

