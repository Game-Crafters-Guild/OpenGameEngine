#pragma once

#include "Graph/GraphModel.h"
#include "Graph/GraphPortLabels.h"

#include <cmath>

namespace GameEngine {
namespace GraphNodeMetrics {

inline constexpr float kGridSizeGraph = 20.f;
inline constexpr float kNodeWidth = kGridSizeGraph * 10.f;
inline constexpr float kNodeHeight = kGridSizeGraph * 3.f;
inline constexpr float kNodeTitleRowHeightGraph = 28.f;
inline constexpr float kNodePortAreaTopPaddingGraph = 14.f;
inline constexpr float kNodePortAreaBottomPaddingGraph = 12.f;
inline constexpr float kPortSpacing = 24.f;
/* Diameter in graph space. Width percent is of the node's rect width;
   height percent is of this node's height, which grows with port rows. */
inline constexpr float kPortSizeGraph = 10.f;
/* Port hangs half its size past the node edge. */
inline constexpr float kPortHangGraph = kPortSizeGraph * 0.5f;
/* Port labels start at node-x 10. Long names ellipsize in the 60-unit
   column; value hosts run to the right-edge port clearance. The output port
   sits mid-node and does not own a full-height Out column. */
inline constexpr float kOutGutterGraph = 20.f;
inline constexpr float kInlineLabelStartGraph = kPortSizeGraph;
inline constexpr float kInlineLabelColumnGraph = 60.f;
inline constexpr float kInlineEditorStartGraph = kInlineLabelStartGraph + kInlineLabelColumnGraph;
inline constexpr float kInlineEditorEndGraph = kNodeWidth - kOutGutterGraph;
inline constexpr float kInlineEditorWidthGraph = kInlineEditorEndGraph - kInlineEditorStartGraph;
/* Percents of the port, used by node-graph.css for port-relative hosts. */
inline constexpr float kInlineEditorLeftPercentOfPort =
    ((kInlineEditorStartGraph + kPortHangGraph) / kPortSizeGraph) * 100.f;
inline constexpr float kInlineEditorWidthPercentOfPort =
    (kInlineEditorWidthGraph / kPortSizeGraph) * 100.f;
inline constexpr float kInlineLabelMaxWidthPercentOfPort =
    (kInlineLabelColumnGraph / kPortSizeGraph) * 100.f;
/* Value host is 180% of the port (18 units). Port centers are 24 apart, so
   stacked boxes keep a 6-unit row gap and still fit 12px type. */
inline constexpr float kInlineEditorHeightPercentOfPort = 180.f;
inline constexpr float kInlineEditorTopPercentOfPort = -40.f;
/* Out labels sit inside the node, right-aligned 10 units from the right
   edge (right:150% of the out port) and ellipsized at 50 units. Any value
   host sharing the port row ends before the label the row actually has —
   measured per name, so "Out" costs 23 units and "Pressed" costs 50, and
   the box never runs under the glyphs. */
/* An input row and an Out port collide when their centers are closer than
   this, in graph units — units, not percent of node height, because the value
   host is a fixed 16 units tall (160% of the port) whatever the node height.
   Half of (host 16 + label row ~12) plus rounding: an output centered between
   two rows (12 units away on a 2-row node) still collides; a neighbouring row
   (24 units) does not. */
inline constexpr float kShareOutRowGraph = 16.f;
inline constexpr float kOutLabelGlyphGraph = 7.f;
inline constexpr float kOutLabelPadGraph = 2.f;
inline constexpr float kOutLabelRightInsetGraph = 10.f;
inline constexpr float kOutLabelColumnGraph = 50.f;
inline constexpr float kHostOutLabelGapGraph = 4.f;
/* A shared host never shrinks below this; longer Out names ellipsize. */
inline constexpr float kInlineEditorMinWidthGraph = 60.f;
/* Lone float boxes cap below the full slot: a full-slot box for one number
   reads overlong next to shared-row neighbours. Strings keep the full slot. */
inline constexpr float kFloatEditorMaxWidthGraph = 90.f;
inline constexpr float kFloatEditorMaxWidthPercentOfPort =
    (kFloatEditorMaxWidthGraph / kPortSizeGraph) * 100.f;
inline constexpr float kOutLabelMaxWidthPercentOfPort =
    (kOutLabelColumnGraph / kPortSizeGraph) * 100.f;

inline float OutLabelWidthGraph(int nameChars)
{
    if (nameChars <= 0)
        return 0.f;
    const float width = static_cast<float>(nameChars) * kOutLabelGlyphGraph + kOutLabelPadGraph;
    return width < kOutLabelColumnGraph ? width : kOutLabelColumnGraph;
}

inline float PortWidthPercent(float nodeWidthGraph = kNodeWidth)
{
    const float width = nodeWidthGraph > 1.0f ? nodeWidthGraph : 1.0f;
    return (kPortSizeGraph / width) * 100.0f;
}

inline float PortSideHangPercent(float nodeWidthGraph = kNodeWidth)
{
    const float width = nodeWidthGraph > 1.0f ? nodeWidthGraph : 1.0f;
    return (-kPortHangGraph / width) * 100.0f;
}

inline void CountPorts(const Graph::Node& node, int& outInputs, int& outOutputs)
{
    outInputs = 0;
    outOutputs = 0;
    for (const auto& port : node.Ports)
    {
        if (port.Direction == Graph::PortDirection::In)
            ++outInputs;
        else
            ++outOutputs;
    }
}

/* The label a port renders: its DisplayName, or its id prettified — material
   registrations often carry bare ids ("normal"), and a raw id on a node is
   an unfinished look. Every measurement uses the same label. */
inline std::string PortDisplayLabel(const Graph::Port& port)
{
    return port.DisplayName.empty() ? GraphPortLabels::PrettifyParamKey(port.Id)
                                   : port.DisplayName;
}

/* Strictly "vec2(...)", "vec3(...)" or "vec4(...)" — GraphValue::ToString for
   vector parameters. Arbitrary text with digits stays text. */
inline int VecTextComponentCount(const std::string& text)
{
    if (text.size() < 6 || text.compare(0, 3, "vec") != 0)
        return 0;
    const char n = text[3];
    if (n < '2' || n > '4' || text[4] != '(' || text.back() != ')')
        return 0;
    return n - '0';
}

enum class InlineEditorKind { None, Float, Vec2, Vec3, Vec4, Bool, String };

inline InlineEditorKind EditorKindForDataType(std::string_view dataType)
{
    if (dataType == "float")
        return InlineEditorKind::Float;
    if (dataType == "float2" || dataType == "vec2")
        return InlineEditorKind::Vec2;
    if (dataType == "float3" || dataType == "vec3")
        return InlineEditorKind::Vec3;
    if (dataType == "float4" || dataType == "vec4")
        return InlineEditorKind::Vec4;
    if (dataType == "bool")
        return InlineEditorKind::Bool;
    if (dataType == "string")
        return InlineEditorKind::String;
    return InlineEditorKind::None;
}

/* drawsInlineEditors is the node type's say (GraphPortedNode::
   DrawsInlinePortEditors) — kinds that edit values elsewhere draw none. */
inline InlineEditorKind PortEditorKind(const Graph::Port& inputPort, bool drawsInlineEditors)
{
    if (!drawsInlineEditors)
        return InlineEditorKind::None;
    return EditorKindForDataType(inputPort.DataType);
}

/* Total port rows: inputs and outputs occupy independent left/right columns
   (Unreal-style) and the node fits the deeper one. */
inline int PortRowTotal(const Graph::Node& node)
{
    int inputs = 0;
    int outputs = 0;
    CountPorts(node, inputs, outputs);
    const int rows = inputs > outputs ? inputs : outputs;
    return rows > 0 ? rows : 1;
}

inline float SnapUpToGrid(float units)
{
    return std::ceil(units / kGridSizeGraph) * kGridSizeGraph;
}

/* Heights snap up to the grid like widths do; the slack is extra bottom
   padding, because port rows anchor at the top with fixed kPortSpacing steps
   rather than stretching to the snapped bottom. */
/* Height of the port area alone. Port placement always measures against this,
   never against a rect grown by detail rows. */
inline float GetNodeBaseHeight(const Graph::Node& node)
{
    const int portRows = PortRowTotal(node);
    if (portRows <= 1)
        return kNodeHeight;
    const float grown = kNodeTitleRowHeightGraph + kNodePortAreaTopPaddingGraph +
        kNodePortAreaBottomPaddingGraph + kPortSpacing * static_cast<float>(portRows - 1);
    return SnapUpToGrid(grown > kNodeHeight ? grown : kNodeHeight);
}

/* ---- Expanded node view -------------------------------------------------
   Parameters that get no inline port editor are invisible on a collapsed node.
   Expanded view gives each one a row of its own under the port area. Ports keep
   their base-height lattice positions; only the rect grows, so a wire between
   two grid-aligned nodes stays straight whichever view they are in. */

/* "title" names the node in its header; it is never a value row. */
inline bool ExpandedViewSkipsParam(std::string_view key)
{
    return key == "title";
}

/* A param earns an expanded row when the node gives it no inline editor.
   drawsInlineEditors is the node type's say (GraphPortedNode::
   DrawsInlinePortEditors): kinds that draw none put every value in Expand. */
inline bool ParamHasInlineEditor(const Graph::Node& node, const std::string& key,
                                 bool drawsInlineEditors)
{
    if (!drawsInlineEditors)
        return false;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction == Graph::PortDirection::In && port.Id == key)
            return true;
    }
    return false;
}

