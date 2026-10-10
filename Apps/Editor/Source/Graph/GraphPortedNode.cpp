#include "Graph/GraphPortedNode.h"

#include <Types/ParseNumber.h>

#include "Core/Application.h"
#include "Editor/EditorPaths.h"
#include "Graph/GraphInspectorRows.h"
#include "Graph/GraphNodeIcons.h"
#include "Graph/GraphNodeMetrics.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphPortLabels.h"
#include "Graph/GraphNodeSummary.h"
#include "Graph/GraphPort.h"
#include "Graph/GraphPortDropSlot.h"
#include "Graph/GraphVariableValues.h"
#include "Graph/NodeColorSettings.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Types/StringId.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UITemplateNode.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine {

namespace
{
/** True only while the pointer is captured, i.e. the user is dragging this
    field. Value-CHANGING also fires for programmatic updates — a range clamp on
    bind, a re-seed on relayout — and writing the model from one of those
    corrupts it: an authored 1.45 comes back clamped and 8-bit-quantised, and
    because the write re-binds, the value keeps drifting. Only a real drag may
    write. */
bool UserIsDraggingField(const UIElement* field)
{
    if (!field)
        return false;
    const UIManager* ui = field->GetOwnerManager();
    return ui && ui->IsMouseCaptured();
}
} // namespace

namespace {

// Relative to the editor install-assets root (Contents/Resources/Assets inside
// a macOS bundle, <exe>/Assets elsewhere) — never exe-relative directly.
constexpr const char* kPortedNodeTemplateRel = "UI/Graph/graph-ported-node.uxml";

bool IsShareableRootTag(std::string_view tagLower)
{
    return tagLower == "graphportednode" || tagLower == "graphnode";
}

std::unique_ptr<UIElement> InstantiateShareable(const UITemplateNode& src)
{
    std::unique_ptr<UIElement> out;
    if (!IsShareableRootTag(src.TagName))
        out = UIRegistration::ElementFactoryRegistry::Instance().Create(src.TagName);
    if (!out)
        out = std::make_unique<UIElement>();

    for (const auto& cls : src.Classes)
        out->AddClass(cls);
    for (const auto& child : src.Children)
    {
        if (child)
            out->AddChild(InstantiateShareable(*child));
    }
    return out;
}

const UITemplateNode* LoadPortedInnerTemplate()
{
    static std::unique_ptr<UITemplateNode> s_Root;
    static bool s_Tried = false;
    if (s_Tried)
        return s_Root.get();
    s_Tried = true;

    const auto path = Editor::GetEditorGlobalPaths().installAssetsRoot / kPortedNodeTemplateRel;
    if (!UIParsing::XMLParser::ParseLayoutTemplateFromFile(path.string(), s_Root) || !s_Root)
    {
        Logger::Log::Warning("GraphPortedNode: inner template '{}' missing or unreadable; using C++ body",
                             path.generic_string());
        s_Root.reset();
        return nullptr;
    }
    Logger::Log::Info("GraphPortedNode: inner template loaded from '{}'", path.generic_string());
    return s_Root.get();
}

std::unique_ptr<UIElement> MakeFallbackBody()
{
    auto body = std::make_unique<UIElement>();
    body->AddClass("graph-node-body");

    auto header = std::make_unique<UIElement>();
    header->AddClass("graph-node-header");
    auto icon = std::make_unique<UIElement>();
    icon->AddClass("graph-node-icon");
    header->AddChild(std::move(icon));
    auto title = std::make_unique<Label>();
    title->AddClass("graph-node-title");
    title->AddClass("graph-node-detail");
    header->AddChild(std::move(title));

    auto content = std::make_unique<UIElement>();
    content->AddClass("graph-node-content");
    content->AddClass("graph-node-detail");
    auto preview = std::make_unique<GraphPortDropSlot>();
    preview->AddClass("graph-node-preview");
    preview->AddClass("graph-node-detail");
    content->AddChild(std::move(preview));
    auto value = std::make_unique<Label>();
    value->AddClass("graph-node-value");
    content->AddChild(std::move(value));

    body->AddChild(std::move(header));
    body->AddChild(std::move(content));
    return body;
}

void DecorateInlineField(UIElement& el)
{
    el.AddClass("graph-inline-field");
    el.AddClass("graph-node-detail");
    if (auto* field = dynamic_cast<FloatField*>(&el))
        field->EnableDragToChange();
    if (auto* vec = dynamic_cast<Vector3Field*>(&el))
    {
        vec->SetLabelDragEnabled(false);
        vec->EnableComponentDragToChange();
    }
}

/* An editor surface claims its own primary presses — padding included, and
   any moment a child widget cannot (zero size, font not staged yet) — the
   same consumption every standard control uses (capture + stop). Presses
   that reach the canvas are therefore always canvas gestures, and it never
   needs to recognize editor widgets by class name to keep node drags off
   them. Right/middle presses bubble on: pan and the context menu work over
   editors. */
void ClaimPrimaryPresses(UIElement& host)
{
    host.RegisterEventHandler(kEventMouseDown, [](UIEvent& e)
    {
        if (e.Button != Input::kMouseButton_Left)
            return;
        if (e.CurrentTarget)
            e.Capture(e.CurrentTarget);
        e.Stop();
    });
}

bool FieldHoldsFocus(UIElement* el)
{
    if (!el)
        return false;
    UIManager* ui = el->GetOwnerManager();
    if (ui)
    {
        const std::string& focusId = ui->GetFocusedElementId();
        if (el->IsFocusTargetForId(focusId))
            return true;
    }
    for (const auto& child : el->GetChildren())
    {
        if (child && FieldHoldsFocus(child.get()))
            return true;
    }
    return false;
}

void HideElement(UIElement* el, bool force = false)
{
    if (el && (force || !FieldHoldsFocus(el)))
        UI::Layout::SetElementHidden(*el, true);
}

void ShowElement(UIElement* el)
{
    if (el)
        UI::Layout::SetElementHidden(*el, false);
}

void ClearElementIds(UIElement& el)
{
    if (!el.GetId().empty())
        el.SetId({});
    for (const auto& child : el.GetChildren())
    {
        if (child)
            ClearElementIds(*child);
    }
}

void HideDirectChildren(UIElement& host, bool force = false)
{
    for (const auto& child : host.GetChildren())
    {
        if (child && (force || !FieldHoldsFocus(child.get())))
            UI::Layout::SetElementHidden(*child, true);
    }
}

float ReadFloatParam(const Graph::Node& node, const char* key, float fallback)
{
    return static_cast<float>(node.Parameters.GetFloat(key, fallback));
}

std::uint32_t PackSrgbArgb(float r, float g, float b)
{
    auto toU8 = [](float v) -> std::uint32_t
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<std::uint32_t>(v * 255.0f + 0.5f);
    };
    return (255u << 24) | (toU8(r) << 16) | (toU8(g) << 8) | toU8(b);
}

int ParseFloatsFromText(const std::string& text, float* out, int count)
{
    int n = 0;
    const char* cursor = text.c_str();
    const char* end = cursor + text.size();
    while (cursor < end && n < count)
    {
        while (cursor < end && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
                                 *cursor == '-' || *cursor == '+' || *cursor == '.'))
            ++cursor;
        if (cursor >= end)
            break;
        char* parsedEnd = nullptr;
        const float value = std::strtof(cursor, &parsedEnd);
        if (parsedEnd == cursor)
            break;
        out[n++] = value;
        cursor = parsedEnd;
    }
    return n;
}

