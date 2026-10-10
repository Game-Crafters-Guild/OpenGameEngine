#include "DebugServer/RenderGraphResourceFilter.h"

#include <cctype>
#include <span>

namespace GameEngine::Editor
{
namespace
{

// Canonical spellings the resource listing emits. Swapchain is the frame's
// imported backbuffer, which is an external import too and so is reported ahead
// of the plain external case.
constexpr std::string_view kLifetimeNames[] = {"Transient", "Persistent", "Imported", "Swapchain"};
constexpr std::string_view kKindNames[] = {"Texture", "Buffer", "AccelerationStructure"};

bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Canonical spelling of a client value, or empty when it names none of them.
std::string_view Canonical(const std::string& value, std::span<const std::string_view> accepted)
{
    for (std::string_view a : accepted)
        if (IEquals(value, a))
            return a;
    return {};
}

std::string AcceptedList(std::span<const std::string_view> accepted)
{
    std::string list;
    for (std::size_t i = 0; i < accepted.size(); ++i)
    {
        if (i != 0)
            list += ", ";
        list += accepted[i];
    }
    return list;
}

// Reads one canonical-name parameter. Omitted, null and empty all mean "no filter";
// anything else the names don't cover fails with a message stating what would work.
// A wrong TYPE has to fail too: skipping it would widen the answer back to the whole
// list, which is the failure this type exists to make impossible.
bool ReadNameParam(const nlohmann::json& params, const char* key,
                   std::span<const std::string_view> accepted, std::string_view& out, std::string& error)
{
    const auto it = params.find(key);
    if (it == params.end() || it->is_null())
        return true;

    if (!it->is_string())
    {
        error = std::string("Parameter '") + key + "' must be a string naming one of: " +
                AcceptedList(accepted) + " - or be omitted to list every resource.";
        return false;
    }

    const std::string value = it->get<std::string>();
    if (value.empty())
        return true;

    out = Canonical(value, accepted);
    if (out.empty())
    {
        error = std::string("Unknown '") + key + "' value '" + value + "'. Use one of: " +
                AcceptedList(accepted) + " - or omit the parameter to list every resource.";
        return false;
    }
    return true;
}

} // namespace

RenderGraphResourceFilter::RenderGraphResourceFilter(const nlohmann::json& params)
{
    if (!ReadNameParam(params, "lifetime", kLifetimeNames, m_Lifetime, m_Error))
        return;
    if (!ReadNameParam(params, "type", kKindNames, m_Kind, m_Error))
        return;

    const auto alive = params.find("aliveOnly");
    if (alive != params.end() && !alive->is_null())
    {
        if (!alive->is_boolean())
        {
            m_Error = "Parameter 'aliveOnly' must be a boolean - true lists only the resources a live pass touches.";
            return;
        }
        m_AliveOnly = alive->get<bool>();
    }
}

bool RenderGraphResourceFilter::Accepts(std::string_view kind, std::string_view lifetime, bool used) const
{
    if (m_AliveOnly && !used)
        return false;
    if (!m_Kind.empty() && m_Kind != kind)
        return false;
    if (!m_Lifetime.empty() && m_Lifetime != lifetime)
        return false;
    return true;
}

} // namespace GameEngine::Editor
