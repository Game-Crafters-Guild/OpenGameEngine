#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>

#include "Editor/EditorPaths.h"

#include "Graph/GraphModel.h"
#include "Graph/GraphNodeMetrics.h"
#include "Graph/GraphPortLabels.h"
#include "ShaderGraph/MaterialGraphNode.h"
#include "Graph/GraphNode.h"
#include "Graph/GraphNodePool.h"
#include "Graph/GraphOverlay.h"
#include "Graph/GraphPort.h"
#include "Graph/GraphPortedNode.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"
#include "UI/UIElement.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace {

void ExpectNoSelectorAttributes(const UIElement& el)
{
    EXPECT_FALSE(el.HasSelectorAttributes());
    EXPECT_TRUE(el.GetId().empty());
    for (const auto& child : el.GetChildren())
    {
        if (child)
            ExpectNoSelectorAttributes(*child);
    }
}

bool RowIsHidden(const UIElement& el)
{
    const std::optional<DisplayMode> display = el.Overrides().Get(Style::Display);
    return display.has_value() && display.value() == DisplayMode::None;
}

std::vector<UIElement*> VisibleDetailRows(UIElement& node)
{
    std::vector<UIElement*> rows;
    for (const auto& child : node.GetChildren())
    {
        if (child && child->HasClass("graph-detail-row") && !RowIsHidden(*child))
            rows.push_back(child.get());
    }
    return rows;
}

UIElement* FindByClass(UIElement& root, const char* className)
{
    if (root.HasClass(className))
        return &root;
    for (const auto& child : root.GetChildren())
    {
        if (!child)
            continue;
        if (UIElement* found = FindByClass(*child, className))
            return found;
    }
    return nullptr;
}

// Const counterpart, and strictly a DESCENDANT search: it never matches the
// root it is given, which is what its callers rely on.
const UIElement* FindDescendantWithClass(const UIElement& root, const char* className)
{
    for (const auto& child : root.GetChildren())
    {
        if (!child)
            continue;
        if (child->HasClass(className))
            return child.get();
        if (const UIElement* found = FindDescendantWithClass(*child, className))
            return found;
    }
    return nullptr;
}

std::string RowLabelText(UIElement& row)
{
    if (auto* label = dynamic_cast<Label*>(FindByClass(row, "graph-detail-label")))
        return label->GetText();
    return {};
}

/* A material parameter node with no ports and no parameters: every detail row it
   shows is the synthetic Value row, so the count is unambiguous. */
Graph::Node MakePinlessMaterialParameterNode()
{
    Graph::Node node;
    node.Id = "n-param";
    node.TypeId = "FloatParameter";
    return node;
}

} // namespace

// The template body and the C++ fallback are deliberately equivalent trees, so
// the node itself cannot tell you which one it used. What it can prove is that
// every slot BindSlots looks for exists once the staged template is the source:
// the build stages graph-ported-node.uxml at exactly the layout
// GraphPortedNode resolves (the install-assets root, which is <exe>/Assets for
// a test executable on every platform), so the file being there is what makes
// this the template's tree. StagedAssetPathGuardTests locks the resolution itself.
TEST(GraphNodePoolTests, PortedNodeBindsSlotsFromStagedTemplate)
{
    UIRegistration::RegisterBuiltInControls();

    const std::filesystem::path templatePath =
        Editor::GetEditorGlobalPaths().installAssetsRoot / "UI" / "Graph" / "graph-ported-node.uxml";
    ASSERT_TRUE(std::filesystem::exists(templatePath)) << templatePath.string();

    GraphPortedNode node;
    EXPECT_NE(FindDescendantWithClass(node, "graph-port-drop-slot"), nullptr);

    EXPECT_NE(FindDescendantWithClass(node, "graph-node-body"), nullptr);
    EXPECT_NE(FindDescendantWithClass(node, "graph-node-header"), nullptr);
    EXPECT_NE(FindDescendantWithClass(node, "graph-node-content"), nullptr);
    EXPECT_NE(FindDescendantWithClass(node, "graph-node-preview"), nullptr);

    // Title and value must bind to the template's labels, not dangle.
    node.SetTitleText("TemplateTitle");
    const auto* title = dynamic_cast<const Label*>(FindDescendantWithClass(node, "graph-node-title"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->GetText(), "TemplateTitle");

    node.SetValueText("TemplateValue");
    const auto* value = dynamic_cast<const Label*>(FindDescendantWithClass(node, "graph-node-value"));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->GetText(), "TemplateValue");
}