void ReadVecParam(const Graph::GraphValue& value, float* out, int count, float fallback)
{
    for (int i = 0; i < count; ++i)
        out[i] = fallback;
    if (const Graph::GraphObject* object = value.TryObject())
    {
        static constexpr const char* kKeys[] = {"x", "y", "z", "w"};
        for (int i = 0; i < count; ++i)
            out[i] = static_cast<float>(object->GetFloat(kKeys[i], out[i]));
        return;
    }
    if (const std::vector<Graph::GraphValue>* list = value.TryList())
    {
        const int n = std::min(count, static_cast<int>(list->size()));
        for (int i = 0; i < n; ++i)
            out[i] = static_cast<float>((*list)[static_cast<size_t>(i)].AsFloat(out[i]));
        return;
    }
    ParseFloatsFromText(value.ToString(), out, count);
}

std::string FormatGlslVec(int count, const float* values)
{
    char buf[96];
    if (count == 2)
        std::snprintf(buf, sizeof(buf), "vec2(%g, %g)", values[0], values[1]);
    else if (count == 3)
        std::snprintf(buf, sizeof(buf), "vec3(%g, %g, %g)", values[0], values[1], values[2]);
    else
        std::snprintf(buf, sizeof(buf), "vec4(%g, %g, %g, %g)", values[0], values[1], values[2], values[3]);
    return buf;
}

/* Clamp a committed "vecN(...)" text per component when the value has a
   range; anything else passes through untouched. */
std::string ClampVecTextToRange(const std::string& text, const std::optional<NodeValueRange>& range)
{
    if (!range)
        return text;
    const int count = GraphNodeMetrics::VecTextComponentCount(text);
    if (count < 2)
    {
        /* Scalars reach this row too: a numeric parameter read from a graph
           file stays a string until something rewrites it, so the text row is
           the only clamp it gets. Non-numeric text passes through. */
        const char* begin = text.c_str();
        char* end = nullptr;
        const float scalar = std::strtof(begin, &end);
        if (end == begin || *end != '\0')
            return text;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", std::clamp(scalar, range->Min, range->Max));
        return buf;
    }
    float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ReadVecParam(Graph::GraphValue(text), values, count, 0.0f);
    for (int i = 0; i < count; ++i)
        values[i] = std::clamp(values[i], range->Min, range->Max);
    return FormatGlslVec(count, values);
}

enum class InlinePortKind
{
    None,
    Float,
    Vec2,
    Vec3,
    Vec4,
    Bool,
    String
};

InlinePortKind ClassifyPortType(const std::string& dataType)
{
    if (dataType == "float")
        return InlinePortKind::Float;
    if (dataType == "float2" || dataType == "vec2")
        return InlinePortKind::Vec2;
    if (dataType == "float3" || dataType == "vec3")
        return InlinePortKind::Vec3;
    if (dataType == "float4" || dataType == "vec4")
        return InlinePortKind::Vec4;
    if (dataType == "bool")
        return InlinePortKind::Bool;
    if (dataType == "string")
        return InlinePortKind::String;
    return InlinePortKind::None;
}

UIElement* EnsurePortEditorHost(GraphPort& port)
{
    for (const auto& child : port.GetChildren())
    {
        if (child && child->HasClass("graph-inline-editors"))
            return child.get();
    }
    auto host = std::make_unique<UIElement>();
    host->AddClass("graph-inline-editors");
    host->AddClass("graph-node-detail");
    ClaimPrimaryPresses(*host);
    UIElement* raw = host.get();
    port.AddChild(std::move(host));
    return raw;
}

template <typename T, typename Make>
T* EnsureDirectChild(UIElement& host, const char* className, Make&& make)
{
    for (const auto& child : host.GetChildren())
    {
        if (!child || !child->HasClass(className))
            continue;
        UI::Layout::SetElementHidden(*child, false);
        return dynamic_cast<T*>(child.get());
    }
    auto el = make();
    DecorateInlineField(*el);
    el->AddClass(className);
    T* raw = el.get();
    host.AddChild(std::move(el));
    return raw;
}

FloatField* EnsureFloatChild(UIElement& host)
{
    return EnsureDirectChild<FloatField>(host, "float-field", []() {
        return std::make_unique<FloatField>();
    });
}

/* Fields are pooled and rebound across nodes, so a bind without a range must
   clear any range a previous binding left behind. */
void ApplyFieldRange(FloatField* field, const std::optional<NodeValueRange>& range)
{
    if (!field)
        return;
    if (range)
        field->SetValueRange(range->Min, range->Max);
    else
        field->ClearValueRange();
}

/* Same pooled-rebind contract as the scalar overload: a vec3 surface carries
   the schema range on each of its three components. */
void ApplyFieldRange(Vector3Field* field, const std::optional<NodeValueRange>& range)
{
    if (!field)
        return;
    if (range)
        field->SetComponentValueRange(range->Min, range->Max);
    else
        field->ClearComponentValueRange();
}

Vector3Field* EnsureVec3Child(UIElement& host)
{
    return EnsureDirectChild<Vector3Field>(host, "vector3-field", []() {
        return std::make_unique<Vector3Field>();
    });
}

FloatField* EnsureNthFloatChild(UIElement& host, size_t index)
{
    std::vector<FloatField*> floats;
    for (const auto& child : host.GetChildren())
    {
        if (auto* field = dynamic_cast<FloatField*>(child.get()))
            floats.push_back(field);
    }
    while (floats.size() <= index)
    {
        auto field = std::make_unique<FloatField>();
        DecorateInlineField(*field);
        floats.push_back(field.get());
        host.AddChild(std::move(field));
    }
    ShowElement(floats[index]);
    return floats[index];
}

Checkbox* EnsureCheckboxChild(UIElement& host)
{
    return EnsureDirectChild<Checkbox>(host, "checkbox", []() {
        auto box = std::make_unique<Checkbox>();
        box->SetText({});
        return box;
    });
}

TextField* EnsureTextChild(UIElement& host)
{
    return EnsureDirectChild<TextField>(host, "text-field", []() {
        return std::make_unique<TextField>();
    });
}

Dropdown* EnsureDropdownChild(UIElement& host)
{
    return EnsureDirectChild<Dropdown>(host, "graph-enum-dropdown", []() {
        return std::make_unique<Dropdown>();
    });
}

void ClearPortInlineEditor(GraphPort& port)
{
    UIElement* host = nullptr;
    for (const auto& child : port.GetChildren())
    {
        if (child && child->HasClass("graph-inline-editors"))
            host = child.get();
    }
    HideElement(host);
    if (host)
    {
        host->Overrides().Reset(Style::Width);
        host->Overrides().Reset(Style::MaxWidth);
    }
    port.RemoveClass("has-inline-editor");
    port.RemoveClass("bool-editor");
    port.RemoveClass("vec-editor");
    port.RemoveClass("graph-single-inline");
}

/* Compact single-value node: base height and exactly one editable input whose
   editor can fill the whole row (a lone Name or Duration box). Marked with
   "graph-single-inline" purely as a regression guard — the class must NOT
   grow CSS geometry of its own (GraphNodeLayoutTests ports this). */
bool IsCompactSingleInlineNode(const Graph::Node& node)
{
    if (GraphNodeMetrics::GetNodeBaseHeight(node) != GraphNodeMetrics::kNodeHeight)
        return false;
    int editable = 0;
    InlinePortKind kind = InlinePortKind::None;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction != Graph::PortDirection::In)
            continue;
        const InlinePortKind classified = ClassifyPortType(port.DataType);
        if (classified == InlinePortKind::None)
            continue;
        ++editable;
        kind = classified;
    }
    return editable == 1 && (kind == InlinePortKind::String || kind == InlinePortKind::Float);
}

} // namespace