inline int ExpandedExtraParamCount(const Graph::Node& node, bool drawsInlineEditors)
{
    int extra = 0;
    for (const auto& entry : node.Parameters)
    {
        if (ExpandedViewSkipsParam(entry.first))
            continue;
        if (!ParamHasInlineEditor(node, entry.first, drawsInlineEditors))
            ++extra;
    }
    return extra;
}

/* Rows drawn below the port area. Expanded view shows every port-less param; a
   multi-row node shows its central-editor params there in any view, because
   the central host has no room on a body taller than one row.
   hasCentralEditors is the node type's say (GraphPortedNode::
   HasCentralEditors, reachable through the pool's prototype). */
inline int DetailRowCount(const Graph::Node& node, bool drawsInlineEditors,
                          bool hasCentralEditors, bool expandedView)
{
    const bool active = expandedView ||
        (hasCentralEditors && GetNodeBaseHeight(node) > kNodeHeight);
    if (!active)
        return 0;
    return ExpandedExtraParamCount(node, drawsInlineEditors);
}

/* Visits the params DetailRowCount counts, in Parameters order. */
template <typename Fn>
inline void ForEachDetailParam(const Graph::Node& node, bool drawsInlineEditors,
                               bool hasCentralEditors, bool expandedView, Fn&& fn)
{
    if (DetailRowCount(node, drawsInlineEditors, hasCentralEditors, expandedView) <= 0)
        return;
    for (const auto& entry : node.Parameters)
    {
        if (ExpandedViewSkipsParam(entry.first))
            continue;
        if (!ParamHasInlineEditor(node, entry.first, drawsInlineEditors))
            fn(entry.first, entry.second);
    }
}

