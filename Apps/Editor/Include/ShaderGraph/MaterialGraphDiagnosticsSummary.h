#pragma once

#include "Rendering/ShaderGraph/SgTypes.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine {

/** What the shader graph panel shows for one compile verdict, derived from the
 *  compiler's diagnostics: the toolbar label, its tooltip, and the nodes to
 *  badge. Pure, so it is pinned by tests without a panel. */
struct MaterialGraphDiagnosticsSummary
{
    /** "OK" for a clean compile, else the count: "1 error", "3 errors". */
    std::string LabelText;
    /** Every message in order, "; "-joined; empty for a clean compile. */
    std::string Tooltip;
    /** The nodes the diagnostics name. A graph-scoped failure names none. */
    std::unordered_set<std::string> ErrorNodeIds;
    /** The folded strip's one line: the count and the first message, so a
     *  closed strip still names the break. Empty for a clean compile. */
    std::string CollapsedLineText;
};

MaterialGraphDiagnosticsSummary SummarizeMaterialGraphDiagnostics(
    const std::vector<ShaderGraph::SgDiagnostic>& errors);

/** The strip's honest empty state: no verdict in the panel, but the Shader
 *  Errors mirror still holds `errorCount` rows for the open file. */
std::string OnDiskFailureHintText(std::size_t errorCount);

} // namespace GameEngine