TEST(GraphNodePoolTests, GraphNodeIdentityIsModelIdNotElementId)
{
    GraphNode node;
    EXPECT_FALSE(node.IsFocusable());
    EXPECT_TRUE(node.HasClass("graph-node"));
    EXPECT_TRUE(node.GetId().empty());
    EXPECT_FALSE(node.HasSelectorAttributes());

    const std::optional<bool> pointerEvents = node.Overrides().Get(Style::PointerEvents);
    EXPECT_FALSE(pointerEvents.has_value());

    node.SetModelNodeId("n1");
    node.ApplyVisualState({true, true, true, true, true});
    EXPECT_TRUE(node.HasClass("selected"));
    EXPECT_TRUE(node.HasClass("search-match"));
    EXPECT_TRUE(node.HasClass("runtime-active"));
    EXPECT_TRUE(node.HasClass("drop-target"));
    EXPECT_TRUE(node.HasClass("has-error"));

    node.Reset();
    EXPECT_TRUE(node.GetModelNodeId().empty());
    EXPECT_FALSE(node.HasClass("selected"));
    EXPECT_FALSE(node.HasClass("search-match"));
    EXPECT_FALSE(node.HasClass("has-error"));
    EXPECT_FALSE(node.HasClass("runtime-active"));
    EXPECT_FALSE(node.HasClass("drop-target"));
}

TEST(GraphNodePoolTests, SetGraphRectEnrollsAbsoluteOverrides)
{
    GraphNode node;
    const Mathematics::Rect rect{10.0f, 20.0f, 180.0f, 60.0f};
    EXPECT_TRUE(node.SetGraphRect(rect));

    Mathematics::Rect got{};
    ASSERT_TRUE(UI::Layout::TryGetAbsolutePosition(node, got));
    EXPECT_EQ(got, rect);
    EXPECT_FALSE(node.SetGraphRect(rect));
}

TEST(GraphNodePoolTests, PortedNodeBindsByClassAndGrowsPortsOnly)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    ExpectNoSelectorAttributes(node);
    EXPECT_TRUE(node.HasClass("graph-node"));
    EXPECT_TRUE(node.HasClass("graph-ported-node"));

    node.EnsurePortCount(2, 1);
    EXPECT_EQ(node.InputPortCount(), 2u);
    EXPECT_EQ(node.OutputPortCount(), 1u);
    ASSERT_NE(node.InputPortAt(0), nullptr);
    EXPECT_EQ(node.InputPortAt(0)->GetPortId(), "in0");
    EXPECT_TRUE(node.InputPortAt(0)->HasClass("graph-port-input"));
    EXPECT_FALSE(node.InputPortAt(0)->HasClass("graph-port-row-0"));

    Mathematics::Rect portRect{};
    EXPECT_FALSE(UI::Layout::TryGetAbsolutePosition(*node.InputPortAt(0), portRect));
    EXPECT_FALSE(UI::Layout::TryGetAbsolutePosition(*node.OutputPortAt(0), portRect));

    GraphPort* firstIn = node.InputPortAt(0);
    node.EnsurePortCount(1, 1);
    EXPECT_EQ(node.InputPortCount(), 2u);
    EXPECT_EQ(node.InputPortAt(0), firstIn);
}

TEST(GraphNodePoolTests, SurplusSlotsUseDisplayNone)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    UIElement canvas;
    GraphNodePool pool(&layer, &canvas);

    pool.Sync(3);
    ASSERT_EQ(pool.SlotCount(), 3u);
    ASSERT_EQ(layer.GetChildren().size(), 3u);

    pool.Sync(1);
    ASSERT_EQ(pool.SlotCount(), 3u);
    const std::optional<DisplayMode> visibleDisplay = layer.GetChildren()[0]->Overrides().Get(Style::Display);
    EXPECT_TRUE(!visibleDisplay.has_value() || *visibleDisplay != DisplayMode::None);
    ASSERT_TRUE(layer.GetChildren()[1]->Overrides().Get(Style::Display).has_value());
    EXPECT_EQ(*layer.GetChildren()[1]->Overrides().Get(Style::Display), DisplayMode::None);
    ASSERT_TRUE(layer.GetChildren()[2]->Overrides().Get(Style::Display).has_value());
    EXPECT_EQ(*layer.GetChildren()[2]->Overrides().Get(Style::Display), DisplayMode::None);

    GraphPortedNode* recycled = pool.SlotAt(1);
    ASSERT_NE(recycled, nullptr);
    pool.Recycle(recycled);
    ASSERT_TRUE(recycled->Overrides().Get(Style::Display).has_value());
    EXPECT_EQ(*recycled->Overrides().Get(Style::Display), DisplayMode::None);
}

