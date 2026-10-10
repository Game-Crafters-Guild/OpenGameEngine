#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine {
namespace Graph {

class GraphObject;

enum class ValueKind : std::uint8_t
{
    Null = 0,
    Bool,
    Int,
    Float,
    String,
    Guid,
    List,
    Object
};

/**
 * Graph-local typed value. Canonical store for node parameters, passthrough,
 * and extension bags. Does not participate in ECS FieldTypeTraits.
 */
class GraphValue
{
public:
    GraphValue();
    GraphValue(const GraphValue& other);
    GraphValue(GraphValue&& other) noexcept;
    GraphValue& operator=(const GraphValue& other);
    GraphValue& operator=(GraphValue&& other) noexcept;
    ~GraphValue();

    GraphValue(bool value);
    GraphValue(std::nullptr_t) = delete;
    GraphValue(int value);
    GraphValue(std::int64_t value);
    GraphValue(float value);
    GraphValue(double value);
    GraphValue(const char* value);
    GraphValue(std::string value);
    explicit GraphValue(std::string_view value);
    explicit GraphValue(GraphObject object);
    explicit GraphValue(std::vector<GraphValue> list);

    static GraphValue FromGuid(std::string guid);

    ValueKind Kind() const { return m_Kind; }

    bool IsNull() const { return m_Kind == ValueKind::Null; }
    bool IsBool() const { return m_Kind == ValueKind::Bool; }
    bool IsInt() const { return m_Kind == ValueKind::Int; }
    bool IsFloat() const { return m_Kind == ValueKind::Float; }
    bool IsNumber() const { return m_Kind == ValueKind::Int || m_Kind == ValueKind::Float; }
    bool IsString() const { return m_Kind == ValueKind::String; }
    bool IsGuid() const { return m_Kind == ValueKind::Guid; }
    bool IsList() const { return m_Kind == ValueKind::List; }
    bool IsObject() const { return m_Kind == ValueKind::Object; }

    bool empty() const;

    bool AsBool(bool fallback = false) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    double AsFloat(double fallback = 0.0) const;
    std::string ToString() const;

    const std::string* TryString() const;
    const GraphObject* TryObject() const;
    GraphObject* TryObject();
    const std::vector<GraphValue>* TryList() const;
    std::vector<GraphValue>* TryList();

    GraphObject& AsObject();
    std::vector<GraphValue>& AsList();

    GraphValue& operator[](const std::string& key);

    GraphValue& operator=(bool value);
    GraphValue& operator=(int value);
    GraphValue& operator=(std::int64_t value);
    GraphValue& operator=(float value);
    GraphValue& operator=(double value);
    GraphValue& operator=(const char* value);
    GraphValue& operator=(std::string value);

    bool operator==(const GraphValue& other) const;
    bool operator!=(const GraphValue& other) const { return !(*this == other); }
    bool EqualsString(std::string_view text) const;
    /** Parse `text` into this value without changing Kind.
     *  Null becomes String. List and Object are rejected (false, unchanged).
     *  Returns false if `text` cannot be parsed as the current kind. */
    bool AssignFromText(std::string_view text);

private:
    void ResetToNull();
    void CopyFrom(const GraphValue& other);

    ValueKind m_Kind = ValueKind::Null;
    bool m_Bool = false;
    std::int64_t m_Int = 0;
    double m_Float = 0.0;
    std::string m_String;
    std::unique_ptr<std::vector<GraphValue>> m_List;
    std::unique_ptr<GraphObject> m_Object;
};

/**
 * Insertion-ordered object of GraphValue fields.
 * Linear find — node parameter maps are small.
 */
class GraphObject
{
public:
    using Entry = std::pair<std::string, GraphValue>;
    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;

    GraphObject() = default;
    GraphObject(const GraphObject&) = default;
    GraphObject(GraphObject&&) noexcept = default;
    GraphObject& operator=(const GraphObject&) = default;
    GraphObject& operator=(GraphObject&&) noexcept = default;
    ~GraphObject() = default;

    iterator begin() { return m_Entries.begin(); }
    iterator end() { return m_Entries.end(); }
    const_iterator begin() const { return m_Entries.begin(); }
    const_iterator end() const { return m_Entries.end(); }
    const_iterator cbegin() const { return m_Entries.cbegin(); }
    const_iterator cend() const { return m_Entries.cend(); }

    iterator find(std::string_view key);
    const_iterator find(std::string_view key) const;

    GraphValue& operator[](const std::string& key);

    std::pair<iterator, bool> emplace(std::string key, GraphValue value);

    bool empty() const { return m_Entries.empty(); }
    std::size_t size() const { return m_Entries.size(); }
    void clear() { m_Entries.clear(); }
    void erase(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != end(); }

    std::string GetString(std::string_view key, std::string_view fallback = {}) const;
    bool GetBool(std::string_view key, bool fallback = false) const;
    std::int64_t GetInt(std::string_view key, std::int64_t fallback = 0) const;
    double GetFloat(std::string_view key, double fallback = 0.0) const;

private:
    std::vector<Entry> m_Entries;
};

} // namespace Graph
} // namespace GameEngine
