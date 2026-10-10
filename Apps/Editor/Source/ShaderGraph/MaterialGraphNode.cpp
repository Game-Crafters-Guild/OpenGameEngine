#include "ShaderGraph/MaterialGraphNode.h"

#include "Graph/GraphNodeMetrics.h"
#include "Graph/GraphNodeSummary.h"
#include "Graph/GraphPortDropSlot.h"
#include "Graph/GraphVariableValues.h"
#include "Graph/NodeColorSettings.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace GameEngine {
namespace {

/* Parameter nodes bind a graph variable; their value lives in
   model->Variables, never in node params. */
bool NodeIsMaterialParameter(const Graph::Node& node)
{
    return node.TypeId == "FloatParameter" || node.TypeId == "Vec2Parameter" ||
           node.TypeId == "Vec3Parameter" || node.TypeId == "Vec4Parameter" ||
           node.TypeId == "ColorParameter";
}

int VariableComponentCountForType(const std::string& typeId)
{
    if (typeId == "FloatParameter")
        return 1;
    if (typeId == "Vec2Parameter")
        return 2;
    if (typeId == "Vec4Parameter")
        return 4;
    return 3; // Vec3Parameter / ColorParameter
}

/* The detail row's value column holds at most one swatch; find it or make it. */
UIElement* EnsureDetailSwatch(UIElement& host)
{
    for (const auto& child : host.GetChildren())
    {
        if (child && child->HasClass("graph-detail-swatch"))
            return child.get();
    }
    /* Its own class, not the node-body swatch's: this one fills a detail row's
       value column, not a whole node face. */
    auto el = std::make_unique<UIElement>();
    el->AddClass("graph-detail-swatch");
    el->AddClass("graph-node-detail");
    UIElement* swatch = el.get();
    host.AddChild(std::move(el));
    return swatch;
}

std::uint32_t PackSrgbSwatchArgb(const float* rgb)
{
    auto toU8 = [](float v) -> std::uint32_t
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<std::uint32_t>(v * 255.0f + 0.5f);
    };
    return (255u << 24) | (toU8(rgb[0]) << 16) | (toU8(rgb[1]) << 8) | toU8(rgb[2]);
}

} // namespace

MaterialGraphNode::MaterialGraphNode()
{
    AddClass("material-graph-node");
}

bool MaterialGraphNode::WantsPreviewPlate(const Graph::Node& node, bool expandedView) const
{
    // Collapsed nodes have no room. The Output node's preview IS the graph's
    // main preview sphere, and a texture node's plate is its texture.
    return expandedView && node.TypeId != "SurfaceOutput" && !IsTextureValueNode(node);
}

float MaterialGraphNode::ReservedBlockHeight(const Graph::Node& node, bool expandedView) const
{
    // Texture nodes carry their square drop slot in any view: the slot IS the
    // preview, so it owns the reserved block the plate would otherwise take.
    if (IsTextureValueNode(node))
        return GraphNodeMetrics::GetNodeWidth(node);
    if (!WantsPreviewPlate(node, expandedView))
        return 0.f;
    // Full-bleed square plate, so the block is exactly as tall as the node is wide.
    return GraphNodeMetrics::GetNodeWidth(node);
}

int MaterialGraphNode::SyntheticDetailRowCount(const Graph::Node& node, bool expandedView) const
{
    /* ColorConstant's is not gated on the expanded view: its r/g/b rows show in
       both views, and the colour they describe should show with them. */
    if (node.TypeId == "ColorConstant")
        return 1;
    return (expandedView && NodeIsMaterialParameter(node)) ? 1 : 0;
}

std::string MaterialGraphNode::SyntheticDetailRowLabel(const Graph::Node& node,
                                                       int extraIndex) const
{
    (void)extraIndex; // one synthetic row: the Value row
    if (node.TypeId == "ColorConstant")
        return "Color";
    return NodeIsMaterialParameter(node) ? "Value" : std::string();
}

