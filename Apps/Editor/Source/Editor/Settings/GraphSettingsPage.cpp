#include "Editor/Settings/GraphSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Graph/GraphCanvas.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/NodeColorSettings.h"
#include "Panels/GraphPanel.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kPanelScrollbarsPrefKey = "nodeGraph.panelScrollbars";
constexpr const char* kVariablesScrollbarsPrefKey = "nodeGraph.variablesScrollbars";

struct NodeDropShadowState
{
    float OffsetX = GraphCanvas::kDefaultNodeDropShadowOffsetX;
    float OffsetY = GraphCanvas::kDefaultNodeDropShadowOffsetY;
    float Blur = GraphCanvas::kDefaultNodeDropShadowBlur;
    float Opacity = GraphCanvas::kDefaultNodeDropShadowOpacity;
};

NodeDropShadowState LoadDropShadowState()
{
    NodeDropShadowState state;
    auto prefs = OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    auto loadFloat = [&prefs](const char* key, float& out)
    {
        double stored = out;
        if (prefs.TryGetDouble(key, stored))
            out = static_cast<float>(stored);
    };
    loadFloat(GraphCanvas::kNodeDropShadowOffsetXPreference, state.OffsetX);
    loadFloat(GraphCanvas::kNodeDropShadowOffsetYPreference, state.OffsetY);
    loadFloat(GraphCanvas::kNodeDropShadowBlurPreference, state.Blur);
    loadFloat(GraphCanvas::kNodeDropShadowOpacityPreference, state.Opacity);
    state.Blur = std::max(0.0f, state.Blur);
    state.Opacity = std::clamp(state.Opacity, 0.0f, 1.0f);
    return state;
}

void ApplyDropShadow(const NodeDropShadowState& state)
{
    GraphCanvas::SetNodeDropShadowStyle(state.OffsetX, state.OffsetY, state.Blur, state.Opacity);
}

void AppendDropShadowSlider(SettingsCategoryDescriptor& category,
                            const std::shared_ptr<NodeDropShadowState>& state,
                            const char* prefKey, const char* label, const char* searchKeywords,
                            float defaultValue, float minValue, float maxValue, float step,
                            float NodeDropShadowState::*member)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.SearchKeywords = searchKeywords;
    field.PrefKey = prefKey;
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = defaultValue;
    slider.MinValue = minValue;
    slider.MaxValue = maxValue;
    slider.Step = step;
    slider.Get = [state, member]() { return (*state).*member; };
    slider.Set = [state, member](float value)
    {
        if ((*state).*member == value)
            return;
        (*state).*member = value;
        ApplyDropShadow(*state);
    };
    field.Control = std::move(slider);
    category.Fields.push_back(std::move(field));
}

void AppendTypeColorRows(SettingsCategoryDescriptor& category, std::string_view kindId,
                         const char* sectionTitle)
{
    const std::string kindIdOwned(kindId);
    std::vector<NodeTypeMeta> types = GraphNodeRegistry::Get().GetAllTypes(kindIdOwned);
    std::sort(types.begin(), types.end(), [](const NodeTypeMeta& a, const NodeTypeMeta& b)
    {
        if (a.Category != b.Category)
            return a.Category < b.Category;
        return a.DisplayName < b.DisplayName;
    });

    std::string currentCategory;
    bool first = true;
    for (const NodeTypeMeta& type : types)
    {
        SettingsFieldDescriptor field;
        if (first)
        {
            field.SectionHeader = sectionTitle;
            field.SectionDescription = type.Category.empty() ? "Uncategorized" : type.Category;
            first = false;
        }
        else if (type.Category != currentCategory)
        {
            field.SectionHeader = type.Category.empty() ? "Uncategorized" : type.Category;
        }
        currentCategory = type.Category;

        field.Label = type.DisplayName.empty() ? type.TypeId : type.DisplayName;
        field.SearchKeywords = std::string("nodegraph-typecolor:") + kindIdOwned + ":" + type.TypeId;
        SettingsFieldDescriptor::ColorField color;
        color.DefaultArgb = NodeColorSettings::DefaultNodeColorArgb(kindIdOwned, type.TypeId);
        const std::string typeId = type.TypeId;
        color.Get = [kindIdOwned, typeId]() { return NodeColorSettings::GetNodeColorArgb(kindIdOwned, typeId); };
        color.Set = [kindIdOwned, typeId](uint32_t argb)
        {
            const uint32_t defaultArgb = NodeColorSettings::DefaultNodeColorArgb(kindIdOwned, typeId);
            if (argb == defaultArgb)
            {
                if (NodeColorSettings::GetNodeColorArgb(kindIdOwned, typeId) == defaultArgb)
                    return;
                NodeColorSettings::ResetNodeColor(kindIdOwned, typeId);
            }
            else
            {
                if (NodeColorSettings::GetNodeColorArgb(kindIdOwned, typeId) == argb)
                    return;
                NodeColorSettings::SetNodeColorArgb(kindIdOwned, typeId, argb);
            }
            GraphCanvas::MarkLiveCanvasesDirty();
        };
        field.Control = std::move(color);
        category.Fields.push_back(std::move(field));
    }
}
} // namespace

