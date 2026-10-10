#include <gtest/gtest.h>

#include "Graph/GraphCanvasEditKeys.h"
#include "Graph/GraphNodeMetrics.h"
#include "Graph/GraphNodeOverlap.h"
#include "Graph/GraphPortLabels.h"
#include "ShaderGraph/MaterialGraphNode.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNode.h"
#include "Graph/GraphNodePool.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphPort.h"
#include "Graph/GraphPortedNode.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "Input/KeyCodes.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace GameEngine;

namespace {

bool IsDisplayNone(const UIElement& el)
{
    const std::optional<DisplayMode> display = el.Overrides().Get(Style::Display);
    return display.has_value() && *display == DisplayMode::None;
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

std::vector<FloatField*> CollectVisibleFloatFields(UIElement& host)
{
    std::vector<FloatField*> fields;
    for (const auto& child : host.GetChildren())
    {
        if (auto* field = dynamic_cast<FloatField*>(child.get()))
        {
            if (!IsDisplayNone(*field))
                fields.push_back(field);
        }
    }
    return fields;
}

} // namespace

TEST(GraphNodeLayoutTests, PooledNodeNeverTakesElementIdOrCustomState)
{
    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    GraphPortedNode* node = pool.Acquire();
    ASSERT_NE(node, nullptr);

    node->SetModelNodeId("spike-0");
    node->EnsurePortCount(2, 2);
    node->ApplyVisualState({true, false, false, false, false});

    EXPECT_TRUE(node->GetId().empty());
    EXPECT_TRUE(node->GetCustomStateIds().empty());
    EXPECT_EQ(node->GetModelNodeId(), "spike-0");
    EXPECT_TRUE(node->HasClass("selected"));
    EXPECT_FALSE(node->HasClass("search-match"));

    ASSERT_GE(node->InputPortCount(), 2u);
    ASSERT_NE(node->InputPortAt(0), nullptr);
    EXPECT_TRUE(node->InputPortAt(0)->GetId().empty());
    EXPECT_TRUE(node->InputPortAt(0)->GetCustomStateIds().empty());
}

TEST(GraphNodeLayoutTests, PoolGrowsAndNeverShrinks)
{
    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    pool.Sync(60);
    EXPECT_EQ(pool.SlotCount(), 60u);
    EXPECT_EQ(layer->GetChildren().size(), 60u);

    pool.Sync(10);
    EXPECT_EQ(pool.SlotCount(), 60u);
    EXPECT_EQ(layer->GetChildren().size(), 60u);

    for (size_t i = 0; i < pool.SlotCount(); ++i)
    {
        GraphPortedNode* node = pool.SlotAt(i);
        ASSERT_NE(node, nullptr);
        if (i < 10)
            EXPECT_FALSE(IsDisplayNone(*node)) << i;
        else
            EXPECT_TRUE(IsDisplayNone(*node)) << i;
    }
}

TEST(GraphNodeLayoutTests, RecycleHidesWithDisplayNoneAndClearsIdentity)
{
    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    GraphPortedNode* node = pool.Acquire();
    ASSERT_NE(node, nullptr);
    node->SetModelNodeId("keep");
    node->ApplyVisualState({true, true, true, true, false});

    pool.Recycle(node);
    EXPECT_TRUE(IsDisplayNone(*node));
    EXPECT_TRUE(node->GetModelNodeId().empty());
    EXPECT_FALSE(node->HasClass("selected"));
    EXPECT_FALSE(node->HasClass("search-match"));
    EXPECT_FALSE(node->HasClass("runtime-active"));
    EXPECT_FALSE(node->HasClass("drop-target"));
}

TEST(GraphNodeLayoutTests, SetGraphRectWritesAbsolutePx)
{
    GraphNode node;
    const Mathematics::Rect rect{12.f, 24.f, 180.f, 60.f};
    EXPECT_TRUE(node.SetGraphRect(rect));
    EXPECT_FALSE(node.SetGraphRect(rect));

    Mathematics::Rect got{};
    ASSERT_TRUE(UI::Layout::TryGetAbsolutePosition(node, got));
    EXPECT_EQ(got.X, 12.f);
    EXPECT_EQ(got.Y, 24.f);
    EXPECT_EQ(got.Width, 180.f);
    EXPECT_EQ(got.Height, 60.f);
}

TEST(GraphNodeLayoutTests, PortsGrowOnly)
{
    GraphPortedNode node;
    node.EnsurePortCount(2, 2);
    EXPECT_EQ(node.InputPortCount(), 2u);
    EXPECT_EQ(node.OutputPortCount(), 2u);
    GraphPort* in0 = node.InputPortAt(0);
    GraphPort* out1 = node.OutputPortAt(1);
    ASSERT_NE(in0, nullptr);
    ASSERT_NE(out1, nullptr);

    node.EnsurePortCount(4, 3);
    EXPECT_EQ(node.InputPortCount(), 4u);
    EXPECT_EQ(node.OutputPortCount(), 3u);
    EXPECT_EQ(node.InputPortAt(0), in0);
    EXPECT_EQ(node.OutputPortAt(1), out1);

    node.EnsurePortCount(1, 1);
    EXPECT_EQ(node.InputPortCount(), 4u);
    EXPECT_EQ(node.OutputPortCount(), 3u);
}

TEST(GraphNodeLayoutTests, SetGraphRectScalesWithFakeZoom)
{
    GraphNode node;
    constexpr float zoom = 2.0f;
    const Mathematics::Rect rect{
        12.0f * zoom,
        24.0f * zoom,
        GraphNodeMetrics::kNodeWidth * zoom,
        GraphNodeMetrics::kNodeHeight * zoom
    };
    EXPECT_TRUE(node.SetGraphRect(rect));

    Mathematics::Rect got{};
    ASSERT_TRUE(UI::Layout::TryGetAbsolutePosition(node, got));
    EXPECT_EQ(got.X, 24.0f);
    EXPECT_EQ(got.Y, 48.0f);
    EXPECT_EQ(got.Width, GraphNodeMetrics::kNodeWidth * zoom);
    EXPECT_EQ(got.Height, GraphNodeMetrics::kNodeHeight * zoom);
}

TEST(GraphNodeLayoutTests, BindModelPlacesPortsByPortCountPercent)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-ports";
    model.TypeId = "Add";
    model.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    model.Ports.push_back({"b", Graph::PortDirection::In, "float", "B"});
    model.Ports.push_back({"c", Graph::PortDirection::In, "float", "C"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    const float height = GraphNodeMetrics::GetNodeBaseHeight(model);
    const std::optional<StyleLength> top0 = node.FindPort("a")->Overrides().Get(Style::PositionTop);
    const std::optional<StyleLength> top1 = node.FindPort("b")->Overrides().Get(Style::PositionTop);
    const std::optional<StyleLength> top2 = node.FindPort("c")->Overrides().Get(Style::PositionTop);
    ASSERT_TRUE(top0.has_value() && top0->IsPercent());
    ASSERT_TRUE(top1.has_value() && top1->IsPercent());
    ASSERT_TRUE(top2.has_value() && top2->IsPercent());
    EXPECT_FLOAT_EQ(top0->Value, GraphNodeMetrics::PortTopPercent(0, 3, height));
    EXPECT_FLOAT_EQ(top1->Value, GraphNodeMetrics::PortTopPercent(1, 3, height));
    EXPECT_FLOAT_EQ(top2->Value, GraphNodeMetrics::PortTopPercent(2, 3, height));
    EXPECT_GT(top1->Value, top0->Value);
    EXPECT_GT(top2->Value, top1->Value);

    const std::optional<StyleLength> portH = node.FindPort("a")->Overrides().Get(Style::Height);
    ASSERT_TRUE(portH.has_value() && portH->IsPercent());
    EXPECT_FLOAT_EQ(portH->Value, GraphNodeMetrics::PortHeightPercent(height));
    EXPECT_LT(portH->Value, 16.667f);

    const std::optional<StyleLength> portW = node.FindPort("a")->Overrides().Get(Style::Width);
    ASSERT_TRUE(portW.has_value() && portW->IsPercent());
    const float uniformW = GraphNodeMetrics::GetNodeWidth(model);
    EXPECT_FLOAT_EQ(portW->Value, GraphNodeMetrics::PortWidthPercent(uniformW));
    EXPECT_NEAR(portW->Value,
                GraphNodeMetrics::kPortSizeGraph / uniformW * 100.f, 0.01f);

    UIElement* header = FindByClass(node, "graph-node-header");
    ASSERT_NE(header, nullptr);
    const std::optional<StyleLength> headerH = header->Overrides().Get(Style::Height);
    ASSERT_TRUE(headerH.has_value() && headerH->IsPercent());
    EXPECT_FLOAT_EQ(headerH->Value, GraphNodeMetrics::HeaderHeightPercent(height));
    EXPECT_LT(headerH->Value, 46.667f);
}

TEST(GraphNodeLayoutTests, OneRowNodeKeepsCanonicalHeaderAndPortPercents)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-one";
    model.TypeId = "Normal";
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float3", "Normal"});
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    const float height = GraphNodeMetrics::GetNodeBaseHeight(model);
    EXPECT_FLOAT_EQ(height, GraphNodeMetrics::kNodeHeight);
    EXPECT_NEAR(GraphNodeMetrics::HeaderHeightPercent(height), 46.667f, 0.01f);
    EXPECT_NEAR(GraphNodeMetrics::PortHeightPercent(height), 16.667f, 0.01f);
}

TEST(GraphNodeLayoutTests, BindModelAssignsRealPortIdsAndHidesExtra)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    node.EnsurePortCount(3, 3);

    Graph::Node model;
    model.Id = "n-bind";
    model.TypeId = "Add";
    model.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, true, false, false, false});

    EXPECT_EQ(node.GetModelNodeId(), "n-bind");
    EXPECT_TRUE(node.HasClass("search-match"));
    ASSERT_NE(node.FindPort("a"), nullptr);
    EXPECT_EQ(node.FindPort("a")->GetPortId(), "a");
    ASSERT_NE(node.FindPort("out"), nullptr);
    EXPECT_EQ(node.FindPort("out")->GetPortId(), "out");
    EXPECT_EQ(node.InputPortCount(), 3u);
    EXPECT_EQ(node.OutputPortCount(), 3u);

    const std::optional<DisplayMode> extraIn = node.InputPortAt(1)->Overrides().Get(Style::Display);
    ASSERT_TRUE(extraIn.has_value());
    EXPECT_EQ(*extraIn, DisplayMode::None);
    const std::optional<DisplayMode> extraOut = node.OutputPortAt(1)->Overrides().Get(Style::Display);
    ASSERT_TRUE(extraOut.has_value());
    EXPECT_EQ(*extraOut, DisplayMode::None);
}

TEST(GraphNodeLayoutTests, BindModelAnimationTitleUsesDisplayName)
{
    UIRegistration::RegisterBuiltInControls();
    (void)GraphNodeRegistry::Get();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-clip";
    model.TypeId = "ClipPlayer";
    node.BindModel(model, "animation", GraphNodeVisualState{false, false, false, false, false});

    UIElement* titleEl = FindByClass(node, "graph-node-title");
    ASSERT_NE(titleEl, nullptr);
    auto* title = dynamic_cast<Label*>(titleEl);
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->GetText(), "Clip Player");

    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false});
    EXPECT_EQ(title->GetText(), "ClipPlayer");
}