TEST(GraphNodePoolTests, FindByModelIdAndBindModelSetsRealPortIds)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);

    Graph::Node node;
    node.Id = "n1";
    node.TypeId = "FloatConstant";
    node.PositionX = 10.0f;
    node.PositionY = 20.0f;
    node.Ports.push_back({"lhs", Graph::PortDirection::In, "float", "Left"});
    node.Ports.push_back({"rhs", Graph::PortDirection::In, "float", "Right"});
    node.Ports.push_back({"value", Graph::PortDirection::Out, "float", "Value"});
    node.Parameters["value"] = "3.5";

    slot->EnsurePortCount(4, 4);
    slot->BindModel(node, Graph::kKindIdGameLogic, GraphNodeVisualState{true, false, false, false, false});

    EXPECT_EQ(pool.FindByModelId("n1"), slot);
    EXPECT_EQ(slot->GetModelNodeId(), "n1");
    EXPECT_TRUE(slot->HasClass("selected"));
    ASSERT_NE(slot->FindPort("lhs"), nullptr);
    EXPECT_EQ(slot->FindPort("lhs"), slot->InputPortAt(0));
    EXPECT_EQ(slot->FindPort("lhs")->GetPortId(), "lhs");
    ASSERT_NE(slot->FindPort("value"), nullptr);
    EXPECT_EQ(slot->FindPort("value")->GetPortId(), "value");
    EXPECT_EQ(slot->FindPort("missing"), nullptr);

    ASSERT_TRUE(slot->InputPortAt(2) != nullptr);
    const std::optional<DisplayMode> extraDisplay = slot->InputPortAt(2)->Overrides().Get(Style::Display);
    ASSERT_TRUE(extraDisplay.has_value());
    EXPECT_EQ(*extraDisplay, DisplayMode::None);
    const std::optional<DisplayMode> usedDisplay = slot->InputPortAt(0)->Overrides().Get(Style::Display);
    EXPECT_TRUE(!usedDisplay.has_value() || *usedDisplay != DisplayMode::None);
    EXPECT_TRUE(slot->GetId().empty());
}

TEST(GraphNodePoolTests, RecycleClearsFindByModelIdAndHidesDisplayNone)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);
    slot->SetModelNodeId("keep");
    EXPECT_EQ(pool.FindByModelId("keep"), slot);

    pool.Recycle(slot);
    EXPECT_EQ(pool.FindByModelId("keep"), nullptr);
    ASSERT_TRUE(slot->Overrides().Get(Style::Display).has_value());
    EXPECT_EQ(*slot->Overrides().Get(Style::Display), DisplayMode::None);
}

TEST(GraphNodePoolTests, DefaultFactoryAcquiresBaseNodes)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(dynamic_cast<MaterialGraphNode*>(slot), nullptr);
    EXPECT_TRUE(slot->HasClass("graph-ported-node"));
    EXPECT_FALSE(slot->HasClass("material-graph-node"));
}

TEST(GraphNodePoolTests, MaterialFactoryAcquiresMaterialNodesAndReplacesOldSlots)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    GraphPortedNode* base = pool.Acquire();
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(pool.SlotCount(), 1u);

    pool.SetFactory([]() { return std::make_unique<MaterialGraphNode>(); });
    EXPECT_EQ(pool.SlotCount(), 0u); /* slots built by the old factory are destroyed */

    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);
    MaterialGraphNode* material = dynamic_cast<MaterialGraphNode*>(slot);
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(material->HasClass("graph-ported-node"));
    EXPECT_TRUE(material->HasClass("material-graph-node"));
    ExpectNoSelectorAttributes(*material);

    pool.SetFactory({});
    GraphPortedNode* plain = pool.Acquire();
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(dynamic_cast<MaterialGraphNode*>(plain), nullptr);
}

/* The material node's synthetic Value row: reserved by SyntheticDetailRowCount and
   emitted by BindSyntheticDetailRow. The generic GraphExpandedViewTests never build
   a parameter node, so without these the whole override is uncovered. */
