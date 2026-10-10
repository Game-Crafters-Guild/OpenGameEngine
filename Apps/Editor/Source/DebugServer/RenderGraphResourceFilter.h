#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace GameEngine::Editor
{

// The `lifetime` / `type` / `aliveOnly` selector of the `get_render_graph_resources`
// debug command.
//
// The trap this exists to close: a filter the handler never reads returns the FULL
// resource list, and a full list is exactly what a correct filtered answer looks
// like — the caller gets a wrong answer with no error to notice. So an absent or
// empty value means "no filter", and an unrecognised one is an error naming the
// accepted values rather than a silent widening back to everything.
//
// Matching is case-insensitive; `Accepts` compares against the canonical spellings
// the resource listing itself emits.
class RenderGraphResourceFilter
{
public:
    // Parses the request parameters. Check Error() before using the filter.
    explicit RenderGraphResourceFilter(const nlohmann::json& params);

    // Empty when the parameters were understood; otherwise a message naming the
    // parameter, the value nothing matched, and the values that would.
    const std::string& Error() const { return m_Error; }

    // `kind` and `lifetime` are the canonical names of one listed resource; `used`
    // is its realization gate (any live pass touches it).
    bool Accepts(std::string_view kind, std::string_view lifetime, bool used) const;

private:
    std::string_view m_Lifetime; // empty = any
    std::string_view m_Kind;     // empty = any
    bool m_AliveOnly = false;
    std::string m_Error;
};

} // namespace GameEngine::Editor