TEST(GraphNodeLayoutTests, BindModelFloatConstantRealizesInlineFieldAndResetClearsIt)
{
    UIRegistration::RegisterBuiltInControls();

    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    GraphPortedNode* node = pool.Acquire();
    ASSERT_NE(node, nullptr);

    Graph::Node model;
    model.Id = "const-0";
    model.TypeId = "FloatConstant";
    model.Parameters["value"] = 3.5f;
    node->BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    EXPECT_TRUE(node->GetId().empty());
    UIElement* field = FindByClass(*node, "float-field");
    if (!field)
        field = FindByClass(*node, "graph-inline-field");
    ASSERT_NE(field, nullptr);
    EXPECT_FALSE(IsDisplayNone(*field));
    auto* floatField = dynamic_cast<FloatField*>(field);
    ASSERT_NE(floatField, nullptr);
    EXPECT_FLOAT_EQ(floatField->GetValue(), 3.5f);
    UIElement* host = FindByClass(*node, "graph-inline-editors");
    ASSERT_NE(host, nullptr);
    EXPECT_TRUE(host->HasClass("graph-single-float"));

    pool.Recycle(node);
    EXPECT_TRUE(IsDisplayNone(*node));
    EXPECT_TRUE(node->GetId().empty());
    EXPECT_TRUE(node->GetModelNodeId().empty());
    EXPECT_TRUE(IsDisplayNone(*field));
    EXPECT_FALSE(host->HasClass("graph-single-float"));

    auto isEditorIdentity = [](const UIElement& el)
    {
        return el.HasClass("graph-inline-field") ||
               el.HasClass("float-field") ||
               el.HasClass("float-field-input") ||
               el.HasClass("vector3-field") ||
               el.HasClass("dropdown") ||
               el.HasClass("checkbox") ||
               el.HasClass("text-field") ||
               el.HasClass("text-input");
    };
    std::function<void(const UIElement&)> expectPooledIdsEmpty = [&](const UIElement& el)
    {
        if (isEditorIdentity(el))
            return;
        EXPECT_TRUE(el.GetId().empty()) << el.GetId();
        for (const auto& child : el.GetChildren())
        {
            if (child)
                expectPooledIdsEmpty(*child);
        }
    };
    expectPooledIdsEmpty(*node);
}

TEST(GraphNodeLayoutTests, BindModelStateRealizesTitleField)
{
    UIRegistration::RegisterBuiltInControls();

    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    GraphPortedNode* node = pool.Acquire();
    ASSERT_NE(node, nullptr);

    Graph::Node model;
    model.Id = "state-0";
    model.TypeId = "State";
    model.Parameters["title"] = std::string("Idle");
    node->BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false});

    UIElement* field = FindByClass(*node, "text-field");
    if (!field)
        field = FindByClass(*node, "graph-inline-field");
    ASSERT_NE(field, nullptr);
    auto* textField = dynamic_cast<TextField*>(field);
    ASSERT_NE(textField, nullptr);
    EXPECT_EQ(textField->GetValue(), "Idle");
    EXPECT_TRUE(node->GetId().empty());

    UIElement* titleEl = FindByClass(*node, "graph-node-title");
    ASSERT_NE(titleEl, nullptr);
    auto* title = dynamic_cast<Label*>(titleEl);
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->GetText(), "Idle");

    pool.Recycle(node);
    EXPECT_TRUE(IsDisplayNone(*node));
    EXPECT_TRUE(node->GetModelNodeId().empty());
    EXPECT_TRUE(IsDisplayNone(*field));
}

namespace {

UIEvent MakePressEvent(UIElement* target, int button)
{
    UIEvent e;
    e.Id = kEventMouseDown;
    e.Button = button;
    e.Target = target;
    e.CurrentTarget = target;
    return e;
}

} // namespace

/* Editor surfaces claim their own primary presses (capture + stop) so the
   canvas never filters pointer events by widget class name; right/middle
   presses bubble on so pan and the context menu keep working over editors. */
TEST(GraphNodeLayoutTests, EditorHostsClaimPrimaryPressesAndYieldSecondary)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-vec";
    model.TypeId = "Vec2Probe";
    model.Ports.push_back({"value", Graph::PortDirection::In, "vec2", "Value"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* value = node.FindPort("value");
    ASSERT_NE(value, nullptr);
    UIElement* editors = FindByClass(*value, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);

    UIEvent left = MakePressEvent(editors, Input::kMouseButton_Left);
    editors->DispatchEvent(left);
    EXPECT_TRUE(left.Handled);
    EXPECT_EQ(left.CaptureRequested, editors);

    UIEvent right = MakePressEvent(editors, Input::kMouseButton_Right);
    editors->DispatchEvent(right);
    EXPECT_FALSE(right.Handled);
    EXPECT_EQ(right.CaptureRequested, nullptr);

    UIEvent middle = MakePressEvent(editors, Input::kMouseButton_Middle);
    editors->DispatchEvent(middle);
    EXPECT_FALSE(middle.Handled);
    EXPECT_EQ(middle.CaptureRequested, nullptr);
}

/* The expanded-view detail rows' value hosts carry the same claim. */
TEST(GraphNodeLayoutTests, DetailRowValueHostClaimsPrimaryPresses)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-detail";
    model.TypeId = "Probe";
    model.Parameters["speed"] = 1.0f;

    GraphNodeEditHost host;
    host.ExpandedView = true;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    UIElement* valueHost = FindByClass(node, "graph-detail-value");
    ASSERT_NE(valueHost, nullptr);

    UIEvent left = MakePressEvent(valueHost, Input::kMouseButton_Left);
    valueHost->DispatchEvent(left);
    EXPECT_TRUE(left.Handled);
    EXPECT_EQ(left.CaptureRequested, valueHost);

    UIEvent right = MakePressEvent(valueHost, Input::kMouseButton_Right);
    valueHost->DispatchEvent(right);
    EXPECT_FALSE(right.Handled);
    EXPECT_EQ(right.CaptureRequested, nullptr);
}

TEST(GraphNodeLayoutTests, EditKeysIgnoredWhenInlineFieldFocused)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    auto field = std::make_unique<FloatField>();
    field->AddClass("graph-inline-field");
    FloatField* raw = field.get();
    node.AddChild(std::move(field));

    EXPECT_TRUE(GraphCanvasShouldIgnoreEditKeys(raw));
    EXPECT_FALSE(GraphCanvasShouldIgnoreEditKeys(&node));
    EXPECT_FALSE(GraphCanvasShouldIgnoreEditKeys(nullptr));
}

TEST(GraphNodeLayoutTests, MaterialUnconnectedVec2InputHidesPortEditors)
{
    UIRegistration::RegisterBuiltInControls();

    MaterialGraphNode node;
    Graph::Node model;
    model.Id = "n-uv";
    model.TypeId = "SampleTexture";
    model.Ports.push_back({"uv", Graph::PortDirection::In, "vec2", "UV"});
    model.Ports.push_back({"tex", Graph::PortDirection::Out, "float4", "RGBA"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* uv = node.FindPort("uv");
    ASSERT_NE(uv, nullptr);
    EXPECT_FALSE(uv->HasClass("has-inline-editor"));
    UIElement* editors = FindByClass(*uv, "graph-inline-editors");
    if (editors)
        EXPECT_TRUE(IsDisplayNone(*editors));
}

TEST(GraphNodeLayoutTests, GameLogicUnconnectedVec2InputGetsTwoDistinctFloatFields)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-vec";
    model.TypeId = "Vec2Probe";
    model.Ports.push_back({"value", Graph::PortDirection::In, "vec2", "Value"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* value = node.FindPort("value");
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->HasClass("has-inline-editor"));
    EXPECT_FALSE(value->HasClass("bool-editor"));
    UIElement* editors = FindByClass(*value, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);
    EXPECT_FALSE(IsDisplayNone(*editors));

    const std::vector<FloatField*> fields = CollectVisibleFloatFields(*editors);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_NE(fields[0], fields[1]);
}

TEST(GraphNodeLayoutTests, Vec2ConstantDoesNotTakeSingleFloatClass)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-v2";
    model.TypeId = "Vec2Constant";
    model.Parameters["x"] = 0.25f;
    model.Parameters["y"] = 0.75f;
    model.Ports.push_back({"value", Graph::PortDirection::Out, "float2", "Value"});
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    UIElement* host = FindByClass(node, "graph-inline-editors");
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->HasClass("graph-single-float"));
    EXPECT_FALSE(IsDisplayNone(*host));
    ASSERT_EQ(CollectVisibleFloatFields(*host).size(), 2u);
}

TEST(GraphNodeLayoutTests, TimeDoesNotTakeSingleFloatClass)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-time";
    model.TypeId = "Time";
    model.Parameters["scale"] = 1.0f;
    model.Parameters["offset"] = 0.0f;
    model.Ports.push_back({"time", Graph::PortDirection::Out, "float", "Time"});
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    UIElement* host = FindByClass(node, "graph-inline-editors");
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->HasClass("graph-single-float"));
    EXPECT_FALSE(IsDisplayNone(*host));
    ASSERT_EQ(CollectVisibleFloatFields(*host).size(), 2u);
}

TEST(GraphNodeLayoutTests, Vec4ConstantDoesNotTakeSingleFloatClass)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-v4";
    model.TypeId = "Vec4Constant";
    model.Parameters["x"] = 0.1f;
    model.Parameters["y"] = 0.2f;
    model.Parameters["z"] = 0.3f;
    model.Parameters["w"] = 1.0f;
    model.Ports.push_back({"value", Graph::PortDirection::Out, "float4", "Value"});
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false});

    UIElement* host = FindByClass(node, "graph-inline-editors");
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->HasClass("graph-single-float"));
    EXPECT_FALSE(IsDisplayNone(*host));
    ASSERT_EQ(CollectVisibleFloatFields(*host).size(), 4u);
}

TEST(GraphNodeLayoutTests, SingleFloatCssRuleIsScopedToHostClass)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    const char* scoped = ".graph-node-content .graph-single-float .graph-inline-field.float-field";
    const size_t rule = css.find(scoped);
    ASSERT_NE(rule, std::string::npos)
        << "56% float width is no longer scoped to the FloatConstant host class";
    const size_t end = css.find('}', rule);
    ASSERT_NE(end, std::string::npos);
    const std::string body = css.substr(rule, end - rule);
    EXPECT_NE(body.find("56%"), std::string::npos)
        << "the scoped FloatConstant rule no longer sets the 56% column";

    EXPECT_EQ(css.find(".graph-node-content .graph-inline-field.float-field"), std::string::npos)
        << "unscoped 56% on every float-field wraps Vec2/Time; keep graph-single-float on the selector";

    EXPECT_NE(css.find("width: 28%"), std::string::npos)
        << "the 28% multi-float row rule is gone, so the scoped 56% has no baseline";
}

