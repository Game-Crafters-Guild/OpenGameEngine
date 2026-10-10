#include "DebugServer/ComponentValueReader.h"

namespace GameEngine::Editor
{

namespace
{
const nlohmann::json& NullJson()
{
    static const nlohmann::json kNull;
    return kNull;
}
} // namespace

ComponentValueReader::ComponentValueReader(const nlohmann::json& values) : m_Values(values)
{
    if (!values.is_object())
        return;
    m_Keys.reserve(values.size());
    for (auto it = values.begin(); it != values.end(); ++it)
        m_Keys.push_back(it.key());
    m_Consumed.assign(m_Keys.size(), false);
}

bool ComponentValueReader::Has(const char* key)
{
    for (std::size_t i = 0; i < m_Keys.size(); ++i)
    {
        if (m_Keys[i] == key)
        {
            m_Consumed[i] = true;
            return true;
        }
    }
    return false;
}

const nlohmann::json& ComponentValueReader::operator[](const char* key) const
{
    if (!m_Values.is_object())
        return NullJson();
    const auto it = m_Values.find(key);
    return it == m_Values.end() ? NullJson() : *it;
}

std::vector<std::string> ComponentValueReader::UnconsumedKeys() const
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m_Keys.size(); ++i)
        if (!m_Consumed[i])
            out.push_back(m_Keys[i]);
    return out;
}

} // namespace GameEngine::Editor