/* Air under the last detail row: without it the row's band ends flush with
   the node bottom. */
inline constexpr float kDetailRowsBottomPadGraph = 8.f;

/* Center (graph units, from the node top) of expanded row `rowIndex`.
   Rows hang off the port lattice, not the base height: the base carries the
   port-area bottom padding and its own grid snap below the last port, and
   neither may push the rows down. */
inline float ExpandedRowCenterGraph(const Graph::Node& node, int rowIndex)
{
    const int portRows = PortRowTotal(node);
    const float lastPortY = kNodeTitleRowHeightGraph + kNodePortAreaTopPaddingGraph +
        kPortSpacing * static_cast<float>(portRows > 1 ? portRows - 1 : 0);
    return lastPortY + static_cast<float>(rowIndex + 1) * kPortSpacing;
}

/* Bottom edge of the detail-row band, measured the way the rows are actually
   placed. Height math and reserved-block placement must both anchor here:
   deriving one from the base height and the other from the port lattice leaves
   their difference as dead space between the last row and the block. */
inline float DetailRowsBottomGraph(const Graph::Node& node, int rows)
{
    if (rows <= 0)
        return GetNodeBaseHeight(node);
    return ExpandedRowCenterGraph(node, rows - 1) + kPortSpacing * 0.5f +
        kDetailRowsBottomPadGraph;
}