TEST(GraphNodePoolTests, MaterialParameterNodeReservesOneValueRowInExpandedViewOnly)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    pool.SetFactory([]() { return std::make_unique<MaterialGraphNode>(); });

    const Graph::Node model = MakePinlessMaterialParameterNode();
    EXPECT_EQ(pool.SyntheticDetailRowCount(model, /*expandedView=*/true), 1);
    EXPECT_EQ(pool.SyntheticDetailRowCount(model, /*expandedView=*/false), 0);

    // A node type that binds no variable earns no synthetic row in either view.
    Graph::Node plain;
    plain.Id = "n-add";
    plain.TypeId = "Add";
    EXPECT_EQ(pool.SyntheticDetailRowCount(plain, /*expandedView=*/true), 0);
}

TEST(GraphNodePoolTests, ExpandedMaterialParameterNodeEmitsExactlyOneValueRow)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    pool.SetFactory([]() { return std::make_unique<MaterialGraphNode>(); });
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);

    const Graph::Node model = MakePinlessMaterialParameterNode();
    // No parameters, so DetailRowCount contributes nothing: any row present is
    // the synthetic one.
    ASSERT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/true), 0);

    GraphNodeEditHost host;
    host.ExpandedView = true;
    slot->BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    std::vector<UIElement*> rows = VisibleDetailRows(*slot);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(RowLabelText(*rows[0]), "Value");
    EXPECT_NE(FindByClass(*rows[0], "float-field"), nullptr);

    // Collapsing takes the row away again.
    GraphNodeEditHost collapsed;
    collapsed.ExpandedView = false;
    slot->BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &collapsed);
    EXPECT_TRUE(VisibleDetailRows(*slot).empty());
}

TEST(GraphNodePoolTests, ParameterNodeRowsStartAtAValueColumnItsLabelsFit)
{
    UIRegistration::RegisterBuiltInControls();

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    pool.SetFactory([]() { return std::make_unique<MaterialGraphNode>(); });
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);

    /* A parameter node has no editable In ports, so every one of its rows is a
       param or the synthetic Value row. Measuring the value column from ports
       alone leaves it at zero here: boxes flush against the node edge, labels
       clamped to zero width. */
    Graph::Node model = MakePinlessMaterialParameterNode();
    model.Parameters["Slot"] = Graph::GraphValue("1");
    model.Parameters["VariableName"] = Graph::GraphValue("uMetallic");

    GraphNodeEditHost host;
    host.ExpandedView = true;
    slot->BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    std::vector<UIElement*> rows = VisibleDetailRows(*slot);
    ASSERT_EQ(rows.size(), 3u) << "two params plus the synthetic Value row";

    const float nodeWidth = GraphNodeMetrics::GetNodeWidth(model);
    // The widest label decides the column: "Variable Name", not "Slot"/"Value" —
    // as it is drawn, so through the node abbreviation.
    const std::string widest =
        GraphPortLabels::AbbreviateForNode(GraphPortLabels::PrettifyParamKey("VariableName"));
    const float expectedStart =
        GraphNodeMetrics::InlineEditorStartGraphFor(static_cast<int>(widest.size()));
    EXPECT_GT(expectedStart, 0.f) << "a value column of zero flushes the boxes left";
    const float expectedStartPct = expectedStart / nodeWidth * 100.f;

    for (UIElement* row : rows)
    {
        auto* label = dynamic_cast<Label*>(FindByClass(*row, "graph-detail-label"));
        ASSERT_NE(label, nullptr);
        const std::optional<StyleLength> maxWidth = label->Overrides().Get(Style::MaxWidth);
        ASSERT_TRUE(maxWidth.has_value() && maxWidth->IsPercent());
        EXPECT_GT(maxWidth->Value, 0.f) << "label clamped to nothing: " << label->GetText();

        UIElement* editorHost = row->GetChildren()[1].get();
        ASSERT_NE(editorHost, nullptr);
        const std::optional<StyleLength> left = editorHost->Overrides().Get(Style::PositionLeft);
        ASSERT_TRUE(left.has_value() && left->IsPercent());
        // One column: every row's box starts on the same edge, clear of the labels.
        EXPECT_FLOAT_EQ(left->Value, expectedStartPct);
    }
}

