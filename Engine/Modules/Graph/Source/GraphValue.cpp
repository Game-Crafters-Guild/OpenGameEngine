#include "Graph/GraphValue.h"

#include <Types/ParseNumber.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace GameEngine {
namespace Graph {
namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

std::string_view TrimAsciiSpace(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

bool ParseBoolText(std::string_view text, bool& outValue)
{
    text = TrimAsciiSpace(text);
    if (EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "yes") ||
        EqualsIgnoreCase(text, "on") || text == "1")
    {
        outValue = true;
        return true;
    }
    if (EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "no") ||
        EqualsIgnoreCase(text, "off") || text == "0")
    {
        outValue = false;
        return true;
    }
    return false;
}

bool TryNarrowFloatToInt64(double value, std::int64_t& outValue)
{
    if (!std::isfinite(value))
        return false;
    // INT64_MAX is not a double; 2^63 is. Accept [-2^63, 2^63).
    constexpr double kInt64MinAsDouble = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr double kTwoTo63 = 9223372036854775808.0;
    if (value < kInt64MinAsDouble || value >= kTwoTo63)
        return false;
    outValue = static_cast<std::int64_t>(value);
    return true;
}

bool ParseIntText(std::string_view text, std::int64_t& outValue)
{
    std::string_view number = TrimAsciiSpace(text);
    if (number.empty())
        return false;
    if (number[0] == '+')
        number.remove_prefix(1);
    if (number.empty() || number[0] == '+')
        return false;
    const char* begin = number.data();
    const char* end = begin + number.size();
    std::int64_t value = 0;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;
    outValue = value;
    return true;
}


std::string FormatFloat(double value)
{
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general, 9);
    if (result.ec != std::errc{})
        return "0";
    return std::string(buf, static_cast<std::size_t>(result.ptr - buf));
}

} // namespace

// Intersection with Apple strtod_l: one optional leading '+', no 0x (including
// after '-'), finite decimal. leftover '+' and 0x are rejected here because
// strtod_l accepts them; MSVC from_chars already fails both.
GraphValue::GraphValue() = default;

GraphValue::GraphValue(const GraphValue& other)
{
    CopyFrom(other);
}

GraphValue::GraphValue(GraphValue&& other) noexcept = default;

GraphValue& GraphValue::operator=(const GraphValue& other)
{
    if (this != &other)
        CopyFrom(other);
    return *this;
}

GraphValue& GraphValue::operator=(GraphValue&& other) noexcept = default;

GraphValue::~GraphValue() = default;

GraphValue::GraphValue(bool value)
    : m_Kind(ValueKind::Bool)
    , m_Bool(value)
{
}

GraphValue::GraphValue(int value)
    : m_Kind(ValueKind::Int)
    , m_Int(value)
{
}

GraphValue::GraphValue(std::int64_t value)
    : m_Kind(ValueKind::Int)
    , m_Int(value)
{
}

GraphValue::GraphValue(float value)
    : m_Kind(ValueKind::Float)
    , m_Float(value)
{
}

GraphValue::GraphValue(double value)
    : m_Kind(ValueKind::Float)
    , m_Float(value)
{
}

GraphValue::GraphValue(const char* value)
    : m_Kind(ValueKind::String)
    , m_String(value ? value : "")
{
}

GraphValue::GraphValue(std::string value)
    : m_Kind(ValueKind::String)
    , m_String(std::move(value))
{
}

GraphValue::GraphValue(std::string_view value)
    : m_Kind(ValueKind::String)
    , m_String(value)
{
}

GraphValue::GraphValue(GraphObject object)
    : m_Kind(ValueKind::Object)
    , m_Object(std::make_unique<GraphObject>(std::move(object)))
{
}

GraphValue::GraphValue(std::vector<GraphValue> list)
    : m_Kind(ValueKind::List)
    , m_List(std::make_unique<std::vector<GraphValue>>(std::move(list)))
{
}

GraphValue GraphValue::FromGuid(std::string guid)
{
    GraphValue value;
    value.m_Kind = ValueKind::Guid;
    value.m_String = std::move(guid);
    return value;
}

void GraphValue::ResetToNull()
{
    m_Kind = ValueKind::Null;
    m_Bool = false;
    m_Int = 0;
    m_Float = 0.0;
    m_String.clear();
    m_List.reset();
    m_Object.reset();
}

void GraphValue::CopyFrom(const GraphValue& other)
{
    ResetToNull();
    m_Kind = other.m_Kind;
    m_Bool = other.m_Bool;
    m_Int = other.m_Int;
    m_Float = other.m_Float;
    m_String = other.m_String;
    if (other.m_List)
        m_List = std::make_unique<std::vector<GraphValue>>(*other.m_List);
    if (other.m_Object)
        m_Object = std::make_unique<GraphObject>(*other.m_Object);
}