void MaterialGraphNode::OpenColorConstantPicker()
{
    const OpenColorPickerWindowFn open = m_OpenColorPicker;
    if (!open)
        return;

    /* Nothing here may outlive this call through `this` or the swatch element:
       the picker is a separate window, and node slots are pooled — recycling one
       while the picker is open would leave the callbacks writing through a dead
       node, or into whichever node inherited the slot. Everything the write
       needs is captured by value and the node is re-found by id, exactly as
       CommitParams does for the same reason. */
    Graph::Model* model = EditHost().Model;
    const std::string nodeId = GetModelNodeId();
    const Graph::Node* node = model ? model->FindNode(nodeId) : nullptr;
    if (!node)
        return;
    const float rgb[3] = {static_cast<float>(node->Parameters.GetFloat("r", 1.0)),
                          static_cast<float>(node->Parameters.GetFloat("g", 1.0)),
                          static_cast<float>(node->Parameters.GetFloat("b", 1.0))};
    /* The swatch is 8-bit sRGB, so a colour brighter than white cannot survive
       the trip on its own: 1.45 would come back 1.0, quantised. The picker
       carries an HDR multiplier for exactly this — split the value into a
       normalised colour the swatch can hold and the factor it was scaled by,
       and put them back together on write. */
    const float peak = std::max({rgb[0], rgb[1], rgb[2], 1.0f});
    const float normalized[3] = {rgb[0] / peak, rgb[1] / peak, rgb[2] / peak};
    const std::uint32_t original = PackSrgbSwatchArgb(normalized);
    const float originalIntensity = peak;

    auto undo = EditHost().Undo;
    auto onChanged = EditHost().OnChanged;
    auto write = [model, nodeId, onChanged](std::uint32_t argb, float intensity)
    {
        if (!model)
            return;
        Graph::Node* live = model->FindNode(nodeId);
        if (!live)
            return; // node deleted while the picker was open
        const double scale = static_cast<double>(intensity > 0.f ? intensity : 1.f);
        live->Parameters["r"] = (static_cast<double>((argb >> 16) & 0xFFu) / 255.0) * scale;
        live->Parameters["g"] = (static_cast<double>((argb >> 8) & 0xFFu) / 255.0) * scale;
        live->Parameters["b"] = (static_cast<double>(argb & 0xFFu) / 255.0) * scale;
        if (onChanged)
            onChanged();
    };

    /* The picker previews continuously and commits once: the live callback
       writes straight through, apply and cancel each land one undo entry. */
    ColorPickerCallbacks callbacks;
    callbacks.onValueChanging = [write](std::uint32_t argb, float intensity)
    { write(argb, intensity); };
    callbacks.onApply = [write, undo](std::uint32_t argb, float intensity)
    {
        if (undo)
            undo("Set Node Color", [write, argb, intensity]() { write(argb, intensity); });
        else
            write(argb, intensity);
    };
    callbacks.onCancel = [write, undo, original, originalIntensity]()
    {
        if (undo)
            undo("Set Node Color",
                 [write, original, originalIntensity]() { write(original, originalIntensity); });
        else
            write(original, originalIntensity);
    };
    open(original, originalIntensity, std::move(callbacks));
}