TEST(GraphNodeLayoutTests, ColorConstantShowsSwatchWithoutPortRgbFields)
{
    UIRegistration::RegisterBuiltInControls();

    /* The colour rides in a synthetic "Color" detail row rather than an inline
       swatch: r/g/b are detail rows of their own on this node type, and an
       inline swatch shares their band and draws underneath them. */
    MaterialGraphNode node;
    Graph::Model model;
    Graph::Node colorNode;
    colorNode.Id = "n-col";
    colorNode.TypeId = "ColorConstant";
    colorNode.Parameters["r"] = 0.08;
    colorNode.Parameters["g"] = 0.28;
    colorNode.Parameters["b"] = 1.0;
    colorNode.Ports.push_back({"value", Graph::PortDirection::Out, "float3", "RGB"});
    model.Nodes.push_back(colorNode);

    GraphNodeEditHost host;
    host.Model = &model;
    node.BindModel(model.Nodes.front(), Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    UIElement* swatch = FindByClass(node, "graph-detail-swatch");
    ASSERT_NE(swatch, nullptr);
    EXPECT_FALSE(IsDisplayNone(*swatch));
    EXPECT_TRUE(swatch->HasClass("graph-detail-swatch-pickable"));
    EXPECT_EQ(FindByClass(node, "graph-color-swatch"), nullptr);

    GraphPort* value = node.FindPort("value");
    ASSERT_NE(value, nullptr);
    UIElement* editors = FindByClass(*value, "graph-inline-editors");
    if (editors)
        EXPECT_TRUE(IsDisplayNone(*editors));
}

TEST(GraphNodeLayoutTests, GameLogicUnconnectedBoolKeepsConditionLabelAndMarksEditor)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-tr";
    model.TypeId = "Transition";
    model.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    model.Ports.push_back({"condition", Graph::PortDirection::In, "bool", "Condition"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* condition = node.FindPort("condition");
    ASSERT_NE(condition, nullptr);
    EXPECT_TRUE(condition->HasClass("has-inline-editor"));
    EXPECT_TRUE(condition->HasClass("bool-editor"));

    Label* label = nullptr;
    for (const auto& child : condition->GetChildren())
    {
        if (child && child->HasClass("graph-port-label"))
            label = dynamic_cast<Label*>(child.get());
    }
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetText(), "Condition");
    EXPECT_FALSE(IsDisplayNone(*label));

    UIElement* editors = FindByClass(*condition, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);
    EXPECT_FALSE(IsDisplayNone(*editors));
    UIElement* box = FindByClass(*editors, "checkbox");
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(IsDisplayNone(*box));
    EXPECT_NE(box, static_cast<UIElement*>(label));

    GraphPort* in = node.FindPort("in");
    ASSERT_NE(in, nullptr);
    EXPECT_FALSE(in->HasClass("has-inline-editor"));
    EXPECT_FALSE(in->HasClass("bool-editor"));
}

TEST(GraphNodeLayoutTests, ConnectedBoolInputHidesEditorClasses)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-tr-wired";
    model.TypeId = "Transition";
    model.Ports.push_back({"condition", Graph::PortDirection::In, "bool", "Condition"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return true; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* condition = node.FindPort("condition");
    ASSERT_NE(condition, nullptr);
    EXPECT_FALSE(condition->HasClass("has-inline-editor"));
    EXPECT_FALSE(condition->HasClass("bool-editor"));
    UIElement* editors = FindByClass(*condition, "graph-inline-editors");
    if (editors)
        EXPECT_TRUE(IsDisplayNone(*editors));
}

TEST(GraphNodeLayoutTests, UnconnectedStringInputGetsTextField)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-sound";
    model.TypeId = "PlaySound";
    model.Ports.push_back({"clipGuid", Graph::PortDirection::In, "string", "Clip"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    model.Parameters["clipGuid"] = std::string("clip-guid");

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* clip = node.FindPort("clipGuid");
    ASSERT_NE(clip, nullptr);
    UIElement* editors = FindByClass(*clip, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);
    EXPECT_FALSE(IsDisplayNone(*editors));
    UIElement* field = FindByClass(*editors, "text-field");
    ASSERT_NE(field, nullptr);
    auto* textField = dynamic_cast<TextField*>(field);
    ASSERT_NE(textField, nullptr);
    EXPECT_EQ(textField->GetValue(), "clip-guid");
}

TEST(GraphNodeLayoutTests, SurplusAcquireReusesHiddenSlot)
{
    auto layer = std::make_unique<UIElement>();
    GraphNodePool pool(layer.get(), nullptr);
    GraphPortedNode* first = pool.Acquire();
    ASSERT_NE(first, nullptr);
    pool.Recycle(first);
    EXPECT_EQ(pool.SlotCount(), 1u);

    GraphPortedNode* again = pool.Acquire();
    EXPECT_EQ(again, first);
    EXPECT_EQ(pool.SlotCount(), 1u);
    EXPECT_FALSE(IsDisplayNone(*again));
}

TEST(GraphNodeLayoutTests, CssMathFunctionsResolveAgainstLengthScale)
{
    UIRegistration::RegisterBuiltInControls();
    using namespace GameEngine::UIParsing;

    // Standard CSS: calc() scales both ways; min() caps at an authored bound;
    // the var() fallback makes both behave as plain px with no scale in scope.
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".scaled-probe {"
        "  padding-left: min(24px, calc(8px * var(--length-scale, 1)));"
        "  padding-right: calc(2px * var(--length-scale, 1));"
        "}",
        sheet));
    const std::vector<const Stylesheet*> sheets{&sheet};

    UIElement parent;
    UIElement* probe = nullptr;
    {
        auto child = std::make_unique<UIElement>();
        child->AddClass("scaled-probe");
        probe = child.get();
        parent.AddChild(std::move(child));
    }

    static const StringId kLengthScaleVar = HashStringId("--length-scale");
    const ElementState state{};

    auto resolveProbe = [&](const char* scale) -> ResolvedStyle
    {
        parent.Overrides().SetCustom(kLengthScaleVar, scale);
        const ResolvedStyle parentStyle = CSSParser::ComputeStyleFor(parent, sheet, state);
        return CSSParser::ComputeStyleFor(*probe, sheets, state, &parentStyle);
    };

    {
        const ResolvedStyle style = resolveProbe("0.5");
        EXPECT_FLOAT_EQ(style.Layout.Padding.Left, 4.0f) << "min picks the scaled 4px";
        EXPECT_FLOAT_EQ(style.Layout.Padding.Right, 1.0f) << "calc scales 2px down";
    }
    {
        const ResolvedStyle style = resolveProbe("2.5");
        EXPECT_FLOAT_EQ(style.Layout.Padding.Left, 20.0f)
            << "the scaled 20px stays under the 24px cap";
        EXPECT_FLOAT_EQ(style.Layout.Padding.Right, 5.0f) << "calc keeps scaling above 1";
    }
}

TEST(GraphNodeLayoutTests, CssMathFoldsAtParseAndRejectsWhatItCannotFold)
{
    UIRegistration::RegisterBuiltInControls();
    using namespace GameEngine::UIParsing;

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".m { width: 40px; }"
        ".m {"
        "  width: calc(10px * 3);"          // folds: 30px
        "  height: calc((4px + 2px) / 2);"  // nested + division: 3px
        "  min-width: max(5px, 2px, 9px);"  // n-ary max: 9px
        "  padding-left: calc(50% - 4px);"  // px/% mix cannot fold: rejected
        "  padding-right: calc(2px*3);"     // '*' needs no spaces: 6px
        "}",
        sheet));

    UIElement el;
    el.AddClass("m");
    const ElementState state{};
    const ResolvedStyle style = CSSParser::ComputeStyleFor(el, sheet, state);

    ASSERT_TRUE(style.Layout.Width.IsPx());
    EXPECT_FLOAT_EQ(style.Layout.Width.Value, 30.0f);
    ASSERT_TRUE(style.Layout.Height.IsPx());
    EXPECT_FLOAT_EQ(style.Layout.Height.Value, 3.0f);
    ASSERT_TRUE(style.Layout.MinWidth.IsPx());
    EXPECT_FLOAT_EQ(style.Layout.MinWidth.Value, 9.0f);
    EXPECT_FLOAT_EQ(style.Layout.Padding.Left, 0.0f)
        << "an unfoldable calc is rejected whole; the padding stays unset";
    EXPECT_FLOAT_EQ(style.Layout.Padding.Right, 6.0f);
}

TEST(GraphNodeLayoutTests, CssMathRejectsAParenthesisBombWithoutCrashing)
{
    UIRegistration::RegisterBuiltInControls();
    using namespace GameEngine::UIParsing;

    std::string bomb = "calc(";
    for (int i = 0; i < 4096; ++i)
        bomb += '(';
    bomb += "1px";
    for (int i = 0; i < 4096; ++i)
        bomb += ')';
    bomb += ')';

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(
        ".m { width: 40px; } .m { width: " + bomb + "; }", sheet));
    UIElement el;
    el.AddClass("m");
    const ElementState state{};
    const ResolvedStyle style = CSSParser::ComputeStyleFor(el, sheet, state);
    ASSERT_TRUE(style.Layout.Width.IsPx());
    EXPECT_FLOAT_EQ(style.Layout.Width.Value, 40.0f)
        << "the bomb is rejected whole and the cascaded 40px stands";
}

TEST(GraphNodeLayoutTests, SetGraphRectPureMoveKeepsSizeAndDedupes)
{
    /* SetGraphRect exposes no fast-path flag: it always takes the full layout
       path (children carry committed rects only a solve refreshes). The drag
       helper owns the position-only fast path and patches child rects itself.
       Either way the override rect dirty-diffs: same rect returns false. */
    GraphNode node;
    const Mathematics::Rect start{12.f, 24.f, 180.f, 60.f};
    ASSERT_TRUE(node.SetGraphRect(start));
    const Mathematics::Rect moved{40.f, 60.f, 180.f, 60.f};
    EXPECT_TRUE(node.SetGraphRect(moved));

    Mathematics::Rect got{};
    ASSERT_TRUE(UI::Layout::TryGetAbsolutePosition(node, got));
    EXPECT_EQ(got.X, 40.f);
    EXPECT_EQ(got.Y, 60.f);
    EXPECT_EQ(got.Width, 180.f);
    EXPECT_EQ(got.Height, 60.f);
    EXPECT_FALSE(node.SetGraphRect(moved));

    const Mathematics::Rect grown{40.f, 60.f, 200.f, 60.f};
    EXPECT_TRUE(node.SetGraphRect(grown));
    ASSERT_TRUE(UI::Layout::TryGetAbsolutePosition(node, got));
    EXPECT_EQ(got.Width, 200.f);
}