GraphPortedNode::GraphPortedNode()
{
    AddClass("graph-ported-node");
    BuildInnerTree();
    /* Last child on purpose: primitives paint in child order, and the engine
       emits an element's outline before its children — a node-level outline
       ends up under the header. The ring is a border-only overlay that paints
       above everything, carrying the hover/selection line. */
    auto ring = std::make_unique<UIElement>();
    ring->AddClass("graph-node-ring");
    m_Ring = ring.get();
    AddChild(std::move(ring));
}

void GraphPortedNode::BuildInnerTree()
{
    /* The slot is a node-root child: its layout overrides are percentages of
       the whole node, the same frame the preview plate uses. A template-supplied
       slot found deeper in the tree is re-homed rather than duplicated. */
    auto ensurePreview = [this]()
    {
        if (m_Preview)
        {
            if (UIElement* parent = m_Preview->GetParent(); parent && parent != this)
                AddChild(parent->TakeChild(m_Preview));
            return;
        }
        auto slot = std::make_unique<GraphPortDropSlot>();
        slot->AddClass("graph-node-preview");
        slot->AddClass("graph-node-detail");
        m_Preview = slot.get();
        AddChild(std::move(slot));
    };

    if (const UITemplateNode* templ = LoadPortedInnerTemplate())
    {
        if (IsShareableRootTag(templ->TagName))
        {
            for (const auto& child : templ->Children)
            {
                if (child)
                    AddChild(InstantiateShareable(*child));
            }
        }
        else
        {
            AddChild(InstantiateShareable(*templ));
        }
        BindSlots();
        if (m_Body)
        {
            ensurePreview();
            return;
        }
        RemoveAllChildren();
    }

    AddChild(MakeFallbackBody());
    BindSlots();
    ensurePreview();
}

void GraphPortedNode::BindSlots()
{
    m_Body = nullptr;
    m_Header = nullptr;
    m_Preview = nullptr;
    m_Icon = nullptr;
    m_Title = nullptr;
    m_Value = nullptr;
    for (const auto& child : GetChildren())
    {
        if (child)
            BindSlotsFrom(*child);
    }
}

void GraphPortedNode::BindSlotsFrom(UIElement& el)
{
    if (!m_Body && el.HasClass("graph-node-body"))
        m_Body = &el;
    if (!m_Header && el.HasClass("graph-node-header"))
        m_Header = &el;
    if (!m_Icon && el.HasClass("graph-node-icon"))
        m_Icon = &el;
    if (!m_Preview)
    {
        if (auto* slot = dynamic_cast<GraphPortDropSlot*>(&el))
            m_Preview = slot;
    }
    if (!m_Title && el.HasClass("graph-node-title"))
        m_Title = dynamic_cast<Label*>(&el);
    if (!m_Value && el.HasClass("graph-node-value"))
        m_Value = dynamic_cast<Label*>(&el);
    if (!m_ValueEditorHost && el.HasClass("graph-inline-editors"))
    {
        UIElement* parent = el.GetParent();
        if (!parent || !parent->HasClass("graph-port"))
            m_ValueEditorHost = &el;
    }
    for (const auto& child : el.GetChildren())
    {
        if (child)
            BindSlotsFrom(*child);
    }
}

void GraphPortedNode::GrowPorts(std::vector<GraphPort*>& ports, int count, bool isInput)
{
    const int needed = std::max(0, count);
    while (static_cast<int>(ports.size()) < needed)
    {
        const int index = static_cast<int>(ports.size());
        auto port = std::make_unique<GraphPort>();
        port->SetPortId((isInput ? "in" : "out") + std::to_string(index));
        port->AddClass(isInput ? "graph-port-input" : "graph-port-output");
        auto label = std::make_unique<Label>();
        label->AddClass("graph-port-label");
        label->AddClass("graph-node-detail");
        label->SetText((isInput ? "In " : "Out ") + std::to_string(index));
        port->AddChild(std::move(label));
        ports.push_back(port.get());
        AddChild(std::move(port));
    }
}

void GraphPortedNode::EnsurePortCount(int inputCount, int outputCount)
{
    GrowPorts(m_InputPorts, inputCount, true);
    GrowPorts(m_OutputPorts, outputCount, false);
}

void GraphPortedNode::BindModelPorts(const Graph::Node& node)
{
    int inputCount = 0;
    int outputCount = 0;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction == Graph::PortDirection::In)
            ++inputCount;
        else
            ++outputCount;
    }

    EnsurePortCount(inputCount, outputCount);

    int inputIndex = 0;
    int outputIndex = 0;
    for (const Graph::Port& port : node.Ports)
    {
        GraphPort* widget = port.Direction == Graph::PortDirection::In
            ? InputPortAt(static_cast<size_t>(inputIndex++))
            : OutputPortAt(static_cast<size_t>(outputIndex++));
        if (!widget)
            continue;
        widget->SetPortId(port.Id);
        const std::string portName = GraphNodeMetrics::PortDisplayLabel(port);
        /* Node labels follow the abbreviation rule (GraphPortLabels.h); the
           tooltip and the inspector keep the full name. */
        widget->SetDisplayName(GraphPortLabels::AbbreviateForNode(portName));
        std::string tooltip = portName;
        tooltip += port.Direction == Graph::PortDirection::In ? "\nInput connection" : "\nOutput connection";
        if (!port.DataType.empty())
            tooltip += "\nType: " + port.DataType;
        tooltip += port.Direction == Graph::PortDirection::Out
            ? "\nDrag from this port to create a wire."
            : "\nDrop a compatible output wire here.";
        widget->SetTooltip(std::move(tooltip));
        const bool isOutput = port.Direction != Graph::PortDirection::In;
        const int portIndex = isOutput ? outputIndex - 1 : inputIndex - 1;
        const int portCount = isOutput ? outputCount : inputCount;
        const float nodeHeight = RectHeightGraph(node);
        const float nodeWidth = GraphNodeMetrics::GetNodeWidth(node);
        widget->Overrides()
            .Set(Style::PositionTop, StyleLength::Percent(
                GraphNodeMetrics::PortTopPercent(portIndex, portCount, nodeHeight)))
            .Set(Style::Width, StyleLength::Percent(GraphNodeMetrics::PortWidthPercent(nodeWidth)))
            .Set(Style::Height, StyleLength::Percent(
                GraphNodeMetrics::PortHeightPercent(nodeHeight)));
        /* The CSS -2.5% hang assumes the canonical width; the override keeps
           the dot centered on the edge whatever the rect width. */
        if (isOutput)
            widget->Overrides().Set(Style::PositionRight,
                StyleLength::Percent(GraphNodeMetrics::PortSideHangPercent(nodeWidth)));
        else
            widget->Overrides().Set(Style::PositionLeft,
                StyleLength::Percent(GraphNodeMetrics::PortSideHangPercent(nodeWidth)));
        UI::Layout::SetElementHidden(*widget, false);
    }

    for (int i = 0; i < inputCount; ++i)
    {
        GraphPort* inPort = InputPortAt(static_cast<size_t>(i));
        if (!inPort)
            continue;
        if (GraphNodeMetrics::SharedOutNameCharsForInput(node, i) > 0)
            inPort->AddClass("shares-out-row");
        else
            inPort->RemoveClass("shares-out-row");
    }

    for (size_t i = static_cast<size_t>(inputIndex); i < m_InputPorts.size(); ++i)
    {
        if (m_InputPorts[i])
        {
            m_InputPorts[i]->SetTooltip({});
            m_InputPorts[i]->RemoveClass("shares-out-row");
            UI::Layout::SetElementHidden(*m_InputPorts[i], true);
        }
    }
    for (size_t i = static_cast<size_t>(outputIndex); i < m_OutputPorts.size(); ++i)
    {
        if (m_OutputPorts[i])
        {
            m_OutputPorts[i]->SetTooltip({});
            UI::Layout::SetElementHidden(*m_OutputPorts[i], true);
        }
    }
}