void MaterialGraphNode::BindSyntheticDetailRow(const Graph::Node& node, int extraIndex, Label* label,
                                           UIElement* host)
{
    (void)extraIndex; // one synthetic row: the Value row
    if (node.TypeId == "ColorConstant")
    {
        if (!host)
            return;
        if (label)
            label->SetTooltip("Click the swatch to pick the node's colour");
        const float rgb[3] = {static_cast<float>(node.Parameters.GetFloat("r", 1.0)),
                              static_cast<float>(node.Parameters.GetFloat("g", 1.0)),
                              static_cast<float>(node.Parameters.GetFloat("b", 1.0))};
        const std::uint32_t argb = PackSrgbSwatchArgb(rgb);
        UIElement* swatch = EnsureDetailSwatch(*host);
        UI::Layout::SetElementHidden(*swatch, false);
        swatch->Overrides().Set(Style::BackgroundColor, argb);

        /* Rebound on every change, so the handler is registered once and reads
           the picker's colour straight into the node's parameters. */
        if (!swatch->HasClass("graph-detail-swatch-pickable"))
        {
            swatch->AddClass("graph-detail-swatch-pickable");
            swatch->SetTooltip("Pick colour");
            swatch->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
            {
                if (e.Button != 0)
                    return;
                e.Stop();
                OpenColorConstantPicker();
            });
        }
        swatch->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        return;
    }
    if (!NodeIsMaterialParameter(node))
        return;
    if (label)
        label->SetTooltip("Default value of the parameter's variable");
    if (!host)
        return;

    const Graph::Variable* variable = FindNodeVariable(EditHost().Model, node.Id);
    if (node.TypeId == "ColorParameter")
    {
        /* Read-only swatch mirroring the variable's color; editing happens on
           the Variables board, whose picker writes the same variable. */
        UIElement* swatch = EnsureDetailSwatch(*host);
        UI::Layout::SetElementHidden(*swatch, false);
        float rgb[3] = {1.f, 1.f, 1.f};
        if (variable)
            ParseVariableFloats(variable->Value, rgb, 3);
        swatch->Overrides().Set(Style::BackgroundColor, PackSrgbSwatchArgb(rgb));
        swatch->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        return;
    }

    const int components = VariableComponentCountForType(node.TypeId);
    if (components == 1)
    {
        FloatField* field = EnsureRowFloatField(*host);
        if (!field)
            return;
        /* Variable values are user-defined: no schema range. */
        field->ClearValueRange();
        float value = 0.f;
        if (variable)
            ParseVariableFloats(variable->Value, &value, 1);
        if (!RowFieldHoldsFocus(field))
            field->SetValueWithoutNotify(value);
        field->SetOnValueChanged([this](const float& v)
        {
            CommitVariable([value = FormatVariableFloats(&v, 1)](Graph::Variable& variable)
                           { variable.Value = value; });
        });
        return;
    }
    if (components == 3)
    {
        Vector3Field* vec = EnsureRowVec3Field(*host);
        if (!vec)
            return;
        /* Variable values are user-defined: no schema range. */
        vec->ClearComponentValueRange();
        float values[3] = {0.f, 0.f, 0.f};
        if (variable)
            ParseVariableFloats(variable->Value, values, 3);
        if (!RowFieldHoldsFocus(vec))
        {
            vec->SetOnValueChanged({});
            vec->SetValue({values[0], values[1], values[2]});
        }
        vec->SetOnValueChanged([this](const Rendering::Vector3& v)
        {
            const float values[3] = {v.x, v.y, v.z};
            CommitVariable([value = FormatVariableFloats(values, 3)](Graph::Variable& variable)
                           { variable.Value = value; });
        });
        return;
    }

    /* float2 / float4: the raw comma-separated list. */
    TextField* field = EnsureRowTextField(*host);
    if (!field)
        return;
    if (!RowFieldHoldsFocus(field))
        field->SetValueWithoutNotify(variable ? variable->Value : std::string());
    field->SetOnValueChanged([this](const std::string& v)
    {
        CommitVariable([v](Graph::Variable& variable) { variable.Value = v; });
    });
}

void MaterialGraphNode::BindDetailSlot(const Graph::Node& node, GraphPortDropSlot& slot)
{
    if (BindTextureSlot(node, slot))
    {
        BindPreviewPlate(node); // hides the plate — a texture node reserves none
        return;
    }
    GraphPortedNode::BindDetailSlot(node, slot);
    BindPreviewPlate(node);
}

bool MaterialGraphNode::BindTextureSlot(const Graph::Node& node, GraphPortDropSlot& slot)
{
    if (!IsTextureValueNode(node))
        return false;

    auto pathIt = node.Parameters.find("texturePath");
    std::string path =
        (pathIt != node.Parameters.end()) ? pathIt->second.ToString() : std::string();
    if (path.empty())
    {
        auto texIt = node.Parameters.find("texture");
        if (texIt != node.Parameters.end())
            path = texIt->second.ToString();
    }
    UI::Layout::SetBackgroundPath(slot, path);
    UI::Layout::SetElementHidden(slot, false);
    slot.Bind(node.Id, EditHost().Model, EditHost().Undo, EditHost().OnChanged);
    slot.SetPickerServices(EditHost().Assets, EditHost().Thumbnails);

    /* Square slot, framed exactly like the preview plate would be: centred in
       the block ReservedBlockHeight reserved at the bottom of the node. */
    const float layoutHeight = RectHeightGraph(node);
    const float nodeWidth = GraphNodeMetrics::GetNodeWidth(node);
    const float inset = GraphNodeMetrics::kNodeBlockInsetGraph;
    const float side = nodeWidth - 2.f * inset;
    if (layoutHeight <= 0.f || nodeWidth <= 0.f || side <= 0.f)
        return true;
    slot.Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionTop,
             StyleLength::Percent((DetailBlockTopGraph(node) + inset) / layoutHeight * 100.f))
        .Set(Style::Height, StyleLength::Percent(side / layoutHeight * 100.f))
        .Set(Style::PositionLeft, StyleLength::Percent(inset / nodeWidth * 100.f))
        .Set(Style::Width, StyleLength::Percent(side / nodeWidth * 100.f));
    slot.MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    return true;
}

