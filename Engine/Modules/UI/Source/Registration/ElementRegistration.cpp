#include "UI/Registration/ElementRegistration.h"
#include "../UIAttributeAccess.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include <algorithm>
#include <iterator>
#include <vector>

namespace GameEngine { namespace UIRegistration {

static ElementFactoryRegistry* g_registry = nullptr;

ElementFactoryRegistry& ElementFactoryRegistry::Instance() {
    if (!g_registry) g_registry = new ElementFactoryRegistry();
    return *g_registry;
}

bool ElementFactoryRegistry::RegisterFactory(std::string_view tagLower, FactoryFn fn, const std::type_info& typeInfo,
                                             std::uint64_t owner) {
    const std::string key = ToLowerAscii(tagLower);
    // A tag's owner is fixed by whoever registered it first. Re-registration by the SAME
    // owner stays legal — re-declaring a tag you already hold is not a takeover — but a
    // different owner may not take the tag over, or it could claim a built-in and then sweep
    // it under its own id. This is the half of the unremovability property that lives at
    // registration; the other half is UnregisterFactoriesOwnedBy refusing owner 0.
    auto itOwner = m_OwnerByTag.find(key);
    if (itOwner != m_OwnerByTag.end() && itOwner->second != owner)
    {
        Logger::Log::Warning(
            "UI: refusing to register element tag '{}' for owner {} — it is already registered by "
            "owner {}. The existing registration is unchanged.",
            key, owner, itOwner->second);
        return false;
    }
    m_Factories[key] = std::move(fn);
    m_OwnerByTag[key] = owner;
    // Canonical mapping: a canonical tag maps to itself.
    m_CanonicalByLower[key] = key;
    m_TypeInfoByTag[key] = &typeInfo;
    // Reverse maps: the RTTI answer for elements that carry no creation stamp, i.e. ones
    // built directly in C++ rather than through Create. They are lossy by construction —
    // registering two tags against one C++ class leaves whichever registered last — which is
    // exactly why element identity lives on the element (m_TagId) and not here.
    m_TagByTypeInfo[&typeInfo] = key;
    m_TagIdByTypeInfo[&typeInfo] = HashStringId(key);
    return true;
}

std::unique_ptr<UIElement> ElementFactoryRegistry::Create(std::string_view tagLower) const {
    const std::string key = ToLowerAscii(tagLower);
    auto it = m_Factories.find(key);
    if (it == m_Factories.end()) return nullptr;
    std::unique_ptr<UIElement> el = (it->second)();
    // The one place that knows which TAG an element is being created as. An alias is stamped
    // with its canonical tag, so <pane> and <weightedpane> are one identity.
    if (el)
        UIAttributeAccess::SetCreatedTagId(*el, CanonicalTagIdFromKey(key));
    return el;
}

void ElementFactoryRegistry::RegisterFactoryAlias(std::string_view aliasLower, std::string_view existingLower) {
    const std::string existingKey = ToLowerAscii(existingLower);
    auto it = m_Factories.find(existingKey);
    if (it != m_Factories.end()) {
        const std::string aliasKey = ToLowerAscii(aliasLower);
        auto itExistingOwner = m_OwnerByTag.find(existingKey);
        const std::uint64_t aliasOwner = (itExistingOwner != m_OwnerByTag.end()) ? itExistingOwner->second : 0;
        // An alias is a registration like any other, so it cannot take a tag away from another
        // owner — otherwise the refusal above would be bypassable by aliasing onto "button".
        auto itAliasOwner = m_OwnerByTag.find(aliasKey);
        if (itAliasOwner != m_OwnerByTag.end() && itAliasOwner->second != aliasOwner)
        {
            Logger::Log::Warning(
                "UI: refusing to alias element tag '{}' onto '{}' — '{}' is already registered by "
                "owner {}.",
                aliasKey, existingKey, aliasKey, itAliasOwner->second);
            return;
        }
        m_Factories[aliasKey] = it->second;
        // An alias belongs to whoever owns the tag it aliases, so a sweep takes both.
        m_OwnerByTag[aliasKey] = aliasOwner;
        // Preserve canonical mapping for aliases.
        auto itCan = m_CanonicalByLower.find(existingKey);
        const std::string canonical = (itCan != m_CanonicalByLower.end()) ? itCan->second : existingKey;
        m_CanonicalByLower[aliasKey] = canonical;
        // Alias shares the same type_info as the canonical tag.
        auto itTi = m_TypeInfoByTag.find(canonical);
        if (itTi != m_TypeInfoByTag.end())
            m_TypeInfoByTag[aliasKey] = itTi->second;
    }
}

bool ElementFactoryRegistry::HasFactory(std::string_view tagLower) const
{
    const std::string key = ToLowerAscii(tagLower);
    return m_Factories.find(key) != m_Factories.end();
}

std::optional<std::uint64_t> ElementFactoryRegistry::OwnerOfTag(std::string_view tagLower) const
{
    auto it = m_OwnerByTag.find(ToLowerAscii(tagLower));
    if (it == m_OwnerByTag.end())
        return std::nullopt;
    return it->second;
}

std::string ElementFactoryRegistry::CanonicalTagLower(std::string_view tagLower) const
{
    const std::string key = ToLowerAscii(tagLower);
    auto it = m_CanonicalByLower.find(key);
    if (it != m_CanonicalByLower.end())
        return it->second;
    return key;
}

const std::type_info* ElementFactoryRegistry::GetTypeInfoFromKey(const std::string& key) const
{
    // Resolve through canonical mapping first so aliases work.
    auto itCan = m_CanonicalByLower.find(key);
    const std::string& lookup = (itCan != m_CanonicalByLower.end()) ? itCan->second : key;
    auto it = m_TypeInfoByTag.find(lookup);
    return (it != m_TypeInfoByTag.end()) ? it->second : nullptr;
}

const std::type_info* ElementFactoryRegistry::GetTypeInfo(std::string_view tagLower) const
{
    return GetTypeInfoFromKey(ToLowerAscii(tagLower));
}

std::string ElementFactoryRegistry::GetTagForType(const std::type_info& ti) const
{
    auto it = m_TagByTypeInfo.find(&ti);
    return (it != m_TagByTypeInfo.end()) ? it->second : std::string{};
}

bool ElementFactoryRegistry::IsSameType(const UIElement& el, std::string_view tagLower) const
{
    // Lowered ONCE: this runs per child per reconcile, and both lookups below need the key.
    const std::string key = ToLowerAscii(tagLower);
    const StringId expectedId = CanonicalTagIdFromKey(key);
    if (expectedId == 0)
        return false;
    // When the tag is currently REGISTERED, the element must also be of the registered C++
    // type. Without this, a plain UIElement left behind by a document parsed before its type
    // registered would match the tag forever, and the reconciler would keep adopting the
    // placeholder instead of ever building the real control.
    //
    // When the tag is NOT registered the stamp stands alone, which is what preserves an
    // orphan whose factory has gone and a genuinely unknown tag.
    const std::type_info* expectedType = GetTypeInfoFromKey(key);
    if (expectedType && typeid(el) != *expectedType)
        return false;
    // An element with no identity at all (unstamped AND its C++ type unregistered) reports 0,
    // which matches no tag — the same answer the RTTI comparison gave.
    return GetElementTagId(el) == expectedId;
}

StringId ElementFactoryRegistry::GetElementTagId(const UIElement& el) const
{
    const StringId stamped = el.GetTagId();
    if (stamped != 0)
        return stamped;
    return GetTagIdForType(typeid(el));
}

StringId ElementFactoryRegistry::GetTagId(std::string_view tagLower) const
{
    const std::string key = ToLowerAscii(tagLower);
    // Resolve through canonical mapping first so aliases work.
    auto itCan = m_CanonicalByLower.find(key);
    const std::string& lookup = (itCan != m_CanonicalByLower.end()) ? itCan->second : key;
    auto it = m_TypeInfoByTag.find(lookup);
    if (it == m_TypeInfoByTag.end())
        return 0;
    return HashStringId(lookup);
}

StringId ElementFactoryRegistry::CanonicalTagIdFromKey(const std::string& key) const
{
    // An empty tag names nothing, and FNV-1a("") is a perfectly ordinary non-zero value that
    // would otherwise become a real identity that real elements could be stamped with.
    if (key.empty())
        return 0;
    // Hashed straight out of the map so the canonical tag is never copied out to be hashed.
    auto it = m_CanonicalByLower.find(key);
    return HashStringId(it != m_CanonicalByLower.end() ? it->second : key);
}

StringId ElementFactoryRegistry::CanonicalTagId(std::string_view tagLower) const
{
    if (tagLower.empty())
        return 0;
    return CanonicalTagIdFromKey(ToLowerAscii(tagLower));
}

StringId ElementFactoryRegistry::GetTagIdForType(const std::type_info& ti) const
{
    auto it = m_TagIdByTypeInfo.find(&ti);
    return (it != m_TagIdByTypeInfo.end()) ? it->second : 0;
}

std::size_t ElementFactoryRegistry::UnregisterFactoriesOwnedBy(std::uint64_t owner)
{
    // Owner 0 is the engine, and refusing it here is HALF of the "engine registrations are
    // structurally unremovable" property: it closes the exit. The other half is
    // RegisterFactory refusing to change an existing tag's owner, which closes the entrance —
    // without it a caller could take a built-in over by registering under its name and then
    // sweep it under an owner id it is entitled to name. Neither half is sufficient alone.
    if (owner == 0)
        return 0;

    std::vector<std::string> doomed;
    for (const auto& kv : m_OwnerByTag)
    {
        if (kv.second == owner)
            doomed.push_back(kv.first);
    }

    for (const std::string& key : doomed)
    {
        m_Factories.erase(key);
        m_OwnerByTag.erase(key);
        m_CanonicalByLower.erase(key);
        m_TypeInfoByTag.erase(key);
    }

    // The type_info reverse maps are keyed by C++ class, and several tags — possibly several
    // OWNERS' tags — can share one class. Keeping the entry is not enough: it may still NAME a
    // tag that was just swept, in which case a hand-built instance of a still-registered class
    // would report a tag that no longer exists. Re-point it at a surviving tag, and erase only
    // when none survives.
    //
    // This is the expensive part of a sweep and it is deliberately left as a scan: it costs
    // O(classes x tags) type_info comparisons, and type_info::operator== compares the decorated
    // NAME on MSVC, so each one is a string compare rather than a pointer compare. It is
    // affordable only because a sweep runs once per load-context unload — a hot reload, not a
    // frame. Pre-index class -> tags if that ever stops being true (a per-frame or per-document
    // sweep), not before: the index would have to be maintained by every register and alias
    // path to save work on a path that runs seconds apart.
    for (auto it = m_TagByTypeInfo.begin(); it != m_TagByTypeInfo.end();)
    {
        const std::string* survivor = nullptr;
        for (const auto& kv : m_TypeInfoByTag)
        {
            if (kv.second && *kv.second == *it->first)
            {
                // Prefer the entry's current tag if it survived, so a sweep that touched
                // nothing relevant leaves the naming exactly as it was.
                if (kv.first == it->second)
                {
                    survivor = &kv.first;
                    break;
                }
                if (!survivor)
                    survivor = &kv.first;
            }
        }
        if (!survivor)
        {
            m_TagIdByTypeInfo.erase(it->first);
            it = m_TagByTypeInfo.erase(it);
            continue;
        }
        if (it->second != *survivor)
        {
            m_TagIdByTypeInfo[it->first] = HashStringId(*survivor);
            it->second = *survivor;
        }
        ++it;
    }

    // The attribute channel is keyed by attribute NAME and applied to any element carrying
    // that attribute, so a handler left behind would go on applying to elements of other
    // types entirely. It is swept with its factories, not by them.
    for (auto it = m_AllHandlers.begin(); it != m_AllHandlers.end();)
        it = (*it && (*it)->Owner == owner) ? m_AllHandlers.erase(it) : it + 1;

    for (auto byName = m_HandlersByName.begin(); byName != m_HandlersByName.end();)
    {
        std::vector<std::shared_ptr<AttrHandler>>& handlers = byName->second;
        for (auto it = handlers.begin(); it != handlers.end();)
            it = (*it && (*it)->Owner == owner) ? handlers.erase(it) : it + 1;
        byName = handlers.empty() ? m_HandlersByName.erase(byName) : std::next(byName);
    }

    return doomed.size();
}

std::size_t ElementFactoryRegistry::CountFactoriesOwnedBy(std::uint64_t owner) const
{
    std::size_t count = 0;
    for (const auto& kv : m_OwnerByTag)
    {
        if (kv.second == owner && m_Factories.find(kv.first) != m_Factories.end())
            ++count;
    }
    return count;
}

std::size_t ElementFactoryRegistry::CountAttrHandlersOwnedBy(std::uint64_t owner) const
{
    std::size_t count = 0;
    for (const auto& h : m_AllHandlers)
    {
        if (h && h->Owner == owner)
            ++count;
    }
    return count;
}

void ElementFactoryRegistry::RegisterAttrHandler(const std::shared_ptr<AttrHandler>& handler) {
    // index primary and aliases
    for (auto& n : handler->NamesLower) {
        m_HandlersByName[n].push_back(handler);
    }
    m_AllHandlers.push_back(handler);
}

static inline std::string Trim(const std::string& s) {
    size_t a = 0; while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    size_t b = s.size(); while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\n' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

void ElementFactoryRegistry::ApplyAttributes(UIElement& el,
                         const std::unordered_map<std::string, std::string>& attrsLower,
                         const std::string& innerText) const {
    // 1) Apply explicit attributes by name
    for (const auto& kv : attrsLower) {
        auto it = m_HandlersByName.find(kv.first);
        if (it != m_HandlersByName.end()) {
            for (const auto& h : it->second) {
                if (!h->IsText && h->Apply) h->Apply(el, kv.second);
            }
        }
    }

    // 2) Apply inner text if bound
    if (!innerText.empty()) {
        const std::string trimmed = Trim(innerText);
        if (!trimmed.empty()) {
            for (const auto& h : m_AllHandlers) {
                if (h->IsText && h->Apply) h->Apply(el, trimmed);
            }
        }
    }

    // 3) Apply defaults for handlers whose names/aliases are absent
    for (const auto& h : m_AllHandlers) {
        if (h->IsText || !h->ApplyDefault) continue;
        bool present = false;
        for (const auto& n : h->NamesLower) {
            if (attrsLower.find(n) != attrsLower.end()) { present = true; break; }
        }
        if (!present) {
            h->ApplyDefault(el);
        }
    }

    // Last, so an element that binds attributes itself sees the same state a handler-bound
    // one would: explicit values applied, defaults filled in.
    el.OnAuthoredAttributesApplied(attrsLower);
}

}} // namespace GameEngine::UIRegistration