void GraphPortedNode::EnsureValueEditorHost()
{
    if (m_ValueEditorHost)
        return;
    auto host = std::make_unique<UIElement>();
    host->AddClass("graph-inline-editors");
    host->AddClass("graph-node-detail");
    ClaimPrimaryPresses(*host);
    m_ValueEditorHost = host.get();
    if (m_Value && m_Value->GetParent())
        m_Value->GetParent()->AddChild(std::move(host));
    else if (m_Body)
        m_Body->AddChild(std::move(host));
    else
        AddChild(std::move(host));
}

void GraphPortedNode::HideValueEditors(bool force)
{
    if (m_ValueEditorHost)
        m_ValueEditorHost->RemoveClass("graph-single-float");
    if (m_ValueEditorHost && (force || !FieldHoldsFocus(m_ValueEditorHost)))
        HideDirectChildren(*m_ValueEditorHost, force);
    HideElement(m_ValueEditorHost, force);
    HideElement(m_ValueVec3, force);
    HideElement(m_FunctionDropdown, force);
    for (FloatField* field : m_ValueFloats)
    {
        if (!field)
            continue;
        if (force || !FieldHoldsFocus(field))
            field->SetOnValueChanged({});
        HideElement(field, force);
    }
    if (m_ValueVec3 && (force || !FieldHoldsFocus(m_ValueVec3)))
        m_ValueVec3->SetOnValueChanged({});
    if (m_FunctionDropdown && (force || !FieldHoldsFocus(m_FunctionDropdown)))
        m_FunctionDropdown->SetOnValueChanged({});
    if (m_ValueText)
    {
        if (force || !FieldHoldsFocus(m_ValueText))
            m_ValueText->SetOnValueChanged({});
        HideElement(m_ValueText, force);
    }
}

void GraphPortedNode::BindDetailSlot(const Graph::Node& node, GraphPortDropSlot& slot)
{
    // The base node has nothing to put in the slot; kinds that do override.
    (void)node;
    slot.Unbind();
    UI::Layout::ClearBackgroundOverride(slot);
    UI::Layout::SetElementHidden(slot, true);
}

float GraphPortedNode::RectHeightGraph(const Graph::Node& node) const
{
    return GraphNodeMetrics::GetNodeHeight(
        node, DrawsInlinePortEditors(node), HasCentralEditors(node), m_EditHost.ExpandedView,
        ReservedBlockHeight(node, m_EditHost.ExpandedView),
        SyntheticDetailRowCount(node, m_EditHost.ExpandedView));
}

float GraphPortedNode::DetailBlockTopGraph(const Graph::Node& node) const
{
    const int rows = GraphNodeMetrics::DetailRowCount(
                         node, DrawsInlinePortEditors(node), HasCentralEditors(node),
                         m_EditHost.ExpandedView) +
        SyntheticDetailRowCount(node, m_EditHost.ExpandedView);
    return GraphNodeMetrics::DetailRowsBottomGraph(node, rows);
}

float GraphPortedNode::EditorStartGraph(const Graph::Node& node, bool drawsInlineEditors) const
{
    int syntheticChars = 0;
    const int extraRows = SyntheticDetailRowCount(node, m_EditHost.ExpandedView);
    for (int i = 0; i < extraRows; ++i)
    {
        const int chars = static_cast<int>(SyntheticDetailRowLabel(node, i).size());
        if (chars > syntheticChars)
            syntheticChars = chars;
    }
    return GraphNodeMetrics::NodeEditorStartGraph(node, drawsInlineEditors,
                                                  HasCentralEditors(node),
                                                  m_EditHost.ExpandedView, syntheticChars);
}

void GraphPortedNode::InsertBelowChrome(std::unique_ptr<UIElement> child)
{
    InsertChild(GetChildren().size() - (m_Ring ? 1u : 0u), std::move(child));
}

UIElement* GraphPortedNode::EnsureDetailRowSlot(size_t row)
{
    while (m_DetailRows.size() <= row)
    {
        auto rowEl = std::make_unique<UIElement>();
        rowEl->AddClass("graph-detail-row");
        rowEl->AddClass("graph-node-detail");
        auto label = std::make_unique<Label>();
        label->AddClass("graph-detail-label");
        label->AddClass("graph-node-detail");
        rowEl->AddChild(std::move(label));
        auto host = std::make_unique<UIElement>();
        host->AddClass("graph-detail-value");
        ClaimPrimaryPresses(*host);
        rowEl->AddChild(std::move(host));
        m_DetailRows.push_back(rowEl.get());
        InsertBelowChrome(std::move(rowEl));
    }
    return m_DetailRows[row];
}

void GraphPortedNode::HideDetailRows(size_t firstUnused)
{
    for (size_t i = firstUnused; i < m_DetailRows.size(); ++i)
    {
        if (m_DetailRows[i])
            UI::Layout::SetElementHidden(*m_DetailRows[i], true);
    }
}