void MaterialGraphNode::BindPreviewPlate(const Graph::Node& node)
{
    const bool wants = WantsPreviewPlate(node, EditHost().ExpandedView);
    const Editor::GraphNodePreviewBinding preview =
        (wants && m_PreviewLookup) ? m_PreviewLookup(node.Id)
                                   : Editor::GraphNodePreviewBinding{};
    if (!wants)
    {
        if (m_PreviewPlate)
            UI::Layout::SetElementHidden(*m_PreviewPlate, true);
        return;
    }

    if (!m_PreviewPlate)
    {
        auto plate = std::make_unique<UIElement>();
        plate->AddClass("graph-node-preview-plate");
        plate->AddClass("graph-node-detail");
        m_PreviewPlate = plate.get();
        InsertBelowChrome(std::move(plate));
    }

    const float layoutHeight = RectHeightGraph(node);
    const float nodeWidth = GraphNodeMetrics::GetNodeWidth(node);
    if (layoutHeight <= 0.f || nodeWidth <= 0.f)
        return;

    /* Full-bleed and square. An inset would frame the rendered image in node
       body, which reads as a margin around a picture rather than as a viewport.
       Anchored to the node's bottom and given one node width of height, rather
       than spanning from the detail rows down: GetNodeHeight snaps the rect up
       to the grid, and that slack has to land somewhere. Above the plate it is
       invisible node body; below it, it would be a seam — and pinning all four
       edges would stretch the square by exactly that remainder. */
    m_PreviewPlate->Overrides()
        .Set(Style::PositionTop, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Percent(0.f))
        .Set(Style::PositionLeft, StyleLength::Percent(0.f))
        .Set(Style::PositionRight, StyleLength::Percent(0.f))
        .Set(Style::Height, StyleLength::Percent(nodeWidth / layoutHeight * 100.f))
        .Set(Style::Width, StyleLength::Auto());
    UI::Layout::SetElementHidden(*m_PreviewPlate, false);

    /* ReservedBlockHeight reserved this block from the node model alone, so the
       plate has to fill it whether or not the atlas has a cell yet: hiding it on
       a missing cell would leave a node-width-tall hole. Until the cell arrives
       the plate shows the bare backdrop CSS gives it. */
    if (preview.Resource.empty())
    {
        UI::Layout::ClearBackgroundOverride(*m_PreviewPlate);
        return;
    }

    UI::Layout::SetBackgroundResourceName(*m_PreviewPlate, preview.Resource);
    /* Sprite addressing into the panel's preview atlas: background-size scales
       the atlas so one cell fills the plate, background-position slides this
       node's cell into it. Must be set AFTER SetBackgroundResourceName, which
       force-overrides background-size to Contain. The atlas frames the sphere
       at render time, so the plate shows its cell 1:1 — no oversize crop. */
    m_PreviewPlate->Overrides()
        .Set(Style::BackgroundSize,
             BackgroundSizeValue{BackgroundSizeMode::Explicit, preview.SizeXPercent, true,
                                 preview.SizeYPercent, true})
        .Set(Style::BackgroundPosition,
             BackgroundPositionValue{preview.PosXPercent, true, preview.PosYPercent, true});
}

} // namespace GameEngine

namespace RegisterGraphElements
{
static auto s_reg_materialGraphNode =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::MaterialGraphNode>(
        "MaterialGraphNode",
        []() { return std::make_unique<GameEngine::MaterialGraphNode>(); })
        .TagAlias("materialgraphnode");
}