TEST(GraphNodeLayoutTests, NodePortLabelsAbbreviateButTooltipsKeepFullNames)
{
    using GraphPortLabels::AbbreviateForNode;

    // Fits: verbatim. Over budget: word-wise standard abbreviations.
    EXPECT_EQ(AbbreviateForNode("Hours"), "Hours");
    EXPECT_EQ(AbbreviateForNode("Cycle Seconds"), "Cycle Sec");
    EXPECT_EQ(AbbreviateForNode("Position Smoothing"), "Pos Smooth");
    EXPECT_EQ(AbbreviateForNode("Rotation Smoothing"), "Rot Smooth");
    // Unknown long words pass through (and ellipsize in CSS): the rule is to
    // add the word to kWordAbbreviations, not to rename the port.
    EXPECT_EQ(AbbreviateForNode("Reverberation"), "Reverberation");

    // The table's declared array size must match its initializer: a stale
    // size value-initializes trailing entries, and empty Word slots would
    // silently stop nothing while shipping dead table rows (this broke once;
    // keep the guard).
    for (const GraphPortLabels::WordAbbreviation& entry : GraphPortLabels::kWordAbbreviations)
    {
        EXPECT_FALSE(entry.Word.empty());
        EXPECT_FALSE(entry.Short.empty());
    }

    UIRegistration::RegisterBuiltInControls();
    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-sky";
    model.TypeId = "SetSkyTimeOfDay";
    model.Ports.push_back({"cycleSeconds", Graph::PortDirection::In, "float", "Cycle Seconds"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* port = node.FindPort("cycleSeconds");
    ASSERT_NE(port, nullptr);
    Label* label = nullptr;
    for (const auto& child : port->GetChildren())
    {
        if (auto* asLabel = dynamic_cast<Label*>(child.get()))
        {
            label = asLabel;
            break;
        }
    }
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->GetText(), "Cycle Sec") << "the node shows the abbreviated form";
    EXPECT_NE(port->GetTooltip().find("Cycle Seconds"), std::string::npos)
        << "the tooltip keeps the full name";
}

TEST(GraphNodeLayoutTests, StackedFloatBoxesShareTheMostConstrainedWidth)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-makevec";
    model.TypeId = "MakeVector3";
    model.Ports.push_back({"x", Graph::PortDirection::In, "float", "X"});
    model.Ports.push_back({"y", Graph::PortDirection::In, "float", "Y"});
    model.Ports.push_back({"z", Graph::PortDirection::In, "float", "Z"});
    model.Ports.push_back({"vector", Graph::PortDirection::Out, "float3", "Vector"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    /* Measured rows: one-letter labels start the editors at 23 units, and
       even the row sharing with "Vector" keeps the full float cap — the
       right reserve clears the label without the box yielding. */
    const float capPct = GraphNodeMetrics::kFloatEditorMaxWidthGraph /
        GraphNodeMetrics::kPortSizeGraph * 100.f;
    const float startPct = (GraphNodeMetrics::NodeEditorStartGraph(model, true, false, false) +
        GraphNodeMetrics::kPortHangGraph) / GraphNodeMetrics::kPortSizeGraph * 100.f;
    for (const char* portId : {"x", "y", "z"})
    {
        GraphPort* port = node.FindPort(portId);
        ASSERT_NE(port, nullptr) << portId;
        UIElement* editors = FindByClass(*port, "graph-inline-editors");
        ASSERT_NE(editors, nullptr) << portId;
        const std::optional<StyleLength> w = editors->Overrides().Get(Style::Width);
        const std::optional<StyleLength> l = editors->Overrides().Get(Style::PositionLeft);
        ASSERT_TRUE(w.has_value() && w->IsPercent()) << portId;
        ASSERT_TRUE(l.has_value() && l->IsPercent()) << portId;
        EXPECT_FLOAT_EQ(w->Value, capPct) << portId << ": equal widths at the float cap";
        EXPECT_FLOAT_EQ(l->Value, startPct) << portId << ": editor starts after the measured label";
    }
    /* The box still ends before the Out label's reserve. */
    EXPECT_LE(GraphNodeMetrics::InlineEditorStartGraphFor(1) +
                  GraphNodeMetrics::kFloatEditorMaxWidthGraph,
              GraphNodeMetrics::GetNodeWidth(model) -
                  GraphNodeMetrics::InlineEditorRightReserveGraph(6));
}

TEST(GraphNodeLayoutTests, ValueInsetsAreAuthoredInZoomScaledUnits)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    EXPECT_NE(css.find("padding-left: calc(6px * var(--length-scale, 1))"),
              std::string::npos)
        << "every value box — wide fields and vec chips alike — shares one "
           "6-unit zoom-scaled inset, matched to the inspector's density";
    EXPECT_EQ(css.find("padding-left: calc(2px * var(--length-scale, 1))"), std::string::npos)
        << "chips share the common inset, not a private 2-unit one";
    EXPECT_EQ(css.find("sdx"), std::string::npos)
        << "no engine-specific sx/sdx units; math functions are standard CSS";
}

TEST(GraphNodeLayoutTests, CssKeepsTitleAndPortInsetsAndLeftAlignsValues)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    EXPECT_NE(css.find("padding-left: calc(3px * var(--length-scale, 1))"), std::string::npos)
        << "header title inset is 3 fixed units: percent would slide with node width";
    EXPECT_NE(css.find("left: 150%"), std::string::npos)
        << "input port labels are 10px from port center";
    const size_t outLabelRule = css.find(".graph-port-output .graph-port-label {");
    ASSERT_NE(outLabelRule, std::string::npos);
    const size_t outLabelEnd = css.find('}', outLabelRule);
    ASSERT_NE(outLabelEnd, std::string::npos);
    const std::string outLabelBody = css.substr(outLabelRule, outLabelEnd - outLabelRule);
    EXPECT_NE(outLabelBody.find("right: 150%"), std::string::npos)
        << "Out labels sit inside the node, left of the output port";
    EXPECT_EQ(outLabelBody.find("left: 150%"), std::string::npos)
        << "left: 150% on Out parks the name outside the node";
    EXPECT_NE(outLabelBody.find("max-width: 500%"), std::string::npos)
        << "Entity/Pressed need a 50-unit Out column, not a 15-unit clip";
    EXPECT_NE(outLabelBody.find("text-align: right"), std::string::npos);
    EXPECT_NE(css.find("width: 5%"), std::string::npos)
        << "port diameter must stay 10/200 of node width";
    EXPECT_NE(css.find("left: -2.5%"), std::string::npos)
        << "input ports must hang half a port past the node edge";
    EXPECT_EQ(css.find("width: 5.556%"), std::string::npos)
        << "5.556% is 10/180; ports would be oversized";
    EXPECT_EQ(css.find("left: -2.778%"), std::string::npos)
        << "2.778% hang is 5/180; ports would sit inside the node";

    const size_t portRule = css.find(".graph-port {\n\tposition: absolute");
    ASSERT_NE(portRule, std::string::npos);
    const size_t portEnd = css.find('}', portRule);
    ASSERT_NE(portEnd, std::string::npos);
    const std::string portBody = css.substr(portRule, portEnd - portRule);
    EXPECT_NE(portBody.find("border-width: 0"), std::string::npos)
        << "port border must not shrink the 10-unit box that port-relative percents resolve against";
    EXPECT_EQ(portBody.find("border-width: 1px"), std::string::npos);

    const size_t valueRule = css.find(".graph-node-value");
    ASSERT_NE(valueRule, std::string::npos);
    const size_t valueEnd = css.find('}', valueRule);
    ASSERT_NE(valueEnd, std::string::npos);
    const std::string valueBody = css.substr(valueRule, valueEnd - valueRule);
    EXPECT_NE(valueBody.find("text-align: left"), std::string::npos)
        << ".graph-node-value must left-align like inspector float fields";
    EXPECT_NE(valueBody.find("padding-left: calc(6px * var(--length-scale, 1))"),
              std::string::npos)
        << "value-summary inset stays proportional at every zoom";
    EXPECT_NE(valueBody.find("background-color: var(--graph-value-bg, #202020)"), std::string::npos)
        << "value summaries must sit in a value box, not a naked Label";
    EXPECT_NE(valueBody.find("var(--graph-value-radius"), std::string::npos)
        << "value boxes must use the zoom-scaled circular radius";

    const size_t headerRule = css.find(".graph-node-header {");
    ASSERT_NE(headerRule, std::string::npos);
    const size_t headerEnd = css.find('}', headerRule);
    ASSERT_NE(headerEnd, std::string::npos);
    const std::string headerBody = css.substr(headerRule, headerEnd - headerRule);
    EXPECT_NE(headerBody.find("var(--graph-node-radius, 4px) var(--graph-node-radius, 4px) 0 0"), std::string::npos)
        << "header radius tracks zoom with the node shell; top-only so a square bar cannot cover the node corners";
    const size_t titleRule = css.find(".graph-node-title {");
    ASSERT_NE(titleRule, std::string::npos);
    const size_t titleEnd = css.find('}', titleRule);
    ASSERT_NE(titleEnd, std::string::npos);
    const std::string titleBody = css.substr(titleRule, titleEnd - titleRule);
    EXPECT_NE(titleBody.find("white-space: nowrap"), std::string::npos)
        << "titles such as Third Person Camera Follow must not wrap at the uniform width";
    EXPECT_NE(titleBody.find("text-overflow: ellipsis"), std::string::npos);
    EXPECT_EQ(css.find("--graph-node-radius: "), std::string::npos)
        << "radii publish continuously from the canvas, not per tier";
}