void RegisterGraphSettingsCategory()
{
    SettingsCategoryDescriptor page;
    page.CategoryId = "nodeGraph";
    page.Title = "Node Graph";
    page.Group = SettingsCategoryGroup::UI;
    page.TreeRowClass = "ui-node-graph-row";
    page.SearchKeywords = "node graph visual scripting material fsm palette";
    page.Description = "Customize node graph controls and node type colors.";

    {
        SettingsFieldDescriptor field;
        field.Label = "Palette Panel Scrollbar";
        field.SearchKeywords = "node graph palette panel scrollbar";
        field.PrefKey = kPanelScrollbarsPrefKey;
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = false;
        toggle.Get = []() { return GraphPanel::GetPanelScrollbarsPreference(); };
        toggle.Set = [](bool value)
        {
            if (GraphPanel::GetPanelScrollbarsPreference() == value)
                return;
            GraphPanel::SetPanelScrollbarsPreference(value);
        };
        field.Control = std::move(toggle);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Variables Panel Scrollbar";
        field.SearchKeywords = "node graph variables panel scrollbar";
        field.PrefKey = kVariablesScrollbarsPrefKey;
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = true;
        toggle.Get = []() { return GraphPanel::GetVariablesScrollbarsPreference(); };
        toggle.Set = [](bool value)
        {
            if (GraphPanel::GetVariablesScrollbarsPreference() == value)
                return;
            GraphPanel::SetVariablesScrollbarsPreference(value);
        };
        field.Control = std::move(toggle);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Node Header Alignment";
        field.SearchKeywords = "node graph header title alignment left center right";
        field.PrefKey = GraphCanvas::kNodeHeaderAlignmentPreference;
        SettingsFieldDescriptor::DropdownField dropdown;
        dropdown.OptionsProvider = []
        {
            return std::vector<SettingsFieldDescriptor::DropdownField::Option>{
                {"left", "Left"}, {"center", "Center"}, {"right", "Right"}};
        };
        dropdown.DefaultValue = "left";
        dropdown.Get = []() { return GraphCanvas::GetNodeHeaderAlignment(); };
        dropdown.Set = [](const std::string& value)
        {
            if (GraphCanvas::GetNodeHeaderAlignment() == value)
                return;
            GraphCanvas::SetNodeHeaderAlignment(value);
        };
        field.Control = std::move(dropdown);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Node Corner Radius";
        field.SearchKeywords = "node graph corner radius rounded nodes";
        field.PrefKey = GraphCanvas::kNodeCornerRadiusPreference;
        SettingsFieldDescriptor::SliderField slider;
        slider.DefaultValue = GraphCanvas::kDefaultNodeCornerRadius;
        slider.MinValue = GraphCanvas::kMinNodeCornerRadius;
        slider.MaxValue = GraphCanvas::kMaxNodeCornerRadius;
        slider.Step = 0.5f;
        slider.Get = []() { return GraphCanvas::GetNodeCornerRadius(); };
        slider.Set = [](float value)
        {
            if (GraphCanvas::GetNodeCornerRadius() == value)
                return;
            GraphCanvas::SetNodeCornerRadius(value);
        };
        field.Control = std::move(slider);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Node Drop Shadows";
        field.SearchKeywords = "node graph drop shadow shadows depth";
        field.PrefKey = GraphCanvas::kNodeDropShadowsPreference;
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = true;
        toggle.Get = []() { return GraphCanvas::GetNodeDropShadows(); };
        toggle.Set = [](bool value)
        {
            if (GraphCanvas::GetNodeDropShadows() == value)
                return;
            GraphCanvas::SetNodeDropShadows(value);
        };
        field.Control = std::move(toggle);
        page.Fields.push_back(std::move(field));
    }

    auto shadow = std::make_shared<NodeDropShadowState>(LoadDropShadowState());
    AppendDropShadowSlider(page, shadow, GraphCanvas::kNodeDropShadowOffsetXPreference,
                           "Node Shadow Offset X", "node graph drop shadow offset",
                           GraphCanvas::kDefaultNodeDropShadowOffsetX, -32.0f, 32.0f, 1.0f,
                           &NodeDropShadowState::OffsetX);
    AppendDropShadowSlider(page, shadow, GraphCanvas::kNodeDropShadowOffsetYPreference,
                           "Node Shadow Offset Y", "node graph drop shadow offset",
                           GraphCanvas::kDefaultNodeDropShadowOffsetY, -32.0f, 32.0f, 1.0f,
                           &NodeDropShadowState::OffsetY);
    AppendDropShadowSlider(page, shadow, GraphCanvas::kNodeDropShadowBlurPreference,
                           "Node Shadow Blur", "node graph drop shadow blur",
                           GraphCanvas::kDefaultNodeDropShadowBlur, 0.0f, 64.0f, 1.0f,
                           &NodeDropShadowState::Blur);
    AppendDropShadowSlider(page, shadow, GraphCanvas::kNodeDropShadowOpacityPreference,
                           "Node Shadow Opacity", "node graph drop shadow opacity look feel",
                           GraphCanvas::kDefaultNodeDropShadowOpacity, 0.0f, 1.0f, 0.05f,
                           &NodeDropShadowState::Opacity);

    {
        SettingsFieldDescriptor field;
        field.Label = "Rounded Connection Corners";
        field.SearchKeywords = "node graph connection rounded corners wires";
        field.PrefKey = GraphCanvas::kConnectionRoundedCornersPreference;
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = true;
        toggle.Get = []() { return GraphCanvas::GetConnectionRoundedCorners(); };
        toggle.Set = [](bool value)
        {
            if (GraphCanvas::GetConnectionRoundedCorners() == value)
                return;
            GraphCanvas::SetConnectionRoundedCorners(value);
        };
        field.Control = std::move(toggle);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Selected Node Wires";
        field.SearchKeywords = "node graph selected node wires bright glow halo highlight connections";
        field.PrefKey = GraphCanvas::kSelectedNodeWireEmphasisPreference;
        SettingsFieldDescriptor::DropdownField dropdown;
        dropdown.OptionsProvider = []
        {
            return std::vector<SettingsFieldDescriptor::DropdownField::Option>{
                {"off", "Off"}, {"bright", "Bright"}, {"glow", "Glow"}, {"halo", "Halo Only"}};
        };
        dropdown.DefaultValue = "glow";
        dropdown.Get = []() { return GraphCanvas::GetSelectedNodeWireEmphasis(); };
        dropdown.Set = [](const std::string& value)
        {
            if (GraphCanvas::GetSelectedNodeWireEmphasis() == value)
                return;
            GraphCanvas::SetSelectedNodeWireEmphasis(value);
        };
        field.Control = std::move(dropdown);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.SectionHeader = "Node Colors";
        field.Label = "Node Body Color";
        field.SearchKeywords = "node graph body fill color";
        SettingsFieldDescriptor::ColorField color;
        color.DefaultArgb = NodeColorSettings::DefaultNodeBodyColorArgb();
        color.Get = []() { return NodeColorSettings::GetNodeBodyColorArgb(); };
        color.Set = [](uint32_t argb)
        {
            const uint32_t defaultArgb = NodeColorSettings::DefaultNodeBodyColorArgb();
            if (argb == defaultArgb)
            {
                if (NodeColorSettings::GetNodeBodyColorArgb() == defaultArgb)
                    return;
                NodeColorSettings::ResetNodeBodyColor();
            }
            else
            {
                if (NodeColorSettings::GetNodeBodyColorArgb() == argb)
                    return;
                NodeColorSettings::SetNodeBodyColorArgb(argb);
            }
            GraphCanvas::MarkLiveCanvasesDirty();
        };
        field.Control = std::move(color);
        page.Fields.push_back(std::move(field));
    }

    {
        SettingsFieldDescriptor field;
        field.Label = "Graph Background Color";
        field.SearchKeywords = "node graph canvas background color";
        SettingsFieldDescriptor::ColorField color;
        color.DefaultArgb = NodeColorSettings::DefaultCanvasColorArgb();
        color.Get = []() { return NodeColorSettings::GetCanvasColorArgb(); };
        color.Set = [](uint32_t argb)
        {
            const uint32_t defaultArgb = NodeColorSettings::DefaultCanvasColorArgb();
            if (argb == defaultArgb)
            {
                if (NodeColorSettings::GetCanvasColorArgb() == defaultArgb)
                    return;
                NodeColorSettings::ResetCanvasColor();
            }
            else
            {
                if (NodeColorSettings::GetCanvasColorArgb() == argb)
                    return;
                NodeColorSettings::SetCanvasColorArgb(argb);
            }
            GraphCanvas::MarkLiveCanvasesDirty();
        };
        field.Control = std::move(color);
        page.Fields.push_back(std::move(field));
    }

    const std::size_t staticFieldCount = page.Fields.size();
    page.PrepareFields = [staticFieldCount](SettingsCategoryDescriptor& live)
    {
        if (live.Fields.size() > staticFieldCount)
            live.Fields.resize(staticFieldCount);
        GraphNodeRegistry::Get().EnsureMaterialShaderGraphTypesRegistered();
        for (const Graph::GraphTypeDesc& desc : Graph::GraphTypeRegistry::Get().All())
        {
            const std::string sectionTitle = desc.DisplayName + " Nodes";
            AppendTypeColorRows(live, desc.Id, sectionTitle.c_str());
        }
    };
    page.PrepareFields(page);

    EditorSettingsRegistry::Get().RegisterCategory(std::move(page));
}

} // namespace GameEngine::Editor