void GraphPortedNode::BindDetailRows(const Graph::Node& node, std::string_view kindId)
{
    const bool drawsInlineEditors = DrawsInlinePortEditors(node);
    const bool hasCentralEditors = HasCentralEditors(node);
    const int extraRows = SyntheticDetailRowCount(node, m_EditHost.ExpandedView);
    if (GraphNodeMetrics::DetailRowCount(node, drawsInlineEditors, hasCentralEditors,
                                         m_EditHost.ExpandedView) + extraRows <= 0)
    {
        HideDetailRows(0);
        return;
    }

    // Rows are node children, so their percentages are of the whole rect.
    const float layoutHeight = RectHeightGraph(node);
    const float nodeWidthGraph = GraphNodeMetrics::GetNodeWidth(node);
    const float editorStartGraph = EditorStartGraph(node, drawsInlineEditors);
    const float halfRow = GraphNodeMetrics::kPortSpacing * 0.5f;
    const NodeTypeMeta* schema = GraphNodeRegistry::Get().FindNodeMeta(kindId, node.TypeId);

    /* Positions row `rowIndex` on the lattice and preps its label/host pair.
       Label text is the caller's: param rows prettify their key, synthetic
       rows take it from SyntheticDetailRowLabel — the same text the value
       column was measured against. */
    auto prepareRow = [&](size_t rowIndex) -> std::pair<Label*, UIElement*>
    {
        UIElement* rowEl = EnsureDetailRowSlot(rowIndex);
        if (!rowEl)
            return {nullptr, nullptr};
        const float centerUnits =
            GraphNodeMetrics::ExpandedRowCenterGraph(node, static_cast<int>(rowIndex));
        rowEl->Overrides()
            .Set(Style::PositionTop,
                 StyleLength::Percent((centerUnits - halfRow) / layoutHeight * 100.f))
            .Set(Style::Height,
                 StyleLength::Percent(GraphNodeMetrics::kPortSpacing / layoutHeight * 100.f));
        UI::Layout::SetElementHidden(*rowEl, false);

        auto* label = dynamic_cast<Label*>(rowEl->GetChildren()[0].get());
        UIElement* host = rowEl->GetChildren()[1].get();
        if (label)
        {
            label->Overrides()
                .Set(Style::PositionLeft,
                     StyleLength::Percent(GraphNodeMetrics::kInlineLabelStartGraph /
                                          nodeWidthGraph * 100.f))
                .Set(Style::MaxWidth,
                     StyleLength::Percent((editorStartGraph -
                                           GraphNodeMetrics::kInlineLabelStartGraph) /
                                          nodeWidthGraph * 100.f));
        }
        if (host)
        {
            HideDirectChildren(*host);
            ShowElement(host);
            host->Overrides()
                .Set(Style::PositionLeft,
                     StyleLength::Percent(editorStartGraph / nodeWidthGraph * 100.f))
                .Set(Style::Width,
                     StyleLength::Percent((nodeWidthGraph - GraphNodeMetrics::kOutGutterGraph -
                                           editorStartGraph) / nodeWidthGraph * 100.f));
            host->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
        }
        return {label, host};
    };

    size_t row = 0;
    GraphNodeMetrics::ForEachDetailParam(
        node, drawsInlineEditors, hasCentralEditors, m_EditHost.ExpandedView,
        [&](const std::string& key, const Graph::GraphValue& value)
        {
            auto [label, host] = prepareRow(row);
            if (!label && !host)
                return;
            if (label)
            {
                const std::string pretty = GraphPortLabels::PrettifyParamKey(key);
                label->SetText(GraphPortLabels::AbbreviateForNode(pretty));
                label->SetTooltip(pretty);
            }
            if (!host)
            {
                ++row;
                return;
            }

            const std::string paramKey = key;
            const std::string text = value.ToString();
            const NodeParamSpec* spec = FindSchemaParam(schema, key);
            const int vecComponents = GraphNodeMetrics::VecTextComponentCount(text);
            if (spec && !spec->Options.empty())
            {
                if (Dropdown* dropdown = EnsureDropdownChild(*host))
                {
                    std::vector<Dropdown::Option> options;
                    options.reserve(spec->Options.size() + 1);
                    int selected = 0;
                    bool found = false;
                    for (const Graph::NodeParamOption& option : spec->Options)
                    {
                        if (option.Value == text)
                        {
                            selected = static_cast<int>(options.size());
                            found = true;
                        }
                        options.push_back({option.Value, option.Label});
                    }
                    /* A stored value the schema no longer lists gets its own
                       entry rather than being silently displayed — and
                       committed — as option 0. */
                    if (!found && !text.empty())
                    {
                        options.push_back({text, text});
                        selected = static_cast<int>(options.size()) - 1;
                    }
                    dropdown->SetOnValueChanged({});
                    if (!FieldHoldsFocus(dropdown))
                        dropdown->SetOptions(options, selected);
                    dropdown->SetOnValueChanged([this, paramKey](const std::string& v)
                    {
                        CommitParam(paramKey, Graph::GraphValue(v));
                    });
                }
            }
            else if (vecComponents == 3)
            {
                if (Vector3Field* vec = EnsureVec3Child(*host))
                {
                    ApplyFieldRange(vec, FindNodeValueRange(schema, paramKey));
                    float xyz[3] = {0.f, 0.f, 0.f};
                    ReadVecParam(value, xyz, 3, 0.f);
                    vec->SetOnValueChanged({});
                    if (!FieldHoldsFocus(vec))
                        vec->SetValue(Rendering::Vector3{xyz[0], xyz[1], xyz[2]});
                    vec->SetOnValueChanged([this, paramKey](const Rendering::Vector3& v)
                    {
                        const float xyzOut[3] = {v.x, v.y, v.z};
                        CommitParam(paramKey, Graph::GraphValue(FormatGlslVec(3, xyzOut)));
                    });
                }
            }
            else if (value.IsBool())
            {
                if (Checkbox* box = EnsureCheckboxChild(*host))
                {
                    box->SetChecked(value.AsBool());
                    box->SetOnValueChanged([this, paramKey](bool checked)
                    {
                        CommitParam(paramKey, Graph::GraphValue(checked));
                    });
                }
            }
            /* A scalar stored as text still gets the float editor: the row is
               the only place these parameters are edited, and a TextField
               silently costs the drag-to-scrub gesture. ColorConstant's r/g/b
               arrive this way. */
            else if (double scalar = 0.0;
                     value.IsNumber() || ParseStrictDecimal(text, scalar))
            {
                if (FloatField* field = EnsureFloatChild(*host))
                {
                    ApplyFieldRange(field, FindNodeValueRange(schema, paramKey));
                    if (!FieldHoldsFocus(field))
                        field->SetValueWithoutNotify(
                            static_cast<float>(value.IsNumber() ? value.AsFloat() : scalar));
                    field->SetOnValueChanging([this, field, paramKey](const float& v)
                    {
                        if (UserIsDraggingField(field))
                            EditLive(paramKey, Graph::GraphValue(v));
                    });
                    field->SetOnValueChanged([this, paramKey](const float& v)
                    {
                        CommitParam(paramKey, Graph::GraphValue(v));
                    });
                }
            }
            else if (IsTextureValueNode(node) && paramKey == "texture")
            {
                /* Sampler choice comes from the graph's declared textures —
                   a dynamic set, so it cannot live in the registry schema. */
                if (Dropdown* dropdown = EnsureDropdownChild(*host))
                {
                    std::vector<std::string> labels;
                    if (m_EditHost.Model)
                    {
                        for (const Graph::Texture& tex : m_EditHost.Model->Textures)
                            labels.push_back(tex.Name);
                    }
                    const std::string current = text;
                    if (!current.empty() &&
                        std::find(labels.begin(), labels.end(), current) == labels.end())
                        labels.push_back(current);
                    int selected = 0;
                    for (size_t i = 0; i < labels.size(); ++i)
                    {
                        if (labels[i] == current)
                            selected = static_cast<int>(i);
                    }
                    dropdown->SetOnValueChanged({});
                    dropdown->SetOptionsFromLabels(labels, selected);
                    dropdown->SetOnValueChanged([this, paramKey](const std::string& v)
                    { CommitParam(paramKey, Graph::GraphValue(v)); });
                }
            }
            else
            {
                if (TextField* field = EnsureTextChild(*host))
                {
                    if (!FieldHoldsFocus(field))
                        field->SetValueWithoutNotify(text);
                    field->SetOnValueChanged(
                        [this, paramKey, range = FindNodeValueRange(schema, paramKey)](
                            const std::string& v)
                        {
                            CommitParam(paramKey, Graph::GraphValue(ClampVecTextToRange(v, range)));
                        });
                }
            }
            ++row;
        });

    for (int extra = 0; extra < extraRows; ++extra)
    {
        auto [label, host] = prepareRow(row);
        if (!label && !host)
            break;
        if (label)
            label->SetText(SyntheticDetailRowLabel(node, extra));
        BindSyntheticDetailRow(node, extra, label, host);
        ++row;
    }

    HideDetailRows(row);
}

void GraphPortedNode::CommitParam(const std::string& key, Graph::GraphValue value)
{
    CommitParams([key, value](Graph::Node& node) { node.Parameters[key] = value; });
}

void GraphPortedNode::EditLive(const std::string& key, Graph::GraphValue value)
{
    EditLive([key, value = std::move(value)](Graph::Node& node) { node.Parameters[key] = value; });
}

void GraphPortedNode::EditLive(const std::function<void(Graph::Node&)>& write)
{
    if (m_BindSuppressDepth > 0 || !write || !m_EditHost.EditLive)
        return;
    m_EditHost.EditLive(GetModelNodeId(), write);
}

void GraphPortedNode::CommitParams(const std::function<void(Graph::Node&)>& write)
{
    if (m_BindSuppressDepth > 0)
        return;
    const std::string nodeId = GetModelNodeId();
    Graph::Model* model = m_EditHost.Model;
    auto onChanged = m_EditHost.OnChanged;
    auto mutate = [model, nodeId, write, onChanged]()
    {
        if (!model)
            return;
        Graph::Node* node = model->FindNode(nodeId);
        if (!node)
            return;
        write(*node);
        if (onChanged)
            onChanged();
    };
    if (m_EditHost.Undo)
        m_EditHost.Undo("Edit Node Value", std::move(mutate));
    else
        mutate();
}