TEST(GraphNodeLayoutTests, ZoomCssKeepsTypeAndChromeProportional)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    EXPECT_NE(css.find(".graph-z0 { font-size: 7.6px;"), std::string::npos)
        << "z0 must be 13px x kMinZoom, not a floor that grows type on zoom-out";
    EXPECT_NE(css.find(".graph-z1 { font-size: 9.1px;"), std::string::npos);
    EXPECT_NE(css.find(".graph-z2 { font-size: 10.9px;"), std::string::npos);
    EXPECT_NE(css.find(".graph-z3 { font-size: 13px;"), std::string::npos);
    EXPECT_NE(css.find(".graph-z9 { font-size: 38.1px;"), std::string::npos);
    EXPECT_EQ(css.find(".graph-z10"), std::string::npos);
    EXPECT_EQ(css.find("graph-z-lod"), std::string::npos)
        << "no LOD class: type scales with the rect at every tier";

    const size_t boxRule = css.find(".graph-node-layer .graph-port.bool-editor .checkbox-box");
    ASSERT_NE(boxRule, std::string::npos) << "graph checkboxes must override the 18px theme box";
    const size_t boxEnd = css.find('}', boxRule);
    ASSERT_NE(boxEnd, std::string::npos);
    const std::string boxBody = css.substr(boxRule, boxEnd - boxRule);
    EXPECT_EQ(boxBody.find("18px"), std::string::npos)
        << "an 18px checkbox does not shrink with the node";
    EXPECT_NE(boxBody.find("width: 100%"), std::string::npos);
    EXPECT_NE(boxBody.find("height: 100%"), std::string::npos);

    /* One shared class rather than an enumeration of control types: every
       field's editable surface carries .field-editor (TextFieldBase tags it,
       Dropdown's header joins the family), so the inset is one selector. */
    const size_t fieldRule = css.find(".graph-node-layer .field-editor {");
    ASSERT_NE(fieldRule, std::string::npos);
    const size_t fieldEnd = css.find('}', fieldRule);
    ASSERT_NE(fieldEnd, std::string::npos);
    const std::string fieldBody = css.substr(fieldRule, fieldEnd - fieldRule);
    EXPECT_NE(fieldBody.find("padding-left: calc(6px * var(--length-scale, 1))"),
              std::string::npos)
        << "value-box inset stays proportional at every zoom";
    EXPECT_EQ(fieldBody.find("padding-left: 2.222%"), std::string::npos)
        << "percent padding of a port-sized host is almost nothing";
    EXPECT_NE(fieldBody.find("padding-top: 0"), std::string::npos)
        << "inspector 6px vertical padding would blow the port row";
    EXPECT_NE(fieldBody.find("padding-bottom: 0"), std::string::npos);
    EXPECT_NE(fieldBody.find("min-width: 0"), std::string::npos);
    EXPECT_EQ(fieldBody.find("26px"), std::string::npos);
    EXPECT_EQ(fieldBody.find("20%"), std::string::npos)
        << "percent radius is an ellipse on a wide box; value corners must stay circular";
    EXPECT_NE(fieldBody.find("var(--graph-value-radius"), std::string::npos);
    EXPECT_NE(fieldBody.find("background-color: var(--graph-value-bg, #202020)"), std::string::npos)
        << "clip paths and other string fields must paint a value box, not naked glyphs";
    EXPECT_NE(fieldBody.find(".field-editor"), std::string::npos)
        << "TextField inner editors are .field-editor, not .text-input";
    EXPECT_EQ(css.find("--graph-value-radius: "), std::string::npos)
        << "value radius publishes continuously from the canvas";

    const size_t boolHost = css.find(".graph-port.has-inline-editor.bool-editor .graph-inline-editors");
    ASSERT_NE(boolHost, std::string::npos);
    const size_t boolHostEnd = css.find('}', boolHost);
    ASSERT_NE(boolHostEnd, std::string::npos);
    const std::string boolHostBody = css.substr(boolHost, boolHostEnd - boolHost);
    EXPECT_NE(boolHostBody.find("left: 185%"), std::string::npos)
        << "checkbox host starts after a Clip-sized gutter from the port";
    EXPECT_NE(boolHostBody.find("top: -15%"), std::string::npos)
        << "130% host must sit on the port centerline, not hang below the row";
    EXPECT_NE(boolHostBody.find("height: 130%"), std::string::npos);
    EXPECT_NE(boolHostBody.find("width: 130%"), std::string::npos);

    const size_t portEditors = css.find(".graph-port .graph-inline-editors");
    ASSERT_NE(portEditors, std::string::npos);
    const size_t portEditorsEnd = css.find('}', portEditors);
    ASSERT_NE(portEditorsEnd, std::string::npos);
    const std::string portEditorsBody = css.substr(portEditors, portEditorsEnd - portEditors);
    EXPECT_NE(portEditorsBody.find("flex-wrap: nowrap"), std::string::npos)
        << "port fields must shrink in-row; wrap dumps Vector3 axes under the node";

    const size_t nodeRule = css.find(".graph-node {");
    ASSERT_NE(nodeRule, std::string::npos);
    const size_t nodeEnd = css.find('}', nodeRule);
    ASSERT_NE(nodeEnd, std::string::npos);
    const std::string nodeBody = css.substr(nodeRule, nodeEnd - nodeRule);
    EXPECT_NE(nodeBody.find("box-shadow:"), std::string::npos)
        << "nodes keep the pre-refactor drop shadow; paint it on .graph-node, not the clipped body";
    EXPECT_NE(nodeBody.find("overflow: visible"), std::string::npos)
        << "ports and drop shadows must hang outside the node; do not clip .graph-node";
    EXPECT_EQ(nodeBody.find("overflow: hidden"), std::string::npos);

    const size_t valueHost = css.find(".graph-port.has-inline-editor:not(.bool-editor) .graph-inline-editors");
    ASSERT_NE(valueHost, std::string::npos);
    const size_t valueHostEnd = css.find('}', valueHost);
    ASSERT_NE(valueHostEnd, std::string::npos);
    const std::string valueHostBody = css.substr(valueHost, valueHostEnd - valueHost);
    EXPECT_NE(valueHostBody.find("left: 750%"), std::string::npos)
        << "value hosts start after the 60-unit port-label column";
    EXPECT_NE(valueHostBody.find("width: 1100%"), std::string::npos)
        << "1100% of a 10-unit port runs to the right-edge port clearance";
    EXPECT_NE(valueHostBody.find("max-width: 1100%"), std::string::npos);
    EXPECT_NE(valueHostBody.find("height: 180%"), std::string::npos)
        << "value hosts must be shorter than port spacing or stacked X/Y/Z boxes share an edge";
    EXPECT_NE(valueHostBody.find("top: -40%"), std::string::npos);
    EXPECT_EQ(valueHostBody.find("width: 760%"), std::string::npos)
        << "760% of the port cannot fit Offset XYZ and Position Smoothing";
    EXPECT_NE(valueHostBody.find("overflow: hidden"), std::string::npos)
        << "Clip/Volume boxes clip to the host, which ends before Out";

    EXPECT_EQ(css.find(
                  ".graph-port.has-inline-editor.shares-out-row:not(.bool-editor) .graph-inline-editors {"),
              std::string::npos)
        << "shared-row host width is a measured element override now; a fixed CSS width "
           "cannot end before both 'Out' (23 units) and 'Pressed' (50 units)";

    EXPECT_EQ(css.find(".graph-port.has-inline-editor.graph-single-inline .graph-inline-editors"),
              std::string::npos)
        << "compact singles share the same label/value columns as other port editors";
    EXPECT_EQ(css.find(".graph-port.has-inline-editor.vec-editor .graph-inline-editors"),
              std::string::npos)
        << "Vector3 hosts share the float editor slot so Offset and Hours line up";

    const size_t labelClip = css.find(
        ".graph-port.has-inline-editor:not(.bool-editor) .graph-port-label");
    ASSERT_NE(labelClip, std::string::npos)
        << "port labels with value boxes must clip at the label column, not run under the box";
    const size_t labelClipEnd = css.find('}', labelClip);
    ASSERT_NE(labelClipEnd, std::string::npos);
    const std::string labelClipBody = css.substr(labelClip, labelClipEnd - labelClip);
    EXPECT_NE(labelClipBody.find("max-width: 600%"), std::string::npos);
    EXPECT_NE(labelClipBody.find("text-overflow: ellipsis"), std::string::npos);

    const size_t portField = css.find(".graph-port .graph-inline-field {");
    ASSERT_NE(portField, std::string::npos);
    const size_t portFieldEnd = css.find('}', portField);
    ASSERT_NE(portFieldEnd, std::string::npos);
    const std::string portFieldBody = css.substr(portField, portFieldEnd - portField);
    EXPECT_NE(portFieldBody.find("min-width: 90%"), std::string::npos)
        << "Volume/Pitch/Clip floats need a 90% host floor so the box stays readable";
    EXPECT_EQ(portFieldBody.find("min-width: 0"), std::string::npos)
        << "min-width 0 lets the Volume/Pitch/Clip value box collapse";

    const size_t axisRule = css.find(".graph-node-layer .vector3-field > .vector3-label");
    ASSERT_NE(axisRule, std::string::npos)
        << "graph Vector3 X/Y/Z chips must override inspector 13px / 14px";
    const size_t axisEnd = css.find('}', axisRule);
    ASSERT_NE(axisEnd, std::string::npos);
    const std::string axisBody = css.substr(axisRule, axisEnd - axisRule);
    EXPECT_NE(axisBody.find("font-size: inherit"), std::string::npos)
        << "axis letters must follow .graph-zK, not inspector 13px";
    EXPECT_EQ(axisBody.find("13px"), std::string::npos);
    EXPECT_EQ(axisBody.find("14px"), std::string::npos);
    EXPECT_EQ(axisBody.find("27px"), std::string::npos);
    EXPECT_EQ(axisBody.find("em"), std::string::npos)
        << "em is parsed as px; 1em collapsed axis chips to 1px";
    EXPECT_NE(axisBody.find("min-width: 6%"), std::string::npos)
        << "axis column must be percent of the vec host so it tracks zoom with the port";
    EXPECT_NE(axisBody.find("flex: 0 0 6%"), std::string::npos)
        << "one-glyph axis labels take glyph width; every point saved is chip width";

    const size_t axisComp = css.find(".graph-node-layer .vector3-component-z");
    ASSERT_NE(axisComp, std::string::npos);
    const size_t axisCompEnd = css.find('}', axisComp);
    ASSERT_NE(axisCompEnd, std::string::npos);
    const std::string axisCompBody = css.substr(axisComp, axisCompEnd - axisComp);
    EXPECT_NE(axisCompBody.find("min-width: 24%"), std::string::npos)
        << "XYZ value boxes keep 24% of the vec host: a signed two-decimal fits";
    EXPECT_NE(axisCompBody.find("flex: 1 1 0"), std::string::npos);
    EXPECT_EQ(axisCompBody.find("min-width: 0"), std::string::npos)
        << "min-width 0 lets Offset XYZ collapse under the axis chips";

    /* Vec chips take their inset from the ONE shared value-box rule, and they
       reach it through the same .field-editor class every other field editor
       carries — a vector3's axis inputs are FloatFields, and TextFieldBase
       tags their editors. So the rule needs no vector3 spelling at all; a
       private duplicate block would be the regression. */
    EXPECT_EQ(css.find(
                  ".graph-port.has-inline-editor.vec-editor .vector3-field .float-field-input"),
              std::string::npos)
        << "no private vec-chip padding block: the shared rule already covers it";
    EXPECT_EQ(css.find("padding-left: 12px"), std::string::npos);

    const size_t vecFieldOverride = css.find(
        ".graph-port.has-inline-editor.vec-editor .graph-inline-field {");
    ASSERT_NE(vecFieldOverride, std::string::npos);
    const size_t vecFieldOverrideEnd = css.find('}', vecFieldOverride);
    ASSERT_NE(vecFieldOverrideEnd, std::string::npos);
    const std::string vecFieldOverrideBody =
        css.substr(vecFieldOverride, vecFieldOverrideEnd - vecFieldOverride);
    EXPECT_NE(vecFieldOverrideBody.find("min-width: 0"), std::string::npos)
        << "vec-editor fields override the 90% floor so XYZ can share the slot";

    /* Dropdown headers are value boxes too. They are not TextFieldBase, so
       Dropdown adds .field-editor at its creation site rather than the sheet
       naming .dropdown-header a second time. */
    EXPECT_EQ(css.find(".graph-node-layer .graph-inline-field .dropdown-header"),
              std::string::npos)
        << "dropdown headers reach the shared rule by class, not by enumeration";
    EXPECT_NE(css.find("border-color: var(--ui_color_accent)"), std::string::npos)
        << "graph value boxes must use the inspector accent outline on hover/focus";
}

TEST(GraphNodeLayoutTests, InlineEditorHostsEndInsideNodeAndOutGutter)
{
    constexpr float port = GraphNodeMetrics::kPortSizeGraph;
    constexpr float nodeW = GraphNodeMetrics::kNodeWidth;
    constexpr float portLeft = -GraphNodeMetrics::kPortHangGraph;
    constexpr float outGutter = nodeW - GraphNodeMetrics::kOutGutterGraph;

    EXPECT_FLOAT_EQ(GraphNodeMetrics::kNodeWidth, GraphNodeMetrics::kGridSizeGraph * 10.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kInlineEditorLeftPercentOfPort, 750.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kInlineEditorWidthPercentOfPort, 1100.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kInlineLabelMaxWidthPercentOfPort, 600.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kInlineEditorHeightPercentOfPort, 180.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kOutGutterGraph, 20.f);
    EXPECT_FLOAT_EQ(GraphNodeMetrics::kOutLabelMaxWidthPercentOfPort, 500.f);
    /* Measured rows: the right reserve keeps any host clear of its row's
       Out label, whatever the name; absurd names saturate the ellipsis
       column instead of erasing the value box. */
    {
        EXPECT_FLOAT_EQ(GraphNodeMetrics::InlineEditorRightReserveGraph(0),
                        GraphNodeMetrics::kOutGutterGraph);
        EXPECT_FLOAT_EQ(GraphNodeMetrics::InlineEditorRightReserveGraph(3),
                        GraphNodeMetrics::kOutLabelRightInsetGraph +
                            GraphNodeMetrics::OutLabelWidthGraph(3) +
                            GraphNodeMetrics::kHostOutLabelGapGraph);
        EXPECT_FLOAT_EQ(GraphNodeMetrics::OutLabelWidthGraph(7),
                        GraphNodeMetrics::kOutLabelColumnGraph)
            << "7 glyphs saturate the ellipsis column";
        EXPECT_FLOAT_EQ(GraphNodeMetrics::InlineEditorRightReserveGraph(64),
                        GraphNodeMetrics::InlineEditorRightReserveGraph(7))
            << "absurd Out names cost no more than the saturated column";
    }
    EXPECT_GT(GraphNodeMetrics::kPortSpacing,
              GraphNodeMetrics::kPortSizeGraph *
                  (GraphNodeMetrics::kInlineEditorHeightPercentOfPort / 100.f))
        << "port-row value boxes must leave a gap; host height must be below port spacing";
    EXPECT_FLOAT_EQ(
        GraphNodeMetrics::kInlineLabelStartGraph + GraphNodeMetrics::kInlineLabelColumnGraph +
            GraphNodeMetrics::kInlineEditorWidthGraph + GraphNodeMetrics::kOutGutterGraph,
        nodeW);

    const float editorStart = portLeft + port * (GraphNodeMetrics::kInlineEditorLeftPercentOfPort / 100.f);
    const float editorEnd = editorStart + port * (GraphNodeMetrics::kInlineEditorWidthPercentOfPort / 100.f);
    EXPECT_FLOAT_EQ(editorStart, GraphNodeMetrics::kInlineEditorStartGraph);
    EXPECT_FLOAT_EQ(editorEnd, GraphNodeMetrics::kInlineEditorEndGraph);
    EXPECT_LE(editorEnd, nodeW);
    EXPECT_LE(editorEnd, outGutter) << "value boxes must end before the Out column";
}