bool GraphValue::empty() const
{
    switch (m_Kind)
    {
    case ValueKind::Null:
        return true;
    case ValueKind::String:
    case ValueKind::Guid:
        return m_String.empty();
    case ValueKind::List:
        return !m_List || m_List->empty();
    case ValueKind::Object:
        return !m_Object || m_Object->empty();
    default:
        return false;
    }
}

bool GraphValue::AsBool(bool fallback) const
{
    switch (m_Kind)
    {
    case ValueKind::Bool:
        return m_Bool;
    case ValueKind::Int:
        return m_Int != 0;
    case ValueKind::Float:
        return m_Float != 0.0;
    case ValueKind::String:
    case ValueKind::Guid:
    {
        bool parsed = fallback;
        if (ParseBoolText(m_String, parsed))
            return parsed;
        return fallback;
    }
    default:
        return fallback;
    }
}

std::int64_t GraphValue::AsInt(std::int64_t fallback) const
{
    switch (m_Kind)
    {
    case ValueKind::Int:
        return m_Int;
    case ValueKind::Float:
        return static_cast<std::int64_t>(m_Float);
    case ValueKind::Bool:
        return m_Bool ? 1 : 0;
    case ValueKind::String:
    case ValueKind::Guid:
    {
        std::int64_t parsed = 0;
        if (ParseIntText(m_String, parsed))
            return parsed;
        double asFloat = 0.0;
        if (ParseStrictDecimal(m_String, asFloat) && TryNarrowFloatToInt64(asFloat, parsed))
            return parsed;
        return fallback;
    }
    default:
        return fallback;
    }
}

double GraphValue::AsFloat(double fallback) const
{
    switch (m_Kind)
    {
    case ValueKind::Float:
        return m_Float;
    case ValueKind::Int:
        return static_cast<double>(m_Int);
    case ValueKind::Bool:
        return m_Bool ? 1.0 : 0.0;
    case ValueKind::String:
    case ValueKind::Guid:
    {
        double parsed = 0.0;
        if (ParseStrictDecimal(m_String, parsed))
            return parsed;
        return fallback;
    }
    default:
        return fallback;
    }
}

std::string GraphValue::ToString() const
{
    switch (m_Kind)
    {
    case ValueKind::Null:
        return {};
    case ValueKind::Bool:
        return m_Bool ? "true" : "false";
    case ValueKind::Int:
        return std::to_string(m_Int);
    case ValueKind::Float:
        return FormatFloat(m_Float);
    case ValueKind::String:
    case ValueKind::Guid:
        return m_String;
    case ValueKind::List:
        return m_List && !m_List->empty() ? "[...]" : "[]";
    case ValueKind::Object:
        return m_Object && !m_Object->empty() ? "{...}" : "{}";
    }
    return {};
}

const std::string* GraphValue::TryString() const
{
    if (m_Kind == ValueKind::String || m_Kind == ValueKind::Guid)
        return &m_String;
    return nullptr;
}

const GraphObject* GraphValue::TryObject() const
{
    return m_Kind == ValueKind::Object ? m_Object.get() : nullptr;
}

GraphObject* GraphValue::TryObject()
{
    return m_Kind == ValueKind::Object ? m_Object.get() : nullptr;
}

const std::vector<GraphValue>* GraphValue::TryList() const
{
    return m_Kind == ValueKind::List ? m_List.get() : nullptr;
}

std::vector<GraphValue>* GraphValue::TryList()
{
    return m_Kind == ValueKind::List ? m_List.get() : nullptr;
}

GraphObject& GraphValue::AsObject()
{
    if (m_Kind != ValueKind::Object || !m_Object)
    {
        ResetToNull();
        m_Kind = ValueKind::Object;
        m_Object = std::make_unique<GraphObject>();
    }
    return *m_Object;
}

std::vector<GraphValue>& GraphValue::AsList()
{
    if (m_Kind != ValueKind::List || !m_List)
    {
        ResetToNull();
        m_Kind = ValueKind::List;
        m_List = std::make_unique<std::vector<GraphValue>>();
    }
    return *m_List;
}

GraphValue& GraphValue::operator[](const std::string& key)
{
    return AsObject()[key];
}

GraphValue& GraphValue::operator=(bool value)
{
    ResetToNull();
    m_Kind = ValueKind::Bool;
    m_Bool = value;
    return *this;
}

GraphValue& GraphValue::operator=(int value)
{
    ResetToNull();
    m_Kind = ValueKind::Int;
    m_Int = value;
    return *this;
}

GraphValue& GraphValue::operator=(std::int64_t value)
{
    ResetToNull();
    m_Kind = ValueKind::Int;
    m_Int = value;
    return *this;
}

GraphValue& GraphValue::operator=(float value)
{
    ResetToNull();
    m_Kind = ValueKind::Float;
    m_Float = value;
    return *this;
}

GraphValue& GraphValue::operator=(double value)
{
    ResetToNull();
    m_Kind = ValueKind::Float;
    m_Float = value;
    return *this;
}