void GraphPortedNode::CommitVariable(const std::function<void(Graph::Variable&)>& write)
{
    if (m_BindSuppressDepth > 0)
        return;
    const std::string nodeId = GetModelNodeId();
    Graph::Model* model = m_EditHost.Model;
    /* A vanished variable would make the mutate a no-op; skip committing so
       the undo stack never records an entry that changes nothing. */
    if (!FindNodeVariable(model, nodeId))
        return;
    auto onChanged = m_EditHost.OnChanged;
    auto mutate = [model, nodeId, write, onChanged]()
    {
        /* Resolved at mutate time: the variableName param (and thus the
           variable this node addresses) can change between undo replays. */
        Graph::Variable* variable = FindNodeVariable(model, nodeId);
        if (!variable)
            return;
        write(*variable);
        if (onChanged)
            onChanged();
    };
    if (m_EditHost.Undo)
        m_EditHost.Undo("Edit Parameter Value", std::move(mutate));
    else
        mutate();
}

FloatField* GraphPortedNode::EnsureRowFloatField(UIElement& host)
{
    return EnsureFloatChild(host);
}

Vector3Field* GraphPortedNode::EnsureRowVec3Field(UIElement& host)
{
    return EnsureVec3Child(host);
}

TextField* GraphPortedNode::EnsureRowTextField(UIElement& host)
{
    return EnsureTextChild(host);
}

bool GraphPortedNode::RowFieldHoldsFocus(UIElement* el)
{
    return FieldHoldsFocus(el);
}

static FloatField* EnsureIndexedFloat(std::vector<FloatField*>& floats, UIElement& host, size_t index)
{
    while (floats.size() <= index)
    {
        auto field = std::make_unique<FloatField>();
        DecorateInlineField(*field);
        floats.push_back(field.get());
        host.AddChild(std::move(field));
    }
    ShowElement(floats[index]);
    return floats[index];
}

bool GraphPortedNode::HasCentralEditors(const Graph::Node& node) const
{
    const std::string& typeId = node.TypeId;
    return typeId == "FloatConstant" || typeId == "Vec2Constant" ||
           typeId == "Vec3Constant" || typeId == "Vec4Constant" ||
           typeId == "ColorConstant" || typeId == "Time" || typeId == "Compare" ||
           typeId == "State" || typeId == "AnyState" || typeId == "Entry" ||
           typeId == "SetVariable" || typeId == "GetVariable";
}

void GraphPortedNode::BindValueEditors(const Graph::Node& node, std::string_view kindId)
{
    HideValueEditors();
    const std::string& typeId = node.TypeId;
    /* Material *Parameter nodes also bind through the central host, but their
       editors stay put on multi-row bodies, so HasCentralEditors excludes them. */
    const bool isParameterNode = typeId == "FloatParameter" || typeId == "Vec2Parameter" ||
        typeId == "Vec3Parameter" || typeId == "Vec4Parameter" || typeId == "ColorParameter";
    /* ...but not when the node type draws that value as a synthetic detail row
       instead (expanded view). Showing both leaves the central host on screen
       with nothing in it, as an empty band between the last row and the
       preview plate. The rows own the value; the central host yields. */
    const bool valueOwnedByDetailRow =
        SyntheticDetailRowCount(node, m_EditHost.ExpandedView) > 0;
    const bool hasEditors =
        HasCentralEditors(node) || (isParameterNode && !valueOwnedByDetailRow);
    if (!hasEditors)
    {
        HideElement(m_Value);
        return;
    }

    EnsureValueEditorHost();
    ShowElement(m_ValueEditorHost);
    HideElement(m_Value);
    m_ValueEditorHost->RemoveClass("graph-single-float");

    const NodeTypeMeta* schemaMeta = GraphNodeRegistry::Get().FindNodeMeta(kindId, node.TypeId);
    auto bindFloat = [this, schemaMeta](FloatField* field, const char* key, float current)
    {
        if (!field)
            return;
        ApplyFieldRange(field, FindNodeValueRange(schemaMeta, key));
        if (!FieldHoldsFocus(field))
            field->SetValueWithoutNotify(current);
        field->SetOnValueChanging([this, field, key](const float& value)
        {
            if (UserIsDraggingField(field))
                EditLive(key, Graph::GraphValue(value));
        });
        field->SetOnValueChanged([this, key](const float& value)
        {
            CommitParam(key, Graph::GraphValue(value));
        });
    };

    auto bindText = [this](const char* key, const std::string& current)
    {
        if (!m_ValueText)
        {
            auto field = std::make_unique<TextField>();
            DecorateInlineField(*field);
            m_ValueText = field.get();
            m_ValueEditorHost->AddChild(std::move(field));
        }
        ShowElement(m_ValueText);
        if (!FieldHoldsFocus(m_ValueText))
            m_ValueText->SetValueWithoutNotify(current);
        m_ValueText->SetOnValueChanged([this, key](const std::string& value)
        {
            CommitParam(key, Graph::GraphValue(value));
        });
    };

    if (typeId == "FloatConstant")
    {
        FloatField* field = EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 0);
        bindFloat(field, "value", ReadFloatParam(node, "value", 0.0f));
        m_ValueEditorHost->AddClass("graph-single-float");
        return;
    }

    if (typeId == "Vec2Constant")
    {
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 0),
                  "x", ReadFloatParam(node, "x", 0.0f));
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 1),
                  "y", ReadFloatParam(node, "y", 0.0f));
        return;
    }

    if (typeId == "Vec4Constant")
    {
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 0),
                  "x", ReadFloatParam(node, "x", 0.0f));
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 1),
                  "y", ReadFloatParam(node, "y", 0.0f));
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 2),
                  "z", ReadFloatParam(node, "z", 0.0f));
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 3),
                  "w", ReadFloatParam(node, "w", 1.0f));
        return;
    }

    if (typeId == "Vec3Constant")
    {
        if (!m_ValueVec3)
        {
            auto field = std::make_unique<Vector3Field>();
            DecorateInlineField(*field);
            m_ValueVec3 = field.get();
            m_ValueEditorHost->AddChild(std::move(field));
        }
        ShowElement(m_ValueVec3);
        const Rendering::Vector3 current{
            ReadFloatParam(node, "x", 0.0f),
            ReadFloatParam(node, "y", 0.0f),
            ReadFloatParam(node, "z", 0.0f)
        };
        m_ValueVec3->SetOnValueChanged({});
        if (!FieldHoldsFocus(m_ValueVec3))
            m_ValueVec3->SetValue(current);
        m_ValueVec3->SetOnValueChanged([this](const Rendering::Vector3& value)
        {
            CommitParams([value](Graph::Node& n)
            {
                n.Parameters["x"] = value.x;
                n.Parameters["y"] = value.y;
                n.Parameters["z"] = value.z;
            });
        });
        return;
    }

    if (typeId == "Time")
    {
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 0),
                  "scale", ReadFloatParam(node, "scale", 1.0f));
        bindFloat(EnsureIndexedFloat(m_ValueFloats, *m_ValueEditorHost, 1),
                  "offset", ReadFloatParam(node, "offset", 0.0f));
        return;
    }

    if (typeId == "Compare")
    {
        if (!m_FunctionDropdown)
        {
            auto dropdown = std::make_unique<Dropdown>();
            DecorateInlineField(*dropdown);
            m_FunctionDropdown = dropdown.get();
            m_ValueEditorHost->AddChild(std::move(dropdown));
        }
        ShowElement(m_FunctionDropdown);
        const std::string current =
            node.Parameters.GetString("function", "SG_COMPARE_GREATER");
        /* Choices come from the registry's enum descriptors, so the dropdown
           and the schema cannot drift. */
        std::vector<Dropdown::Option> options;
        if (const NodeParamSpec* spec = FindSchemaParam(
                GraphNodeRegistry::Get().FindNodeMeta(kindId, typeId), "function"))
        {
            options.reserve(spec->Options.size());
            for (const Graph::NodeParamOption& option : spec->Options)
                options.push_back({option.Value, option.Label});
        }
        int selected = 0;
        bool found = false;
        for (int i = 0; i < static_cast<int>(options.size()); ++i)
        {
            if (options[static_cast<size_t>(i)].value == current)
            {
                selected = i;
                found = true;
                break;
            }
        }
        if (!found && !current.empty())
        {
            options.push_back({current, current});
            selected = static_cast<int>(options.size()) - 1;
        }
        m_FunctionDropdown->SetOnValueChanged({});
        if (!FieldHoldsFocus(m_FunctionDropdown))
            m_FunctionDropdown->SetOptions(options, selected);
        m_FunctionDropdown->SetOnValueChanged([this](const std::string& value)
        {
            CommitParam("function", Graph::GraphValue(value));
        });
        return;
    }

    if (typeId == "State" || typeId == "AnyState" || typeId == "Entry")
    {
        bindText("title", node.Parameters.GetString("title", ""));
        return;
    }

    if (typeId == "SetVariable" || typeId == "GetVariable" ||
        typeId == "FloatParameter" || typeId == "Vec2Parameter" ||
        typeId == "Vec3Parameter" || typeId == "Vec4Parameter" ||
        typeId == "ColorParameter")
    {
        bindText("variableName", node.Parameters.GetString("variableName", ""));
        return;
    }
}