TEST(GraphNodeLayoutTests, SelectionChromePaintsOnTheRingOverlay)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    const size_t restRule = css.find(".graph-node {");
    ASSERT_NE(restRule, std::string::npos);
    const size_t restEnd = css.find('}', restRule);
    ASSERT_NE(restEnd, std::string::npos);
    const std::string restBody = css.substr(restRule, restEnd - restRule);
    EXPECT_NE(restBody.find("border-width: 0"), std::string::npos)
        << "no layout border: a reserved border opens a node-background gutter that "
           "keeps the header fill off the edge";

    const size_t ringRule = css.find(".graph-node-ring {");
    ASSERT_NE(ringRule, std::string::npos)
        << "hover/selection paint on the ring overlay so the line renders above the header";
    const size_t ringEnd = css.find('}', ringRule);
    const std::string ringBody = css.substr(ringRule, ringEnd - ringRule);
    EXPECT_NE(ringBody.find("pointer-events: none"), std::string::npos);
    EXPECT_NE(css.find(".graph-node:hover > .graph-node-ring"), std::string::npos);
    /* selected and search-match both mean "accent ring", so the node carries
       ring-accent when either holds and the sheet says it once. */
    EXPECT_NE(css.find(".graph-node.ring-accent > .graph-node-ring"), std::string::npos);
    EXPECT_EQ(css.find(".graph-node:hover {"), std::string::npos)
        << "node-level hover outline paints under the header; the ring replaces it";

    UIRegistration::RegisterBuiltInControls();
    GraphPortedNode node;
    ASSERT_NE(node.GetChromeRing(), nullptr);
    EXPECT_EQ(node.GetChildren().back().get(), node.GetChromeRing())
        << "the ring is the last child so it paints above the header and fields";
}

TEST(GraphNodeLayoutTests, ApplyVisualStateSelectsWithoutBindModel)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-seq";
    model.TypeId = "Sequence";
    model.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false});
    EXPECT_FALSE(node.HasClass("selected"));

    node.ApplyVisualState({true, false, false, false, false});
    EXPECT_TRUE(node.HasClass("selected"));
    EXPECT_EQ(node.GetModelNodeId(), "n-seq");

    /* The accent ring is one CSS rule keyed on ring-accent, so the node has to
       carry that class for BOTH states the ring paints for — otherwise the
       stylesheet is correct and the ring still never lights up. */
    EXPECT_TRUE(node.HasClass("ring-accent")) << "selected paints the accent ring";
    node.ApplyVisualState({false, true, false, false, false});
    EXPECT_TRUE(node.HasClass("ring-accent")) << "search-match paints it too";
    node.ApplyVisualState({false, false, false, false, false});
    EXPECT_FALSE(node.HasClass("ring-accent")) << "neither state: no accent";
}

TEST(GraphNodeLayoutTests, NodeChromeRadiiComeFromCanvasPublishedVars)
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets" / "UI" / "theme" / "node-graph.css";
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "node-graph.css did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string css = ss.str();

    EXPECT_NE(css.find("border-radius: var(--graph-node-radius"), std::string::npos);
    EXPECT_NE(css.find("border-radius: var(--graph-value-radius"), std::string::npos);
    EXPECT_NE(css.find("border-radius: var(--graph-ring-radius"), std::string::npos);
    EXPECT_NE(css.find("border-width: var(--graph-ring-width"), std::string::npos);
    EXPECT_EQ(css.find("--graph-node-radius: "), std::string::npos)
        << "no per-tier radius definitions: the canvas publishes the radii "
           "continuously (pref x zoom) so the corner-radius setting stays live "
           "and corners do not step between tiers";

    const size_t ringRule = css.find(".graph-node-ring {");
    ASSERT_NE(ringRule, std::string::npos);
    const size_t ringEnd = css.find('}', ringRule);
    const std::string ringBody = css.substr(ringRule, ringEnd - ringRule);
    EXPECT_NE(ringBody.find("left: calc(-1px * var(--length-scale, 1))"), std::string::npos)
        << "ring insets are zoom-scaled: half the 2-unit line hangs outside";
}

TEST(GraphNodeLayoutTests, GetInputAxisCompactStringUsesSingleInlineClass)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-axis";
    model.TypeId = "GetInputAxis";
    model.Ports.push_back({"axis", Graph::PortDirection::In, "string", "Axis"});
    model.Ports.push_back({"value", Graph::PortDirection::Out, "float", "Value"});
    model.Parameters["axis"] = std::string("Bird.MoveX");

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* axis = node.FindPort("axis");
    ASSERT_NE(axis, nullptr);
    EXPECT_TRUE(axis->HasClass("has-inline-editor"));
    EXPECT_TRUE(axis->HasClass("graph-single-inline"));
    EXPECT_FALSE(axis->HasClass("bool-editor"));
    EXPECT_FALSE(axis->HasClass("vec-editor"));
    UIElement* editors = FindByClass(*axis, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);
    EXPECT_FALSE(IsDisplayNone(*editors));
    UIElement* field = FindByClass(*editors, "text-field");
    ASSERT_NE(field, nullptr);
}

TEST(GraphNodeLayoutTests, PlaySoundPortEditorsStayPortAligned)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-sound-multi";
    model.TypeId = "PlaySound";
    model.Ports.push_back({"clipGuid", Graph::PortDirection::In, "string", "Clip"});
    model.Ports.push_back({"volume", Graph::PortDirection::In, "float", "Volume"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* clip = node.FindPort("clipGuid");
    GraphPort* volume = node.FindPort("volume");
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(volume, nullptr);
    EXPECT_TRUE(clip->HasClass("has-inline-editor"));
    EXPECT_TRUE(volume->HasClass("has-inline-editor"));
    EXPECT_FALSE(clip->HasClass("graph-single-inline"));
    EXPECT_FALSE(volume->HasClass("graph-single-inline"));
    EXPECT_TRUE(clip->HasClass("shares-out-row"))
        << "ports sit on a shared top-anchored lattice, so the lone output lands on "
           "row 0 with Clip";
    EXPECT_FALSE(volume->HasClass("shares-out-row"))
        << "row 1 is a full 24 units below the output: clear";
}

TEST(GraphNodeLayoutTests, LongOutNameOnSharedRowShrinksValueHost)
{
    UIRegistration::RegisterBuiltInControls();

    GraphPortedNode node;
    Graph::Node model;
    model.Id = "n-find";
    model.TypeId = "FindEntityByName";
    model.Ports.push_back({"name", Graph::PortDirection::In, "string", "Name"});
    model.Ports.push_back({"entity", Graph::PortDirection::Out, "entity", "Entity"});

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    node.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    GraphPort* name = node.FindPort("name");
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(name->HasClass("has-inline-editor"));
    EXPECT_TRUE(name->HasClass("shares-out-row"))
        << "Entity is 6 letters on the same row as Name; the value box must yield";
    UIElement* nameHost = FindByClass(*name, "graph-inline-editors");
    ASSERT_NE(nameHost, nullptr);
    const std::optional<StyleLength> nameW = nameHost->Overrides().Get(Style::Width);
    ASSERT_TRUE(nameW.has_value() && nameW->IsPercent());
    const float nodeW = GraphNodeMetrics::GetNodeWidth(model);
    const float nameUnits = nodeW - GraphNodeMetrics::InlineEditorRightReserveGraph(6) -
        GraphNodeMetrics::NodeEditorStartGraph(model, true, false, false);
    EXPECT_FLOAT_EQ(nameW->Value, nameUnits / GraphNodeMetrics::kPortSizeGraph * 100.f);

    Graph::Node sound = model;
    sound.Id = "n-sound-out";
    sound.TypeId = "PlaySound";
    sound.Ports.clear();
    sound.Ports.push_back({"clipGuid", Graph::PortDirection::In, "string", "Clip"});
    sound.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    node.BindModel(sound, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);
    GraphPort* clip = node.FindPort("clipGuid");
    ASSERT_NE(clip, nullptr);
    EXPECT_TRUE(clip->HasClass("shares-out-row"))
        << "even a 3-letter Out shares the row; the host just yields less";
    UIElement* clipHost = FindByClass(*clip, "graph-inline-editors");
    ASSERT_NE(clipHost, nullptr);
    const std::optional<StyleLength> clipW = clipHost->Overrides().Get(Style::Width);
    ASSERT_TRUE(clipW.has_value() && clipW->IsPercent());
    const float clipNodeW = GraphNodeMetrics::GetNodeWidth(sound);
    const float clipUnits = clipNodeW - GraphNodeMetrics::InlineEditorRightReserveGraph(3) -
        GraphNodeMetrics::NodeEditorStartGraph(sound, true, false, false);
    EXPECT_FLOAT_EQ(clipW->Value, clipUnits / GraphNodeMetrics::kPortSizeGraph * 100.f);
    EXPECT_GT(GraphNodeMetrics::InlineEditorRightReserveGraph(7),
              GraphNodeMetrics::InlineEditorRightReserveGraph(3))
        << "'Pressed' reserves more of the row than 'Out'";
}

TEST(GraphNodeLayoutTests, EveryNodeSharesTheUniformWidth)
{
    Graph::Node follow;
    follow.Id = "n-follow";
    follow.TypeId = "ThirdPersonCameraFollow";
    follow.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    follow.Ports.push_back({"offset", Graph::PortDirection::In, "float3", "Offset"});
    follow.Ports.push_back({"lookOffset", Graph::PortDirection::In, "float3", "Look Offset"});
    follow.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});

    Graph::Node plain;
    plain.Id = "n-plain";
    plain.TypeId = "Branch";
    plain.Ports.push_back({"in", Graph::PortDirection::In, "flow", "In"});
    plain.Ports.push_back({"true", Graph::PortDirection::Out, "flow", "True"});

    /* Same width whatever the node or kind: a graph reads as a set only when
       the boxes match; rows clamp inside instead of widening. */
    const float w = GraphNodeMetrics::GetNodeWidth(follow);
    EXPECT_FLOAT_EQ(w, GraphNodeMetrics::SnapUpToGrid(GraphNodeMetrics::kNodeUniformWidthGraph));
    EXPECT_FLOAT_EQ(std::fmod(w, GraphNodeMetrics::kGridSizeGraph), 0.f) << "grid-snapped";
    EXPECT_FLOAT_EQ(GraphNodeMetrics::GetNodeWidth(plain), w);
}

TEST(GraphNodeLayoutTests, MaterialGraphNodesReserveNoSpaceForSuppressedEditors)
{
    UIRegistration::RegisterBuiltInControls();

    /* The material graph draws no inline port editors; the say lives on the
       node type (the factory instantiates MaterialGraphNode for the material
       kind), never on kind checks inside the generic code. */
    Graph::Node model;
    model.Id = "n-mat";
    model.TypeId = "Multiply";
    model.Ports.push_back({"offset", Graph::PortDirection::In, "float3", "Offset"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float3", "Out"});

    GraphPortedNode generic;
    MaterialGraphNode material;
    EXPECT_TRUE(generic.DrawsInlinePortEditors(model));
    EXPECT_FALSE(material.DrawsInlinePortEditors(model));

    /* Editor-less rows start no value column at all. */
    EXPECT_FLOAT_EQ(GraphNodeMetrics::NodeEditorStartGraph(model, false, false, false), 0.f);
    EXPECT_GT(GraphNodeMetrics::NodeEditorStartGraph(model, true, false, false), 0.f);

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };
    material.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);
    GraphPort* offset = material.FindPort("offset");
    ASSERT_NE(offset, nullptr);
    EXPECT_FALSE(offset->HasClass("has-inline-editor"));

    generic.BindModel(model, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);
    GraphPort* genericOffset = generic.FindPort("offset");
    ASSERT_NE(genericOffset, nullptr);
    EXPECT_TRUE(genericOffset->HasClass("has-inline-editor"));
}