TEST(GraphNodePoolTests, MaterialParameterValueRowShowsAndCommitsTheVariablesValue)
{
    UIRegistration::RegisterBuiltInControls();

    Graph::Model graph;
    Graph::Variable variable;
    variable.Name = "gloss";
    variable.Type = "float";
    variable.Value = "0.25";
    graph.Variables.push_back(variable);

    Graph::Node model = MakePinlessMaterialParameterNode();
    model.Parameters["variableName"] = Graph::GraphValue(std::string("gloss"));
    graph.Nodes.push_back(model);

    UIElement layer;
    GraphNodePool pool(&layer, nullptr);
    pool.SetFactory([]() { return std::make_unique<MaterialGraphNode>(); });
    GraphPortedNode* slot = pool.Acquire();
    ASSERT_NE(slot, nullptr);

    int undoCount = 0;
    GraphNodeEditHost host;
    host.Model = &graph;
    host.ExpandedView = true;
    host.Undo = [&](const std::string&, std::function<void()> mutate)
    {
        ++undoCount;
        mutate();
    };

    const Graph::Node* modelNode = graph.FindNode("n-param");
    ASSERT_NE(modelNode, nullptr);
    slot->BindModel(*modelNode, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    // One row per port-less parameter, plus the synthetic Value row.
    const std::vector<UIElement*> rows = VisibleDetailRows(*slot);
    EXPECT_EQ(static_cast<int>(rows.size()),
              GraphNodeMetrics::DetailRowCount(*modelNode, false, false, true) + 1);
    ASSERT_FALSE(rows.empty());
    UIElement* valueRow = rows.back();
    EXPECT_EQ(RowLabelText(*valueRow), "Value");

    auto* field = dynamic_cast<FloatField*>(FindByClass(*valueRow, "float-field"));
    ASSERT_NE(field, nullptr);
    EXPECT_FLOAT_EQ(field->GetValue(), 0.25f);

    // Editing writes the VARIABLE, not a node parameter.
    field->SetValue(0.75f);
    field->NotifyValueChanged();
    EXPECT_EQ(undoCount, 1);
    ASSERT_EQ(graph.Variables.size(), 1u);
    EXPECT_EQ(graph.Variables[0].Value, "0.75");
    EXPECT_EQ(graph.FindNode("n-param")->Parameters.GetString("value", ""), "");
}

/* Reserve and emit are two halves of one contract: ReservedBlockHeight takes
   the square block out of the node rect from the model alone, so BindPreviewPlate
   has to fill it in every case the reserve covers — a missing atlas cell
   included, or the node carries a node-width-tall hole. */
TEST(GraphNodePoolTests, MaterialPreviewPlateFillsTheBlockWhetherOrNotTheAtlasHasACell)
{
    UIRegistration::RegisterBuiltInControls();

    Graph::Node model;
    model.Id = "n-mul";
    model.TypeId = "Multiply";
    model.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    model.Ports.push_back({"b", Graph::PortDirection::In, "float", "B"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});

    MaterialGraphNode node;
    ASSERT_GT(node.ReservedBlockHeight(model, /*expandedView=*/true), 0.f);
    ASSERT_FLOAT_EQ(node.ReservedBlockHeight(model, /*expandedView=*/false), 0.f);

    GraphNodeEditHost host;
    host.ExpandedView = true;

    // No provider installed: the atlas has nothing for this node yet.
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);
    UIElement* plate = FindByClass(node, "graph-node-preview-plate");
    ASSERT_NE(plate, nullptr);
    EXPECT_FALSE(RowIsHidden(*plate));

    // A provider that answers with an empty binding is the same case.
    node.SetPreviewLookup([](const std::string&)
                          { return Editor::GraphNodePreviewBinding{}; });
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);
    EXPECT_FALSE(RowIsHidden(*plate));

    // Once the cell exists the same plate takes the atlas sprite.
    node.SetPreviewLookup([](const std::string&)
    {
        Editor::GraphNodePreviewBinding binding;
        binding.Resource = "graph-preview-atlas";
        return binding;
    });
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);
    EXPECT_FALSE(RowIsHidden(*plate));

    // Collapsed reserves nothing, so the plate goes away with the block.
    GraphNodeEditHost collapsed;
    collapsed.ExpandedView = false;
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &collapsed);
    EXPECT_TRUE(RowIsHidden(*plate));
}

TEST(GraphNodePoolTests, OverlayHasOffThreadClassAndNoFocus)
{
    GraphOverlay overlay;
    EXPECT_TRUE(overlay.HasClass("graph-overlay"));
    EXPECT_FALSE(overlay.IsFocusable());
    const std::optional<bool> pointerEvents = overlay.Overrides().Get(Style::PointerEvents);
    ASSERT_TRUE(pointerEvents.has_value());
    EXPECT_FALSE(*pointerEvents);
}

TEST(GraphNodePoolTests, PointerEventsStayEnabledOnNodeAndPort)
{
    GraphNode node;
    GraphPort port;
    EXPECT_FALSE(node.Overrides().Has(Style::PointerEvents.id));
    EXPECT_FALSE(port.Overrides().Has(Style::PointerEvents.id));
}