GraphValue& GraphValue::operator=(const char* value)
{
    ResetToNull();
    m_Kind = ValueKind::String;
    m_String = value ? value : "";
    return *this;
}

GraphValue& GraphValue::operator=(std::string value)
{
    ResetToNull();
    m_Kind = ValueKind::String;
    m_String = std::move(value);
    return *this;
}

bool GraphValue::operator==(const GraphValue& other) const
{
    if (m_Kind != other.m_Kind)
        return false;
    switch (m_Kind)
    {
    case ValueKind::Null:
        return true;
    case ValueKind::Bool:
        return m_Bool == other.m_Bool;
    case ValueKind::Int:
        return m_Int == other.m_Int;
    case ValueKind::Float:
        return m_Float == other.m_Float;
    case ValueKind::String:
    case ValueKind::Guid:
        return m_String == other.m_String;
    case ValueKind::List:
        if (static_cast<bool>(m_List) != static_cast<bool>(other.m_List))
            return false;
        return !m_List || *m_List == *other.m_List;
    case ValueKind::Object:
        if (static_cast<bool>(m_Object) != static_cast<bool>(other.m_Object))
            return false;
        if (!m_Object)
            return true;
        if (m_Object->size() != other.m_Object->size())
            return false;
        for (const auto& [key, value] : *m_Object)
        {
            auto it = other.m_Object->find(key);
            if (it == other.m_Object->end() || it->second != value)
                return false;
        }
        return true;
    }
    return false;
}

bool GraphValue::EqualsString(std::string_view text) const
{
    const std::string owned = (m_Kind == ValueKind::String || m_Kind == ValueKind::Guid) ? m_String : ToString();
    return owned.size() == text.size() &&
           (owned.empty() || std::memcmp(owned.data(), text.data(), owned.size()) == 0);
}

bool GraphValue::AssignFromText(std::string_view text)
{
    switch (m_Kind)
    {
    case ValueKind::Null:
        *this = std::string(text);
        return true;
    case ValueKind::Bool:
    {
        bool parsed = false;
        if (!ParseBoolText(text, parsed))
            return false;
        *this = parsed;
        return true;
    }
    case ValueKind::Int:
    {
        std::int64_t parsed = 0;
        if (!ParseIntText(text, parsed))
        {
            double asFloat = 0.0;
            if (!ParseStrictDecimal(text, asFloat) || !TryNarrowFloatToInt64(asFloat, parsed))
                return false;
        }
        *this = parsed;
        return true;
    }
    case ValueKind::Float:
    {
        double parsed = 0.0;
        if (!ParseStrictDecimal(text, parsed) || !std::isfinite(parsed))
            return false;
        *this = parsed;
        return true;
    }
    case ValueKind::String:
        *this = std::string(text);
        return true;
    case ValueKind::Guid:
        m_String = std::string(text);
        return true;
    case ValueKind::List:
    case ValueKind::Object:
        return false;
    }
    return false;
}

GraphObject::iterator GraphObject::find(std::string_view key)
{
    return std::find_if(m_Entries.begin(), m_Entries.end(),
                        [&](const Entry& entry) { return entry.first == key; });
}

GraphObject::const_iterator GraphObject::find(std::string_view key) const
{
    return std::find_if(m_Entries.begin(), m_Entries.end(),
                        [&](const Entry& entry) { return entry.first == key; });
}

GraphValue& GraphObject::operator[](const std::string& key)
{
    if (auto it = find(key); it != end())
        return it->second;
    m_Entries.emplace_back(key, GraphValue{});
    return m_Entries.back().second;
}

std::pair<GraphObject::iterator, bool> GraphObject::emplace(std::string key, GraphValue value)
{
    if (auto it = find(key); it != end())
        return {it, false};
    m_Entries.emplace_back(std::move(key), std::move(value));
    return {std::prev(end()), true};
}

void GraphObject::erase(std::string_view key)
{
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&](const Entry& entry) { return entry.first == key; }),
                    m_Entries.end());
}

std::string GraphObject::GetString(std::string_view key, std::string_view fallback) const
{
    auto it = find(key);
    if (it == end())
        return std::string(fallback);
    std::string text = it->second.ToString();
    return text.empty() && it->second.IsNull() ? std::string(fallback) : text;
}

bool GraphObject::GetBool(std::string_view key, bool fallback) const
{
    auto it = find(key);
    if (it == end())
        return fallback;
    return it->second.AsBool(fallback);
}

std::int64_t GraphObject::GetInt(std::string_view key, std::int64_t fallback) const
{
    auto it = find(key);
    if (it == end())
        return fallback;
    return it->second.AsInt(fallback);
}

double GraphObject::GetFloat(std::string_view key, double fallback) const
{
    auto it = find(key);
    if (it == end())
        return fallback;
    return it->second.AsFloat(fallback);
}

} // namespace Graph
} // namespace GameEngine