TEST(GraphNodeLayoutTests, RebindInlineFloatDoesNotRecordUndo)
{
    UIRegistration::RegisterBuiltInControls();

    Graph::Model graph;
    Graph::Node sound;
    sound.Id = "n-sound-vol";
    sound.TypeId = "PlaySound";
    sound.Ports.push_back({"volume", Graph::PortDirection::In, "float", "Volume"});
    sound.Ports.push_back({"out", Graph::PortDirection::Out, "flow", "Out"});
    sound.Parameters["volume"] = 1.0f;
    graph.Nodes.push_back(std::move(sound));

    int undoCount = 0;
    int changeCount = 0;
    GraphNodeEditHost host;
    host.Model = &graph;
    host.Undo = [&](const std::string&, std::function<void()> mutate)
    {
        ++undoCount;
        mutate();
    };
    host.OnChanged = [&]() { ++changeCount; };
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    const Graph::Node* modelNode = graph.FindNode("n-sound-vol");
    ASSERT_NE(modelNode, nullptr);
    node.BindModel(*modelNode, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);
    node.BindModel(*modelNode, Graph::kKindIdGameLogic, GraphNodeVisualState{true, false, false, false, false}, &host);
    EXPECT_EQ(undoCount, 0);
    EXPECT_EQ(changeCount, 0);

    GraphPort* volume = node.FindPort("volume");
    ASSERT_NE(volume, nullptr);
    UIElement* editors = FindByClass(*volume, "graph-inline-editors");
    ASSERT_NE(editors, nullptr);
    UIElement* fieldEl = FindByClass(*editors, "float-field");
    ASSERT_NE(fieldEl, nullptr);
    auto* field = dynamic_cast<FloatField*>(fieldEl);
    ASSERT_NE(field, nullptr);
    field->NotifyValueChanged();
    EXPECT_EQ(undoCount, 1);
    EXPECT_EQ(changeCount, 1);
}


// ---- Expanded node view --------------------------------------------------
// Parameters with no inline port editor are invisible on a collapsed node.
// Expanded view gives each one a row; the node rect grows to fit them and the
// port lattice must NOT move, or wires between grid-aligned nodes bend.

namespace {

Graph::Node MakeExpandableNode()
{
    Graph::Node model;
    model.Id = "n-expand";
    model.TypeId = "Sample2D";
    model.Ports.push_back({"uv", Graph::PortDirection::In, "float2", "UV"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float4", "Out"});
    // "title" names the node, never a value row; the other two have no port.
    model.Parameters["title"] = Graph::GraphValue(std::string("Sampler"));
    model.Parameters["wrapMode"] = Graph::GraphValue(std::string("Repeat"));
    model.Parameters["mipBias"] = Graph::GraphValue(0.5f);
    return model;
}

bool RowIsHidden(const UIElement& el)
{
    const std::optional<DisplayMode> display = el.Overrides().Get(Style::Display);
    return display.has_value() && display.value() == DisplayMode::None;
}

int VisibleDetailRows(const UIElement& node)
{
    int visible = 0;
    for (const auto& child : node.GetChildren())
    {
        if (child && child->HasClass("graph-detail-row") && !RowIsHidden(*child))
            ++visible;
    }
    return visible;
}

} // namespace

TEST(GraphExpandedViewTests, CollapsedNodesHaveNoDetailRows)
{
    const Graph::Node model = MakeExpandableNode();
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/false), 0);
}

TEST(GraphExpandedViewTests, ExpandedRowsCountPinlessParamsAndSkipTheTitle)
{
    const Graph::Node model = MakeExpandableNode();
    // wrapMode + mipBias; "title" is excluded.
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/true), 2);
}

TEST(GraphExpandedViewTests, PortBackedParamsKeepTheirInlineEditorWhereTheKindDrawsOne)
{
    Graph::Node model = MakeExpandableNode();
    // "uv" has an input port, so a kind that draws inline port editors covers it
    // there and it earns no row of its own.
    model.Parameters["uv"] = Graph::GraphValue(std::string("0,0"));
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/true,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/true), 2);
    // A kind that draws none (the material graph) puts every value in Expand.
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/true), 3);
}

TEST(MathematicsRectTests, InteriorsOverlapAndTouchingEdgesDoNot)
{
    const Mathematics::Rect a{0.f, 0.f, 10.f, 10.f};
    const Mathematics::Rect interior{5.f, 5.f, 10.f, 10.f};
    const Mathematics::Rect touching{10.f, 0.f, 10.f, 10.f};
    EXPECT_TRUE(a.Overlaps(interior));
    EXPECT_FALSE(a.Overlaps(touching));
    EXPECT_TRUE(a.Inflated(1.f).Overlaps(touching));
    EXPECT_FLOAT_EQ(a.Right(), 10.f);
    EXPECT_FLOAT_EQ(a.Bottom(), 10.f);
}

TEST(GraphExpandedViewTests, ExpandingGrowsTheRectButNeverTheBaseHeight)
{
    const Graph::Node model = MakeExpandableNode();
    const float base = GraphNodeMetrics::GetNodeBaseHeight(model);
    const float collapsed = GraphNodeMetrics::GetNodeHeight(model, false, false, false);
    const float expanded = GraphNodeMetrics::GetNodeHeight(model, false, false, true);

    EXPECT_FLOAT_EQ(collapsed, base);
    EXPECT_GT(expanded, base);
}

TEST(GraphExpandedViewTests, ExpandingResolvesRectsThatCollideAtExpandedHeight)
{
    Graph::Model model;
    Graph::Node a = MakeExpandableNode();
    a.Id = "a";
    a.PositionX = 0.f;
    a.PositionY = 0.f;
    Graph::Node b = MakeExpandableNode();
    b.Id = "b";
    b.PositionX = 0.f;
    const float collapsedH = GraphNodeMetrics::GetNodeHeight(a, false, false, false);
    b.PositionY = collapsedH + GraphNodeMetrics::kGridSizeGraph;
    model.Nodes.push_back(a);
    model.Nodes.push_back(b);

    const auto width = [](const Graph::Node& n) { return GraphNodeMetrics::GetNodeWidth(n); };
    const auto collapsedHeight = [](const Graph::Node& n) {
        return GraphNodeMetrics::GetNodeHeight(n, false, false, false);
    };
    const auto expandedHeight = [](const Graph::Node& n) {
        return GraphNodeMetrics::GetNodeHeight(n, false, false, true);
    };

    EXPECT_FALSE(GraphNodeOverlap::WouldOverlapAny(
        model, model.Nodes[1],
        Mathematics::Vector2(model.Nodes[1].PositionX, model.Nodes[1].PositionY),
        std::unordered_set<std::string>{"b"}, width, collapsedHeight));
    EXPECT_TRUE(GraphNodeOverlap::WouldOverlapAny(
        model, model.Nodes[1],
        Mathematics::Vector2(model.Nodes[1].PositionX, model.Nodes[1].PositionY),
        std::unordered_set<std::string>{"b"}, width, expandedHeight));

    GraphNodeOverlap::ResolveAllNodeOverlaps(model, width, expandedHeight);

    EXPECT_FALSE(GraphNodeOverlap::WouldOverlapAny(
        model, model.Nodes[0],
        Mathematics::Vector2(model.Nodes[0].PositionX, model.Nodes[0].PositionY),
        std::unordered_set<std::string>{"a"}, width, expandedHeight));
    EXPECT_FALSE(GraphNodeOverlap::WouldOverlapAny(
        model, model.Nodes[1],
        Mathematics::Vector2(model.Nodes[1].PositionX, model.Nodes[1].PositionY),
        std::unordered_set<std::string>{"b"}, width, expandedHeight));
    const float expanded = GraphNodeMetrics::GetNodeHeight(model.Nodes[0], false, false, true);
    EXPECT_GE(std::abs(model.Nodes[1].PositionY - model.Nodes[0].PositionY),
              expanded + GraphNodeMetrics::kGridSizeGraph);
}

TEST(GraphExpandedViewTests, PortsKeepTheirAbsolutePositionWhenTheNodeExpands)
{
    const Graph::Node model = MakeExpandableNode();
    const float collapsedH = GraphNodeMetrics::GetNodeHeight(model, false, false, false);
    const float expandedH = GraphNodeMetrics::GetNodeHeight(model, false, false, true);
    ASSERT_GT(expandedH, collapsedH);

    // PortTopPercent is a share of whatever rect it is given, so the same port
    // resolves to the same absolute unit offset in both views.
    const float collapsedUnits =
        GraphNodeMetrics::PortTopPercent(0, 1, collapsedH) * collapsedH / 100.f;
    const float expandedUnits =
        GraphNodeMetrics::PortTopPercent(0, 1, expandedH) * expandedH / 100.f;
    EXPECT_NEAR(collapsedUnits, expandedUnits, 0.001f);
}

TEST(GraphExpandedViewTests, DetailRowsStackBelowTheLastPortRow)
{
    const Graph::Node model = MakeExpandableNode();
    const float row0 = GraphNodeMetrics::ExpandedRowCenterGraph(model, 0);
    const float row1 = GraphNodeMetrics::ExpandedRowCenterGraph(model, 1);

    const int portRows = GraphNodeMetrics::PortRowTotal(model);
    const float lastPortY = GraphNodeMetrics::kNodeTitleRowHeightGraph +
        GraphNodeMetrics::kNodePortAreaTopPaddingGraph +
        GraphNodeMetrics::kPortSpacing * static_cast<float>(portRows > 1 ? portRows - 1 : 0);

    EXPECT_GT(row0, lastPortY);
    EXPECT_FLOAT_EQ(row1 - row0, GraphNodeMetrics::kPortSpacing);
    // Every row has to fit inside the rect it grew.
    EXPECT_LT(row1, GraphNodeMetrics::GetNodeHeight(model, false, false, true));
}

TEST(GraphExpandedViewTests, ForEachDetailParamVisitsExactlyTheCountedParams)
{
    const Graph::Node model = MakeExpandableNode();
    std::vector<std::string> visited;
    GraphNodeMetrics::ForEachDetailParam(
        model, /*drawsInlineEditors=*/false, /*hasCentralEditors=*/false, /*expandedView=*/true,
        [&](const std::string& key, const Graph::GraphValue&) { visited.push_back(key); });

    EXPECT_EQ(static_cast<int>(visited.size()),
              GraphNodeMetrics::DetailRowCount(model, false, false, true));
    EXPECT_EQ(std::count(visited.begin(), visited.end(), std::string("title")), 0);

    // Collapsed: the walk yields nothing, matching a row count of zero.
    visited.clear();
    GraphNodeMetrics::ForEachDetailParam(
        model, false, false, false,
        [&](const std::string& key, const Graph::GraphValue&) { visited.push_back(key); });
    EXPECT_TRUE(visited.empty());
}