void GraphPortedNode::BindPortEditors(const Graph::Node& node, std::string_view kindId)
{
    (void)kindId;
    if (!DrawsInlinePortEditors(node))
    {
        int hideIndex = 0;
        for (const Graph::Port& port : node.Ports)
        {
            if (port.Direction != Graph::PortDirection::In)
                continue;
            GraphPort* widget = InputPortAt(static_cast<size_t>(hideIndex++));
            if (widget)
                ClearPortInlineEditor(*widget);
        }
        return;
    }

    const bool compactSingleInline = IsCompactSingleInlineNode(node);
    const NodeTypeMeta* schemaMeta = GraphNodeRegistry::Get().FindNodeMeta(kindId, node.TypeId);
    const float nodeWidth = GraphNodeMetrics::GetNodeWidth(node);

    /* One value column per node: editors start at the widest measured label
       (inspector-style, so stacked rows keep equal chip widths) and run to
       the row's right reserve. Rows clamp inside the uniform node width, so
       spans only fall under the comfortable minimum when an Out label
       genuinely crowds a row. */
    const float editorStart = EditorStartGraph(node, true);
    auto rowSpanUnits = [&](int inputIdx, float& outStart) -> float
    {
        outStart = editorStart;
        const float end = nodeWidth - GraphNodeMetrics::InlineEditorRightReserveGraph(
            GraphNodeMetrics::SharedOutNameCharsForInput(node, inputIdx));
        float width = end - outStart;
        if (width < GraphNodeMetrics::kInlineEditorMinWidthGraph)
            width = GraphNodeMetrics::kInlineEditorMinWidthGraph;
        return width;
    };

    /* Stacked float boxes share one width — the most-constrained row sets
       it — so single-number columns stay even. */
    float floatWidthUnits = GraphNodeMetrics::kFloatEditorMaxWidthGraph;
    {
        int floatScan = 0;
        for (const Graph::Port& port : node.Ports)
        {
            if (port.Direction != Graph::PortDirection::In)
                continue;
            const int thisInput = floatScan++;
            if (ClassifyPortType(port.DataType) != InlinePortKind::Float)
                continue;
            float rowStart = 0.f;
            const float rowUnits = rowSpanUnits(thisInput, rowStart);
            floatWidthUnits = std::min(floatWidthUnits, rowUnits);
        }
    }

    int inputIndex = 0;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction != Graph::PortDirection::In)
            continue;
        GraphPort* widget = InputPortAt(static_cast<size_t>(inputIndex++));
        if (!widget)
            continue;

        const InlinePortKind portKind = ClassifyPortType(port.DataType);
        const bool connected = m_EditHost.IsInputConnected &&
            m_EditHost.IsInputConnected(node.Id, port.Id);
        if (portKind == InlinePortKind::None || connected)
        {
            ClearPortInlineEditor(*widget);
            continue;
        }

        UIElement* host = EnsurePortEditorHost(*widget);
        HideDirectChildren(*host);
        ShowElement(host);
        widget->AddClass("has-inline-editor");
        if (compactSingleInline)
            widget->AddClass("graph-single-inline");
        else
            widget->RemoveClass("graph-single-inline");
        if (portKind == InlinePortKind::Bool)
            widget->AddClass("bool-editor");
        else
            widget->RemoveClass("bool-editor");
        if (portKind == InlinePortKind::Vec2 || portKind == InlinePortKind::Vec3 ||
            portKind == InlinePortKind::Vec4)
            widget->AddClass("vec-editor");
        else
            widget->RemoveClass("vec-editor");
        if (portKind == InlinePortKind::Bool)
        {
            /* Checkbox hosts keep their own compact CSS geometry. */
            host->Overrides().Reset(Style::Width);
            host->Overrides().Reset(Style::MaxWidth);
        }
        else
        {
            /* Measured row: editor starts after the shared value column and
               ends at the row's right reserve. Percents are of the 10-unit
               port box. */
            float startUnits = 0.f;
            float widthUnits = rowSpanUnits(inputIndex - 1, startUnits);
            if (portKind == InlinePortKind::Float)
                widthUnits = floatWidthUnits;
            const float leftPct = (startUnits + GraphNodeMetrics::kPortHangGraph) /
                GraphNodeMetrics::kPortSizeGraph * 100.f;
            const float widthPct = widthUnits / GraphNodeMetrics::kPortSizeGraph * 100.f;
            host->Overrides()
                .Set(Style::PositionLeft, StyleLength::Percent(leftPct))
                .Set(Style::Width, StyleLength::Percent(widthPct))
                .Set(Style::MaxWidth, StyleLength::Percent(widthPct));
            host->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
            /* The measured start guarantees the label room; lift the CSS
               60-unit ellipsis cap to match. */
            for (const auto& child : widget->GetChildren())
            {
                if (child && child->HasClass("graph-port-label"))
                {
                    child->Overrides().Set(Style::MaxWidth, StyleLength::Percent(
                        (startUnits - GraphNodeMetrics::kInlineLabelStartGraph) /
                        GraphNodeMetrics::kPortSizeGraph * 100.f));
                    child->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
                    break;
                }
            }
        }
        const std::string portId = port.Id;

        if (portKind == InlinePortKind::Float)
        {
            FloatField* field = EnsureFloatChild(*host);
            if (!field)
                continue;
            ApplyFieldRange(field, FindNodeValueRange(schemaMeta, portId));
            if (!FieldHoldsFocus(field))
                field->SetValueWithoutNotify(ReadFloatParam(node, portId.c_str(), 0.0f));
            field->SetOnValueChanging([this, field, portId](const float& value)
            {
                if (UserIsDraggingField(field))
                    EditLive(portId, Graph::GraphValue(value));
            });
            field->SetOnValueChanged([this, portId](const float& value)
            {
                CommitParam(portId, Graph::GraphValue(value));
            });
            continue;
        }

        if (portKind == InlinePortKind::Vec3)
        {
            Vector3Field* field = EnsureVec3Child(*host);
            if (!field)
                continue;
            ApplyFieldRange(field, FindNodeValueRange(schemaMeta, portId));
            float xyz[3] = {0.0f, 0.0f, 0.0f};
            auto it = node.Parameters.find(portId);
            if (it != node.Parameters.end())
                ReadVecParam(it->second, xyz, 3, 0.0f);
            field->SetOnValueChanged({});
            if (!FieldHoldsFocus(field))
                field->SetValue(Rendering::Vector3{xyz[0], xyz[1], xyz[2]});
            field->SetOnValueChanged([this, portId](const Rendering::Vector3& value)
            {
                const float v[3] = {value.x, value.y, value.z};
                CommitParam(portId, Graph::GraphValue(FormatGlslVec(3, v)));
            });
            continue;
        }

        if (portKind == InlinePortKind::Vec2 || portKind == InlinePortKind::Vec4)
        {
            const int count = portKind == InlinePortKind::Vec2 ? 2 : 4;
            float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            auto it = node.Parameters.find(portId);
            if (it != node.Parameters.end())
                ReadVecParam(it->second, values, count, 0.0f);
            for (int i = 0; i < count; ++i)
            {
                FloatField* field = EnsureNthFloatChild(*host, static_cast<size_t>(i));
                if (!field)
                    continue;
                ApplyFieldRange(field, FindNodeValueRange(schemaMeta, portId));
                if (!FieldHoldsFocus(field))
                    field->SetValueWithoutNotify(values[i]);
                const auto writeComponent = [portId, i, count](float component)
                {
                    return [portId, i, count, component](Graph::Node& n)
                    {
                        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        auto found = n.Parameters.find(portId);
                        if (found != n.Parameters.end())
                            ReadVecParam(found->second, v, count, 0.0f);
                        v[i] = component;
                        n.Parameters[portId] = FormatGlslVec(count, v);
                    };
                };
                field->SetOnValueChanging([this, field, writeComponent](const float& component)
                {
                    if (UserIsDraggingField(field))
                        EditLive(writeComponent(component));
                });
                field->SetOnValueChanged([this, writeComponent](const float& component)
                {
                    CommitParams(writeComponent(component));
                });
            }
            continue;
        }

        if (portKind == InlinePortKind::Bool)
        {
            Checkbox* box = EnsureCheckboxChild(*host);
            if (!box)
                continue;
            if (!FieldHoldsFocus(box))
                box->SetValueWithoutNotify(node.Parameters.GetBool(portId, false));
            box->SetOnValueChanged([this, portId](const bool& value)
            {
                CommitParam(portId, Graph::GraphValue(value));
            });
            continue;
        }

        if (portKind == InlinePortKind::String)
        {
            TextField* field = EnsureTextChild(*host);
            if (!field)
                continue;
            if (!FieldHoldsFocus(field))
                field->SetValueWithoutNotify(node.Parameters.GetString(portId, ""));
            field->SetOnValueChanged([this, portId](const std::string& value)
            {
                CommitParam(portId, Graph::GraphValue(value));
            });
        }
    }
}

