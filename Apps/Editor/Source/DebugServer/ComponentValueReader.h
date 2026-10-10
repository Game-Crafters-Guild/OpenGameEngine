#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Reads a `set_component` values object while recording which keys were looked at, so a
// handler branch that understands a fixed set of field names can report the keys it did
// not apply.
//
// The trap this exists to close: a branch written on raw `values.contains("Density")`
// cannot tell a MISSPELLED key from an ABSENT one, so every typo and every wrong-case
// name reads as a successful no-op — the caller is told `ok: true` about a write that
// changed nothing. Consuming keys through this reader makes the leftovers visible.
//
// A key is consumed by asking about it, not by the caller liking its value: a branch that
// recognises a field owns the outcome, including a value it rejects on its own terms.
class ComponentValueReader
{
public:
    explicit ComponentValueReader(const nlohmann::json& values);

    // True when the key is present. Marks it consumed either way.
    bool Has(const char* key);

    // The value of a key already matched with Has(). Null json when absent.
    const nlohmann::json& operator[](const char* key) const;

    // Value of `key` if present and convertible, else `fallback`. Marks it consumed.
    template <class T>
    T Value(const char* key, T fallback)
    {
        if (!Has(key))
            return fallback;
        const nlohmann::json& v = (*this)[key];
        return v.is_null() ? fallback : v.get<T>();
    }

    // Keys nothing asked about, in the order the request supplied them. Empty means every
    // key in the request reached a field.
    std::vector<std::string> UnconsumedKeys() const;

private:
    const nlohmann::json& m_Values;
    std::vector<std::string> m_Keys;     // request order
    std::vector<bool> m_Consumed;        // parallel to m_Keys
};

} // namespace GameEngine::Editor