TEST(GraphExpandedViewTests, ExpandedNodeBindsAValueRowPerPinlessParam)
{
    MaterialGraphNode node;
    const Graph::Node model = MakeExpandableNode();

    GraphNodeEditHost host;
    host.ExpandedView = true;
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &host);

    EXPECT_EQ(VisibleDetailRows(node), GraphNodeMetrics::DetailRowCount(model, false, false, true));
}

TEST(GraphExpandedViewTests, EnumOptionsRoundTripThroughADetailRowDropdown)
{
    UIRegistration::RegisterBuiltInControls();

    Graph::Model graph;
    Graph::Node compare;
    compare.Id = "n-cmp";
    compare.TypeId = "CompareFloat";
    compare.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    compare.Ports.push_back({"b", Graph::PortDirection::In, "float", "B"});
    compare.Ports.push_back({"true", Graph::PortDirection::Out, "flow", "True"});
    compare.Parameters["a"] = 0.0f;
    compare.Parameters["b"] = 0.0f;
    compare.Parameters["comparison"] = Graph::GraphValue(std::string("greater"));
    graph.Nodes.push_back(std::move(compare));

    int undoCount = 0;
    GraphNodeEditHost host;
    host.Model = &graph;
    host.ExpandedView = true;
    host.Undo = [&](const std::string&, std::function<void()> mutate)
    {
        ++undoCount;
        mutate();
    };
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    const Graph::Node* modelNode = graph.FindNode("n-cmp");
    ASSERT_NE(modelNode, nullptr);
    node.BindModel(*modelNode, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    // The port-less enum param gets a detail row hosting a dropdown built from
    // the registry's typed options.
    UIElement* dropdownEl = FindByClass(node, "graph-enum-dropdown");
    ASSERT_NE(dropdownEl, nullptr);
    auto* dropdown = dynamic_cast<Dropdown*>(dropdownEl);
    ASSERT_NE(dropdown, nullptr);
    EXPECT_EQ(dropdown->GetSelectedValue(), "greater");
    EXPECT_EQ(dropdown->GetSelectedLabel(), "Greater");

    // Committing stores the option VALUE (wire format), never the label.
    dropdown->SetSelectedValue("less_or_equal");
    EXPECT_EQ(undoCount, 1);
    EXPECT_EQ(graph.FindNode("n-cmp")->Parameters.GetString("comparison", ""),
              "less_or_equal");
}

TEST(GraphExpandedViewTests, CentralEditorRelocationFollowsTheNodeTypesPredicate)
{
    // Multi-row Compare: central-editor params relocate to detail rows even
    // collapsed — but only when the node type says it has central editors.
    Graph::Node model;
    model.Id = "n-compare";
    model.TypeId = "Compare";
    model.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    model.Ports.push_back({"b", Graph::PortDirection::In, "float", "B"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});
    model.Parameters["function"] = Graph::GraphValue(std::string("SG_COMPARE_GREATER"));
    ASSERT_GT(GraphNodeMetrics::GetNodeBaseHeight(model), GraphNodeMetrics::kNodeHeight);

    const GraphPortedNode prototype;
    EXPECT_TRUE(prototype.HasCentralEditors(model));
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               prototype.HasCentralEditors(model),
                                               /*expandedView=*/false), 1);
    EXPECT_EQ(GraphNodeMetrics::DetailRowCount(model, /*drawsInlineEditors=*/false,
                                               /*hasCentralEditors=*/false,
                                               /*expandedView=*/false), 0);
}

TEST(GraphExpandedViewTests, CollapsingHidesRowsTheNodeNoLongerNeeds)
{
    MaterialGraphNode node;
    const Graph::Node model = MakeExpandableNode();

    GraphNodeEditHost expanded;
    expanded.ExpandedView = true;
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &expanded);
    ASSERT_GT(VisibleDetailRows(node), 0);

    GraphNodeEditHost collapsed;
    collapsed.ExpandedView = false;
    node.BindModel(model, Graph::kKindIdMaterial, GraphNodeVisualState{false, false, false, false, false}, &collapsed);
    EXPECT_EQ(VisibleDetailRows(node), 0);
}

TEST(GraphExpandedViewTests, DetailRowDropdownKeepsAnUnlistedStoredValue)
{
    UIRegistration::RegisterBuiltInControls();

    Graph::Model graph;
    Graph::Node compare;
    compare.Id = "n-cmp-unlisted";
    compare.TypeId = "CompareFloat";
    compare.Ports.push_back({"a", Graph::PortDirection::In, "float", "A"});
    compare.Ports.push_back({"b", Graph::PortDirection::In, "float", "B"});
    compare.Ports.push_back({"true", Graph::PortDirection::Out, "flow", "True"});
    compare.Parameters["a"] = 0.0f;
    compare.Parameters["b"] = 0.0f;
    // A value the schema does not list — a graph from a newer build, or one
    // hand-edited. Showing option 0 would silently retype the node.
    compare.Parameters["comparison"] = Graph::GraphValue(std::string("sideways"));
    graph.Nodes.push_back(std::move(compare));

    int undoCount = 0;
    GraphNodeEditHost host;
    host.Model = &graph;
    host.ExpandedView = true;
    host.Undo = [&](const std::string&, std::function<void()> mutate)
    {
        ++undoCount;
        mutate();
    };
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    const Graph::Node* modelNode = graph.FindNode("n-cmp-unlisted");
    ASSERT_NE(modelNode, nullptr);
    node.BindModel(*modelNode, Graph::kKindIdGameLogic, GraphNodeVisualState{false, false, false, false, false}, &host);

    auto* dropdown = dynamic_cast<Dropdown*>(FindByClass(node, "graph-enum-dropdown"));
    ASSERT_NE(dropdown, nullptr);
    EXPECT_EQ(dropdown->GetSelectedValue(), "sideways");
    EXPECT_EQ(dropdown->GetSelectedLabel(), "sideways");

    // Binding alone commits nothing, and the stored value is untouched.
    EXPECT_EQ(undoCount, 0);
    EXPECT_EQ(graph.FindNode("n-cmp-unlisted")->Parameters.GetString("comparison", ""),
              "sideways");

    // The schema's own options are still all offered alongside it.
    dropdown->SetSelectedValue("less");
    EXPECT_EQ(undoCount, 1);
    EXPECT_EQ(graph.FindNode("n-cmp-unlisted")->Parameters.GetString("comparison", ""), "less");
}

// ---- Schema ranges on vector edit surfaces --------------------------------
// GraphNodeRegistry.h documents one invariant for every float edit surface: a
// value can never commit out of range. A vec3 surface is three float boxes, so
// it has to carry the range the same way the vec2/vec4 fan-out does.

namespace {

/* A kind id nothing else uses, so these tests neither see nor disturb the real
   node catalogs. */
constexpr const char* kRangeTestKindId = "range-test-kind";

const FloatField* Vec3Component(UIElement& root, const char* componentClass)
{
    UIElement* el = FindByClass(root, componentClass);
    return el ? dynamic_cast<const FloatField*>(el) : nullptr;
}

void ExpectVec3ComponentsClampTo(UIElement& root, float min, float max)
{
    for (const char* componentClass :
         {"vector3-component-x", "vector3-component-y", "vector3-component-z"})
    {
        const FloatField* field = Vec3Component(root, componentClass);
        ASSERT_NE(field, nullptr) << componentClass;
        EXPECT_TRUE(field->HasValueRange()) << componentClass;
        EXPECT_FLOAT_EQ(field->ClampToValueRange(max + 10.f), max) << componentClass;
        EXPECT_FLOAT_EQ(field->ClampToValueRange(min - 10.f), min) << componentClass;
    }
}

} // namespace

TEST(GraphValueRangeTests, RangedVec3ParamRowClampsEveryComponent)
{
    UIRegistration::RegisterBuiltInControls();

    NodeTypeMeta meta;
    meta.TypeId = "RangedVec3Param";
    meta.DisplayName = "Ranged Vec3 Param";
    meta.Parameters = {{"tint", Graph::GraphValue(std::string("vec3(0, 0, 0)")), {}, 0.f, 1.f}};
    GraphNodeRegistry::Get().Register(kRangeTestKindId, meta);

    Graph::Node model;
    model.Id = "n-vec3-param";
    model.TypeId = "RangedVec3Param";
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float3", "Out"});
    model.Parameters["tint"] = Graph::GraphValue(std::string("vec3(2, 2, 2)"));

    GraphNodeEditHost host;
    host.ExpandedView = true;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    node.BindModel(model, kRangeTestKindId, GraphNodeVisualState{false, false, false, false, false}, &host);

    ASSERT_NE(FindByClass(node, "vector3-field"), nullptr);
    ExpectVec3ComponentsClampTo(node, 0.f, 1.f);
}

TEST(GraphValueRangeTests, RangedVec3PortEditorClampsEveryComponent)
{
    UIRegistration::RegisterBuiltInControls();

    NodeTypeMeta meta;
    meta.TypeId = "RangedVec3Port";
    meta.DisplayName = "Ranged Vec3 Port";
    meta.Ports = {{"dir", Graph::PortDirection::In, "float3", "Dir", -1.f, 1.f},
                 {"out", Graph::PortDirection::Out, "float", "Out"}};
    GraphNodeRegistry::Get().Register(kRangeTestKindId, meta);

    Graph::Node model;
    model.Id = "n-vec3-port";
    model.TypeId = "RangedVec3Port";
    model.Ports.push_back({"dir", Graph::PortDirection::In, "float3", "Dir"});
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float", "Out"});
    model.Parameters["dir"] = Graph::GraphValue(std::string("vec3(5, 5, 5)"));

    GraphNodeEditHost host;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    node.BindModel(model, kRangeTestKindId, GraphNodeVisualState{false, false, false, false, false}, &host);

    ASSERT_NE(FindByClass(node, "vector3-field"), nullptr);
    ExpectVec3ComponentsClampTo(node, -1.f, 1.f);
}

TEST(GraphValueRangeTests, PooledVec3SurfaceDropsThePreviousBindingsRange)
{
    UIRegistration::RegisterBuiltInControls();

    NodeTypeMeta ranged;
    ranged.TypeId = "RangedVec3Rebind";
    ranged.Parameters = {{"tint", Graph::GraphValue(std::string("vec3(0, 0, 0)")), {}, 0.f, 1.f}};
    GraphNodeRegistry::Get().Register(kRangeTestKindId, ranged);

    NodeTypeMeta unranged;
    unranged.TypeId = "UnrangedVec3Rebind";
    unranged.Parameters = {{"tint", Graph::GraphValue(std::string("vec3(0, 0, 0)"))}};
    GraphNodeRegistry::Get().Register(kRangeTestKindId, unranged);

    Graph::Node model;
    model.Id = "n-rebind";
    model.TypeId = "RangedVec3Rebind";
    model.Ports.push_back({"out", Graph::PortDirection::Out, "float3", "Out"});
    model.Parameters["tint"] = Graph::GraphValue(std::string("vec3(0.5, 0.5, 0.5)"));

    GraphNodeEditHost host;
    host.ExpandedView = true;
    host.IsInputConnected = [](const std::string&, const std::string&) { return false; };

    GraphPortedNode node;
    node.BindModel(model, kRangeTestKindId, GraphNodeVisualState{false, false, false, false, false}, &host);
    ExpectVec3ComponentsClampTo(node, 0.f, 1.f);

    // Same pooled element, rebound to a type whose vec3 carries no range.
    model.TypeId = "UnrangedVec3Rebind";
    node.BindModel(model, kRangeTestKindId, GraphNodeVisualState{false, false, false, false, false}, &host);
    const FloatField* x = Vec3Component(node, "vector3-component-x");
    ASSERT_NE(x, nullptr);
    EXPECT_FALSE(x->HasValueRange());
}