void GraphPortedNode::BindModel(const Graph::Node& node, std::string_view kindId,
                                const GraphNodeVisualState& visual, const GraphNodeEditHost* editHost)
{
    SetModelNodeId(node.Id);
    m_EditHost = editHost ? *editHost : GraphNodeEditHost{};
    ++m_BindSuppressDepth;
    struct BindSuppressGuard
    {
        GraphPortedNode& Node;
        explicit BindSuppressGuard(GraphPortedNode& node) : Node(node) {}
        ~BindSuppressGuard() { --Node.m_BindSuppressDepth; }
    } bindGuard(*this);
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().FindNodeMeta(kindId, node.TypeId);
    std::string title = meta ? meta->DisplayName : node.TypeId;
    const std::string named = node.Parameters.GetString("title", "");
    if (!named.empty())
        title = named;
    SetTitleText(title);
    if (m_Icon)
        GraphNodeIcons::ApplyIconClass(*m_Icon, meta ? meta->IconStem : std::string{});

    if (m_Header)
    {
        const float nodeHeight = RectHeightGraph(node);
        m_Header->Overrides().Set(Style::Height,
            StyleLength::Percent(GraphNodeMetrics::HeaderHeightPercent(nodeHeight)));
        m_Header->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    }

    /* The node body and every box inside it read these variables
       (node-graph.css); published per node so they track the body-color
       preference. */
    Overrides().SetCustomColor(HashStringId("--graph-node-bg"),
                               NodeColorSettings::GetNodeBodyColorArgb());
    Overrides().SetCustomColor(HashStringId("--graph-node-header-bg"),
                               NodeColorSettings::GetNodeColorArgb(kindId, node.TypeId));
    Overrides().SetCustomColor(HashStringId("--graph-value-bg"),
                               NodeColorSettings::GetNodeValueSurfaceColorArgb());
    /* Same deal for the plates that frame a rendered image: the preview plate
       and the texture drop slot. */
    Overrides().SetCustomColor(HashStringId("--graph-plate-bg"),
                               NodeColorSettings::GetNodePlateColorArgb());

    BindModelPorts(node);
    BindValueEditors(node, kindId);
    BindPortEditors(node, kindId);

    if (m_Preview)
        BindDetailSlot(node, *m_Preview);

    BindDetailRows(node, kindId);

    ApplyVisualState(visual);
}

void GraphPortedNode::SetTitleText(const std::string& text)
{
    if (m_Title)
        m_Title->SetText(text);
}

void GraphPortedNode::SetValueText(const std::string& text)
{
    if (m_Value)
        m_Value->SetText(text);
}

void GraphPortedNode::Reset()
{
    GraphNode::Reset();
    m_EditHost = {};
    m_BindSuppressDepth = 0;
    SetTitleText({});
    SetValueText({});
    if (m_Icon)
        GraphNodeIcons::ApplyIconClass(*m_Icon, {});
    HideValueEditors(true);
    HideElement(m_Value, true);
    if (m_Header)
        m_Header->Overrides().Reset(Style::BackgroundColor);
    if (m_Preview)
        m_Preview->Reset();
    HideDetailRows(0);
    for (GraphPort* port : m_InputPorts)
    {
        if (port)
            port->Reset();
    }
    for (GraphPort* port : m_OutputPorts)
    {
        if (port)
            port->Reset();
    }
    ClearElementIds(*this);
}

GraphPort* GraphPortedNode::InputPortAt(size_t index) const
{
    return index < m_InputPorts.size() ? m_InputPorts[index] : nullptr;
}

GraphPort* GraphPortedNode::OutputPortAt(size_t index) const
{
    return index < m_OutputPorts.size() ? m_OutputPorts[index] : nullptr;
}

GraphPort* GraphPortedNode::FindPort(const std::string& portId) const
{
    if (portId.empty())
        return nullptr;
    for (GraphPort* port : m_InputPorts)
    {
        if (port && port->GetPortId() == portId)
            return port;
    }
    for (GraphPort* port : m_OutputPorts)
    {
        if (port && port->GetPortId() == portId)
            return port;
    }
    return nullptr;
}

} // namespace GameEngine

namespace RegisterGraphElements
{
static auto s_reg_graphPortedNode =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::GraphPortedNode>(
        "GraphPortedNode",
        []() { return std::make_unique<GameEngine::GraphPortedNode>(); })
        .TagAlias("graphportednode");
}
