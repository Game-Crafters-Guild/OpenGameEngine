#include "ShaderGraph/MaterialGraphDiagnosticsSummary.h"

namespace GameEngine {

MaterialGraphDiagnosticsSummary SummarizeMaterialGraphDiagnostics(
    const std::vector<ShaderGraph::SgDiagnostic>& errors)
{
    MaterialGraphDiagnosticsSummary summary;
    for (const ShaderGraph::SgDiagnostic& diagnostic : errors)
    {
        if (!summary.Tooltip.empty())
            summary.Tooltip += "; ";
        summary.Tooltip += diagnostic.Message;
        if (!diagnostic.NodeId.empty())
            summary.ErrorNodeIds.insert(diagnostic.NodeId);
    }

    // No "Graph:" prefix: the toolbar slot is sized to content, and a bare
    // count next to the compile dot reads better than an ellipsised one.
    if (errors.empty())
        summary.LabelText = "OK";
    else if (errors.size() == 1)
        summary.LabelText = "1 error";
    else
        summary.LabelText = std::to_string(errors.size()) + " errors";

    /* Only the first message: the folded line is clipped at the strip's width,
       so a second message would be cut before the first finished reading.
       U+00B7 separates, where a hyphen would read as part of a message that
       often contains hyphens itself. */
    if (!errors.empty())
        summary.CollapsedLineText = summary.LabelText + " \xC2\xB7 " + errors.front().Message;
    return summary;
}

std::string OnDiskFailureHintText(std::size_t errorCount)
{
    return "file failing on disk (" + std::to_string(errorCount)
           + (errorCount == 1 ? " error" : " errors") + ") — see Shader Errors";
}

} // namespace GameEngine