/* Rect height including detail rows and whatever block the node type reserves
   below them (GraphPortedNode::ReservedBlockHeight — the material graph's
   preview plate). syntheticDetailRows are the node type's synthetic rows
   (GraphPortedNode::SyntheticDetailRowCount — the material parameter Value row),
   appended after the parameter rows. Grid-snap slack lands under the block as
   bottom padding. Port math uses the BASE height, never this one: growing the
   rect must not move the ports. */
inline float GetNodeHeight(const Graph::Node& node, bool drawsInlineEditors,
                                bool hasCentralEditors, bool expandedView,
                                float reservedBlockHeight = 0.f, int syntheticDetailRows = 0)
{
    const int rows =
        DetailRowCount(node, drawsInlineEditors, hasCentralEditors, expandedView) +
        (syntheticDetailRows > 0 ? syntheticDetailRows : 0);
    if (rows <= 0 && reservedBlockHeight <= 0.f)
        return GetNodeBaseHeight(node);
    const float base = GetNodeBaseHeight(node);
    const float rowsBottom = DetailRowsBottomGraph(node, rows);
    return SnapUpToGrid((rowsBottom > base ? rowsBottom : base) + reservedBlockHeight);
}

/* Margin around a node-type's reserved block. The texture drop slot sits inside
   it; the material graph's preview plate runs the block edge to edge and ignores
   this. Either way the block is exactly as tall as the node is wide. */
inline constexpr float kNodeBlockInsetGraph = 6.f;

inline float HeaderHeightPercent(float nodeHeightGraph)
{
    const float height = nodeHeightGraph > 1.0f ? nodeHeightGraph : 1.0f;
    return (kNodeTitleRowHeightGraph / height) * 100.0f;
}

inline float PortHeightPercent(float nodeHeightGraph)
{
    const float height = nodeHeightGraph > 1.0f ? nodeHeightGraph : 1.0f;
    return (kPortSizeGraph / height) * 100.0f;
}

inline float PortTopPercent(int index, int count, float nodeHeightGraph)
{
    const float height = nodeHeightGraph > 1.0f ? nodeHeightGraph : 1.0f;
    const float portAreaTop = kNodeTitleRowHeightGraph + kNodePortAreaTopPaddingGraph;
    /* Every port sits on the same top-anchored lattice: row i is exactly
       portAreaTop + i*kPortSpacing whatever the node. Centering lone ports put
       them 3 units below row 0 of a taller neighbour, so a wire between two
       grid-aligned nodes could never be straight. Heights snap up to the
       grid; the slack stays at the bottom. */
    const int clamped = index < 0 ? 0 : index;
    const float portY = portAreaTop + kPortSpacing * static_cast<float>(clamped);
    (void)count;
    const float top = portY - kPortSizeGraph * 0.5f;
    return (top / height) * 100.0f;
}

/* ---- Measured rows ------------------------------------------------------
   The label column is not fixed: each input row measures its abbreviated
   label and the editor starts right after it, so short labels buy their
   editor width and long labels never clip. Rows clamp inside the uniform
   node width (value spans keep their minimums, labels ellipsize). */

/* Mirrors node-graph.css: every value box pads calc(6px * --length-scale). */
inline constexpr float kValueBoxInsetGraph = 6.f;
/* A chip holds a signed two-decimal value — "-4.88", five glyphs — with the
   shared inset intact on BOTH sides; clipping otherwise eats the right one. */
inline constexpr float kVecChipGlyphs = 5.f;
inline constexpr float kVecChipWidthGraph =
    kVecChipGlyphs * kOutLabelGlyphGraph + 2.f * kValueBoxInsetGraph;
/* Axis label, gap, chip, inter-axis gap. */
inline constexpr float kVecAxisSpanGraph =
    kOutLabelGlyphGraph + kHostOutLabelGapGraph + kVecChipWidthGraph + kHostOutLabelGapGraph;

inline float InlineLabelWidthGraph(int chars)
{
    return static_cast<float>(chars) * kOutLabelGlyphGraph + kOutLabelPadGraph;
}

/* Where this row's editor starts: after its measured label plus real air —
   the glyph estimate runs tight on mixed-case text, and a label touching
   its value box reads as clipped. */
inline float InlineEditorStartGraphFor(int labelChars)
{
    return kInlineLabelStartGraph + InlineLabelWidthGraph(labelChars) +
           2.f * kHostOutLabelGapGraph;
}

/* Longest Out-port name (glyphs) whose row the given input row shares; 0 when
   the row is clear. Mirrors the PortTopPercent placement. */
inline int SharedOutNameCharsForInput(const Graph::Node& node, int inputIndex)
{
    int inputCount = 0;
    int outputCount = 0;
    CountPorts(node, inputCount, outputCount);
    if (outputCount == 0 || inputIndex >= inputCount)
        return 0;

    // Port geometry: base height, so detail rows never shift a shared row.
    const float nodeHeight = GetNodeBaseHeight(node);
    const float inTopUnits =
        PortTopPercent(inputIndex, inputCount, nodeHeight) * nodeHeight / 100.f;
    int chars = 0;
    int outScan = 0;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction != Graph::PortDirection::Out)
            continue;
        const float outTopUnits =
            PortTopPercent(outScan++, outputCount, nodeHeight) * nodeHeight / 100.f;
        const float delta =
            inTopUnits > outTopUnits ? inTopUnits - outTopUnits : outTopUnits - inTopUnits;
        if (delta >= kShareOutRowGraph)
            continue;
        const std::string_view outName = port.DisplayName.empty() ? port.Id : port.DisplayName;
        if (static_cast<int>(outName.size()) > chars)
            chars = static_cast<int>(outName.size());
    }
    return chars;
}

/* Units the row keeps clear at its right edge. */
inline float InlineEditorRightReserveGraph(int sharedOutNameChars)
{
    if (sharedOutNameChars <= 0)
        return kOutGutterGraph;
    return kOutLabelRightInsetGraph + OutLabelWidthGraph(sharedOutNameChars) +
           kHostOutLabelGapGraph;
}

/* One value column per node: every editor starts at the widest measured
   label's edge, inspector-style, so stacked rows keep identical widths
   instead of raggedly following their own labels.

   The column carries all three row sources, so all three are measured here:
   port rows, detail-param rows, and the node type's synthetic rows
   (syntheticLabelChars — GraphPortedNode owns their text, so it measures
   them). Measuring ports alone collapses the column to zero on a node whose
   rows are all params — a parameter node has no editable In ports — which
   flushes its value boxes against the node edge and clamps its labels to
   zero width. */
inline float NodeEditorStartGraph(const Graph::Node& node, bool drawsInlineEditors,
                                  bool hasCentralEditors, bool expandedView,
                                  int syntheticLabelChars = 0)
{
    int chars = syntheticLabelChars > 0 ? syntheticLabelChars : 0;
    auto widen = [&chars](std::string_view text)
    {
        const int size = static_cast<int>(GraphPortLabels::AbbreviateForNode(text).size());
        if (size > chars)
            chars = size;
    };
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction != Graph::PortDirection::In)
            continue;
        const InlineEditorKind kind = PortEditorKind(port, drawsInlineEditors);
        if (kind == InlineEditorKind::None || kind == InlineEditorKind::Bool)
            continue;
        widen(PortDisplayLabel(port));
    }
    ForEachDetailParam(node, drawsInlineEditors, hasCentralEditors, expandedView,
                       [&widen](const std::string& key, const Graph::GraphValue&)
    {
        widen(GraphPortLabels::PrettifyParamKey(key));
    });
    if (chars <= 0)
        return 0.f;
    return InlineEditorStartGraphFor(chars);
}

/* Every node shares one width, in every kind: a graph reads as a set only
   when the boxes are the same size, and rows clamp gracefully inside it
   (value spans keep their minimums, labels ellipsize). 260 covers the
   common row shapes; extreme labels compress rather than widen. */
inline constexpr float kNodeUniformWidthGraph = 260.f;

inline float GetNodeWidth(const Graph::Node& node)
{
    (void)node;
    return SnapUpToGrid(kNodeUniformWidthGraph);
}

} // namespace GraphNodeMetrics
} // namespace GameEngine
