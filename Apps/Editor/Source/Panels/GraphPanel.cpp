#include "Panels/GraphPanel.h"
#include "Platform/SystemMetrics.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "EditorContext.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphKindChrome.h"
#include "Graph/GraphNodeIcons.h"
#include "Graph/GraphSubgraphStore.h"
#include "Graph/GraphNest.h"
#include "Graph/GraphTransitionStore.h"
#include "Graph/GraphAsset.h"
#include "Graph/GraphGlslAuthoring.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgTagParser.h"
#include "Graph/NodeColorSettings.h"
#include "Input/InputSystem.h"
#include "Mathematics/Vector2.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Editor/Settings/SettingsStore.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/Controls/Button.h"
#include "UI/EditorIcons.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include <string_view>
#include "EditorContextMenu/UIContextMenu.h"
#include "UI/PanelSearchBar.h"
#include "UI/EditorSearchBars.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/StyleProperties.h"
#include "Panels/SettingsPanel.h"
#include "UI/UIEvents.h"
#include "Scheduler/Scheduler.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace GameEngine {

namespace {

constexpr float kNodePaletteCollapsedHeight = 48.f;
constexpr float kNodePaletteWidth = 220.f;
constexpr float kNodePaletteMinHeight = 120.f;
constexpr float kVariablesPanelCollapsedHeight = 48.f;
/* Wide enough for a whole variable row: name 92 + value 96 + type 82 + spacer 8
   + two 72px toggle groups, five 6px gaps, and the row's and panel's padding. At
   420 the Global toggle fell off the right edge. Mirrored by .node-graph-variables
   in node-graph.css, which this override wins over. */
constexpr float kVariablesPanelWidth = 480.f;
constexpr float kVariablesPanelMinHeight = 120.f;
constexpr float kVariableRowHeight = 44.f;
constexpr float kFloatingPanelMargin = 16.f;
constexpr float kFloatingPanelMaxBodyRatio = 0.95f;
constexpr const char* kNodeGraphColorPaletteNodesPreference = "nodeGraph.colorPaletteNodes";
constexpr const char* kNodeGraphPanelScrollbarsPreference = "nodeGraph.panelScrollbars";
constexpr const char* kNodeGraphVariablesScrollbarsPreference = "nodeGraph.variablesScrollbars";

constexpr const char* kCategoryState = "State";
constexpr size_t kMaxNestDepth = 3;

bool HostTypeMatchesNestKind(std::string_view typeId, GraphNestKind kind)
{
    switch (kind)
    {
    case GraphNestKind::StateMachine:
        return typeId == "StateMachine";
    case GraphNestKind::PoseGraph:
        return typeId == "State";
    case GraphNestKind::BlendSpace1D:
        return typeId == "BlendSpace1D";
    case GraphNestKind::BlendSpace2D:
        return typeId == "BlendSpace2D";
    default:
        return false;
    }
}

template <typename FrameVec, typename ModelT>
ModelT* NestParentModel(FrameVec& nest, ModelT& root, size_t index)
{
    for (int j = static_cast<int>(index) - 1; j >= 0; --j)
    {
        if (NestKindUsesSubgraphModel(nest[static_cast<size_t>(j)].Kind))
            return &nest[static_cast<size_t>(j)].Model;
    }
    return &root;
}

namespace
{

/* Menus fall back to the generic node glyph for unmapped types, matching the
   element holders' CSS base image. */
std::string MenuIconPathForStem(const std::string& stem)
{
    return stem.empty() ? std::string(EditorIcons::kNode)
                        : GraphNodeIcons::IconPathForStem(stem);
}

} // namespace


bool g_PanelScrollbarsPreferenceInitialized = false;
bool g_PanelScrollbarsPreference = false;
std::unordered_set<GraphPanel*> g_LiveGraphPanels;
GraphPanel* g_LastOpenedGraphPanel = nullptr;
GraphPanel* g_InspectorGraphPanel = nullptr;
bool g_VariablesScrollbarsPreferenceInitialized = false;
bool g_VariablesScrollbarsPreference = true;

class NodeGraphOverflowIndicator final : public UIElement
{
public:
    enum class Edge
    {
        Top,
        Bottom
    };

    explicit NodeGraphOverflowIndicator(Edge edge)
        : m_Edge(edge)
    {
        AddClass("node-graph-scroll-more-indicator");
        AddClass(edge == Edge::Top ? "node-graph-scroll-more-indicator-top"
                                   : "node-graph-scroll-more-indicator-bottom");
        AddClass("hidden");
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override
    {
        // Parallel-drain thread contract: custom emission may touch shared
        // text/measure state, so it never runs on a JobSystem worker — escalate
        // and let the drain re-emit this element on the UI thread.
        if (ctx.OffThread)
        {
            if (ctx.EscalateFlag)
                *ctx.EscalateFlag = true;
            return;
        }

        (void)style;
        if (w <= 1.0f || h <= 1.0f)
            return;

        const bool isTop = m_Edge == Edge::Top;
        const uint32_t solid = UI::PackColor(0.11f, 0.11f, 0.115f, 0.86f);
        const uint32_t clear = UI::PackColor(0.11f, 0.11f, 0.115f, 0.0f);
        auto fade = UI::MakeRect(x, y, w, h, isTop ? solid : clear);
        UI::AddGradient(fade, UI::GradientMode::Vertical, isTop ? solid : clear, isTop ? clear : solid);
        ctx.Emit(fade);

        const float lineW = std::min(42.0f, std::max(12.0f, w * 0.32f));
        const float lineH = 1.0f;
        auto line = UI::MakeRect(x + (w - lineW) * 0.5f,
                                 isTop ? y + 4.0f : y + h - 5.0f,
                                 lineW,
                                 lineH,
                                 UI::PackColor(1.0f, 1.0f, 1.0f, 0.24f),
                                 0.5f, 0.5f, 0.5f, 0.5f);
        ctx.Emit(line);
    }

private:
    Edge m_Edge = Edge::Bottom;
};

bool LoadColorPaletteNodesPreference()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool enabled = false;
    (void)prefs.TryGetBool(kNodeGraphColorPaletteNodesPreference, enabled);
    return enabled;
}

bool LoadPanelScrollbarsPreference()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool enabled = false;
    (void)prefs.TryGetBool(kNodeGraphPanelScrollbarsPreference, enabled);
    return enabled;
}

bool LoadVariablesScrollbarsPreference()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool enabled = true;
    (void)prefs.TryGetBool(kNodeGraphVariablesScrollbarsPreference, enabled);
    return enabled;
}

void SaveColorPaletteNodesPreference(bool enabled)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(kNodeGraphColorPaletteNodesPreference, enabled);
    (void)prefs.Save(&err);
}

bool AreNodeGraphPortTypesCompatible(const std::string& sourceType, const std::string& targetType)
{
    return ShaderGraph::ArePortTypesCompatible(sourceType, targetType);
}

class GraphSnapshotCommand final : public Editor::IEditorCommand
{
public:
    GraphSnapshotCommand(GraphPanel* panel,
                         std::vector<std::uint8_t> before,
                         std::vector<std::uint8_t> after,
                         std::string name)
        : m_Panel(panel), m_Before(std::move(before)), m_After(std::move(after)), m_Name(std::move(name))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override
    {
        if (m_Panel && !m_After.empty())
            m_Panel->ApplyGraphSnapshot(m_After);
    }

    void Undo() override
    {
        if (m_Panel && !m_Before.empty())
            m_Panel->ApplyGraphSnapshot(m_Before);
    }

    void Redo() override { Do(); }

private:
    GraphPanel* m_Panel = nullptr;
    std::vector<std::uint8_t> m_Before;
    std::vector<std::uint8_t> m_After;
    std::string m_Name;
};

void ShiftLayoutSubtree(UIElement* el, float dx, float dy)
{
    if (!el)
        return;
    UILayoutAccess::SetLastLayoutRect(*el, el->GetLayoutX() + dx, el->GetLayoutY() + dy,
                          el->GetLayoutWidth(), el->GetLayoutHeight());
    for (const auto& child : el->GetChildren())
        ShiftLayoutSubtree(child.get(), dx, dy);
}

void MarkLayoutVisualDirtySubtree(UIElement* el)
{
    if (!el)
        return;
    el->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    for (const auto& child : el->GetChildren())
        MarkLayoutVisualDirtySubtree(child.get());
}

void MarkPanelToggleDirty(UIElement* panel, UIElement* body, ScrollView* scroll)
{
    if (panel)
    {
        panel->MarkDirtySubtree(UIElement::StyleDirty |
                                UIElement::LayoutDirty |
                                UIElement::VisualDirty |
                                UIElement::ChildrenDirty);
        panel->RequestRelayout();
    }
    if (scroll)
    {
        scroll->MarkHorizontalMeasureDirty();
        scroll->ClampScroll();
        scroll->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (body)
    {
        body->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        body->RequestRelayout();
    }
}

uint32_t WithAlpha(uint32_t argb, uint32_t alpha)
{
    return (argb & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24u);
}

std::string TrimCopy(const std::string& value)
{
    auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
}

bool NodeReferencesGraphVariable(const Graph::Node& node, const std::string& name)
{
    auto it = node.Parameters.find("variableName");
    return it != node.Parameters.end() && TrimCopy(it->second.ToString()) == name;
}

std::string NormalizeGraphVariableType(const std::string& type)
{
    static const std::unordered_set<std::string> kTypes = {
        "bool", "float", "int", "string", "float2", "float3", "float4"
    };
    return kTypes.count(type) ? type : std::string("float");
}

std::string FormatGraphVariableFloat(float value)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss << std::setprecision(3) << value;
    std::string out = oss.str();
    while (out.size() > 1 && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out.empty() ? std::string("0") : out;
}

int GetVariableFieldModifierKeys(UIElement* element)
{
    if (element)
    {
        if (UIManager* owner = element->GetOwnerManager())
            return owner->GetModifierKeys();
    }
    return 0;
}

float ApplyVariableFieldDragPrecision(float deltaX, int mods)
{
    if ((mods & Input::kModShift) != 0)
        deltaX *= 0.1f;
    return deltaX;
}

class VariableFloatField final : public FloatField {
public:
    VariableFloatField()
    {
        if (TextInput* editor = GetTextInput())
        {
            editor->RegisterEventHandler(kEventMouseDown, [this, editor](UIEvent& e)
            {
                if (e.Button != 0)
                    return;
                BeginScrub(e.X);
                e.Capture(editor);
                e.Stop();
            });
            editor->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
            {
                if (!m_ScrubPending && !m_Scrubbing)
                    return;
                UpdateScrub(e.X);
                e.Stop();
            });
            editor->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
            {
                if (e.Button != 0 || (!m_ScrubPending && !m_Scrubbing))
                    return;
                EndScrub();
                e.Stop();
            });
        }
    }

private:
    bool m_ScrubPending = false;
    bool m_Scrubbing = false;
    bool m_SuppressTextFocus = false;
    bool m_FocusableBeforeScrub = true;
    float m_ScrubStartX = 0.f;
    float m_ScrubStartValue = 0.f;

    void BeginScrub(float mouseX)
    {
        m_SuppressTextFocus = false;
        m_FocusableBeforeScrub = IsFocusable();
        SetFocusable(false);
        if (UIManager* owner = GetOwnerManager())
            owner->SetFocusById({});
        FloatField::OnFocusChanged(false);
        m_ScrubPending = true;
        m_Scrubbing = false;
        m_ScrubStartX = mouseX;
        m_ScrubStartValue = GetValue();
    }

    void UpdateScrub(float mouseX)
    {
        if (!m_ScrubPending && !m_Scrubbing)
            return;

        constexpr float kScrubThresholdPx = 4.f;
        const float deltaX = mouseX - m_ScrubStartX;
        if (!m_Scrubbing && std::fabs(deltaX) <= kScrubThresholdPx)
            return;

        if (!m_Scrubbing)
            m_SuppressTextFocus = true;
        m_Scrubbing = true;
        const int mods = GetVariableFieldModifierKeys(this);
        const float adjustedDeltaX = ApplyVariableFieldDragPrecision(deltaX, mods);
        float newValue = m_ScrubStartValue + adjustedDeltaX * InspectorDrag::kInspectorDragFloatSensitivity;
        if (Input::IsPrimaryShortcutModifier(mods))
            newValue = std::round(newValue);
        SetValue(newValue);
        NotifyValueChanging();
    }

    void EndScrub()
    {
        const bool wasScrubbing = m_Scrubbing;
        m_ScrubPending = false;
        m_Scrubbing = false;
        if (wasScrubbing)
        {
            FloatField::OnFocusChanged(false);
            m_SuppressTextFocus = false;
            NotifyValueChanged();
            PostSafeAction([this, focusable = m_FocusableBeforeScrub]()
            {
                SetFocusable(focusable);
            });
        }
        else
        {
            SetFocusable(m_FocusableBeforeScrub);
            m_SuppressTextFocus = false;
            FloatField::OnFocusChanged(true);
            SelectAll();
        }
    }

    void OnFocusChanged(bool focused) override
    {
        if (focused && (m_ScrubPending || m_SuppressTextFocus))
            return;
        if (!focused)
            m_SuppressTextFocus = false;
        FloatField::OnFocusChanged(focused);
    }

    bool OnChar(unsigned int codepoint) override
    {
        if (m_SuppressTextFocus)
            return false;
        return FloatField::OnChar(codepoint);
    }

    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override
    {
        if (m_SuppressTextFocus)
            return false;
        return FloatField::OnKey(key, mods, platform);
    }

    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override
    {
        (void)mouseY;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)style;
        (void)font;
        BeginScrub(mouseX);
    }

    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override
    {
        (void)mouseY;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)style;
        (void)font;
        UpdateScrub(mouseX);
    }

    void OnEvent(UIEvent& e) override
    {
        FloatField::OnEvent(e);
        if (e.Id == kEventMouseUp && (m_ScrubPending || m_Scrubbing))
        {
            EndScrub();
            e.Stop();
        }
    }
};

class VariableIntField final : public IntField {
public:
    VariableIntField()
    {
        if (TextInput* editor = GetTextInput())
        {
            editor->RegisterEventHandler(kEventMouseDown, [this, editor](UIEvent& e)
            {
                if (e.Button != 0)
                    return;
                BeginScrub(e.X);
                e.Capture(editor);
                e.Stop();
            });
            editor->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
            {
                if (!m_ScrubPending && !m_Scrubbing)
                    return;
                UpdateScrub(e.X);
                e.Stop();
            });
            editor->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
            {
                if (e.Button != 0 || (!m_ScrubPending && !m_Scrubbing))
                    return;
                EndScrub();
                e.Stop();
            });
        }
    }

private:
    bool m_ScrubPending = false;
    bool m_Scrubbing = false;
    bool m_SuppressTextFocus = false;
    bool m_FocusableBeforeScrub = true;
    float m_ScrubStartX = 0.f;
    int m_ScrubStartValue = 0;

    void BeginScrub(float mouseX)
    {
        m_SuppressTextFocus = false;
        m_FocusableBeforeScrub = IsFocusable();
        SetFocusable(false);
        if (UIManager* owner = GetOwnerManager())
            owner->SetFocusById({});
        IntField::OnFocusChanged(false);
        m_ScrubPending = true;
        m_Scrubbing = false;
        m_ScrubStartX = mouseX;
        m_ScrubStartValue = GetValue();
    }

    void UpdateScrub(float mouseX)
    {
        if (!m_ScrubPending && !m_Scrubbing)
            return;

        constexpr float kScrubThresholdPx = 4.f;
        const float deltaX = mouseX - m_ScrubStartX;
        if (!m_Scrubbing && std::fabs(deltaX) <= kScrubThresholdPx)
            return;

        if (!m_Scrubbing)
            m_SuppressTextFocus = true;
        m_Scrubbing = true;
        const int mods = GetVariableFieldModifierKeys(this);
        const float adjustedDeltaX = ApplyVariableFieldDragPrecision(deltaX, mods);
        SetValue(m_ScrubStartValue + static_cast<int>(std::round(adjustedDeltaX * InspectorDrag::kInspectorDragIntSensitivity)));
        NotifyValueChanging();
    }

    void EndScrub()
    {
        const bool wasScrubbing = m_Scrubbing;
        m_ScrubPending = false;
        m_Scrubbing = false;
        if (wasScrubbing)
        {
            IntField::OnFocusChanged(false);
            m_SuppressTextFocus = false;
            NotifyValueChanged();
            PostSafeAction([this, focusable = m_FocusableBeforeScrub]()
            {
                SetFocusable(focusable);
            });
        }
        else
        {
            SetFocusable(m_FocusableBeforeScrub);
            m_SuppressTextFocus = false;
            IntField::OnFocusChanged(true);
            SelectAll();
        }
    }

    void OnFocusChanged(bool focused) override
    {
        if (focused && (m_ScrubPending || m_SuppressTextFocus))
            return;
        if (!focused)
            m_SuppressTextFocus = false;
        IntField::OnFocusChanged(focused);
    }

    bool OnChar(unsigned int codepoint) override
    {
        if (m_SuppressTextFocus)
            return false;
        return IntField::OnChar(codepoint);
    }

    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override
    {
        if (m_SuppressTextFocus)
            return false;
        return IntField::OnKey(key, mods, platform);
    }

    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override
    {
        (void)mouseY;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)style;
        (void)font;
        BeginScrub(mouseX);
    }

    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override
    {
        (void)mouseY;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
        (void)style;
        (void)font;
        UpdateScrub(mouseX);
    }

    void OnEvent(UIEvent& e) override
    {
        IntField::OnEvent(e);
        if (e.Id == kEventMouseUp && (m_ScrubPending || m_Scrubbing))
        {
            EndScrub();
            e.Stop();
        }
    }
};

std::string DefaultGraphVariableValue(const std::string& type)
{
    const std::string normalized = NormalizeGraphVariableType(type);
    if (normalized == "bool")
        return "false";
    if (normalized == "string")
        return "";
    if (normalized == "float2")
        return "0, 0";
    if (normalized == "float3")
        return "0, 0, 0";
    if (normalized == "float4")
        return "0, 0, 0, 0";
    return "0";
}

std::vector<float> ParseGraphVariableVectorValue(const std::string& value, int componentCount)
{
    std::string normalized = value;
    for (char& c : normalized)
    {
        if (c == ',' || c == ';' || c == '(' || c == ')')
            c = ' ';
    }

    std::istringstream iss(normalized);
    std::vector<float> values;
    values.reserve(static_cast<size_t>(componentCount));
    float parsed = 0.f;
    while (iss >> parsed && static_cast<int>(values.size()) < componentCount)
        values.push_back(parsed);
    while (static_cast<int>(values.size()) < componentCount)
        values.push_back(0.f);
    return values;
}

std::string FormatGraphVariableVectorValue(const std::vector<float>& values, int componentCount)
{
    std::ostringstream oss;
    for (int i = 0; i < componentCount; ++i)
    {
        if (i > 0)
            oss << ", ";
        const float value = i < static_cast<int>(values.size()) ? values[static_cast<size_t>(i)] : 0.f;
        oss << FormatGraphVariableFloat(value);
    }
    return oss.str();
}

uint32_t GraphVariableVectorToArgb(const std::vector<float>& components, int componentCount)
{
    const float r = componentCount > 0 ? components[0] : 0.f;
    const float g = componentCount > 1 ? components[1] : 0.f;
    const float b = componentCount > 2 ? components[2] : 0.f;
    const float intensity = std::max(1.0f, std::max(r, std::max(g, b)));
    auto toByte = [](float v) -> uint32_t {
        const float clamped = std::max(0.0f, std::min(1.0f, v));
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    const float denom = std::max(1.0f, intensity);
    const uint32_t rr = toByte(r / denom);
    const uint32_t gg = toByte(g / denom);
    const uint32_t bb = toByte(b / denom);
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

void ArgbIntensityToGraphVariableVector(uint32_t argb, float intensity, float& r, float& g, float& b)
{
    const float scale = std::max(1.0f, intensity) / 255.0f;
    r = static_cast<float>((argb >> 16) & 0xFF) * scale;
    g = static_cast<float>((argb >> 8) & 0xFF) * scale;
    b = static_cast<float>(argb & 0xFF) * scale;
}

void StyleGraphVariableColorSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(20.0f))
        .Set(Style::Height, StyleLength::Px(20.0f))
        .Set(Style::MinWidth, StyleLength::Px(20.0f))
        .Set(Style::MinHeight, StyleLength::Px(20.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF555555u, 0xFF555555u, 0xFF555555u, 0xFF555555u})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

std::string NormalizeGraphVariableValue(const std::string& type, const std::string& value)
{
    const std::string normalized = NormalizeGraphVariableType(type);
    if (normalized == "bool")
    {
        std::string lower = TrimCopy(value);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return (lower == "true" || lower == "1" || lower == "yes" || lower == "on") ? "true" : "false";
    }
    if (normalized == "int")
    {
        int parsed = 0;
        if (IntField::TryParseInt(value, parsed))
            return std::to_string(parsed);
        return "0";
    }
    if (normalized == "float")
    {
        float parsed = 0.f;
        if (FloatField::TryParseFloat(value, parsed))
            return FormatGraphVariableFloat(parsed);
        return "0";
    }
    if (normalized == "float2" || normalized == "float3" || normalized == "float4")
    {
        const int count = normalized == "float2" ? 2 : (normalized == "float3" ? 3 : 4);
        return FormatGraphVariableVectorValue(ParseGraphVariableVectorValue(value, count), count);
    }
    return value;
}

std::vector<Dropdown::Option> GraphVariableTypeOptions()
{
    return {
        {"bool", "Bool"},
        {"float", "Float"},
        {"int", "Int"},
        {"string", "String"},
        {"float2", "Vector2"},
        {"float3", "Vector3"},
        {"float4", "Vector4"},
    };
}

int GraphVariableTypeIndex(const std::string& type)
{
    const std::vector<Dropdown::Option> options = GraphVariableTypeOptions();
    const std::string normalized = NormalizeGraphVariableType(type);
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].value == normalized)
            return static_cast<int>(i);
    }
    return 0;
}

bool LoadGraphModelFromPath(const std::filesystem::path& path, Graph::Model& outModel)
{
    if (path.empty())
        return false;
    if (path.extension() == ".glsl")
    {
        const auto file = ShaderGraph::LoadGraphFile(path);
        if (!ShaderGraph::IsShaderGraphSource(file.TagBlock + file.Body))
            return false;
        if (!Graph::LoadModelFromShaderGraphComments(file.TagBlock, outModel))
            return false;
        GraphNodeRegistry::Get().HydrateGraphModelPorts(outModel);
        return true;
    }
    GameEngine::String text;
    if (!GameEngine::ReadFileTextShared(path, text))
        return false;
    if (!Graph::FromJson(text, outModel))
        return false;
    GraphNodeRegistry::Get().HydrateGraphModelPorts(outModel);
    return true;
}

std::string LowerAsciiCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

GraphPanel::GraphPanel(std::string_view kindId)
    : DockPanel("Node Graph")
{
    g_LiveGraphPanels.insert(this);
    if (kindId.empty())
    {
        Logger::Log::Error("GraphPanel constructed with empty kind; using shader graph");
        m_PanelKindId = std::string(Graph::kKindIdMaterial);
    }
    else
    {
        m_PanelKindId = std::string(kindId);
    }
    m_Model.KindId = m_PanelKindId;
    m_ColorPaletteNodes = LoadColorPaletteNodesPreference();
    AddClass("node-graph-panel");
}

void GraphPanel::ForEachLive(const std::function<void(GraphPanel&)>& fn)
{
    for (GraphPanel* panel : g_LiveGraphPanels)
    {
        if (panel)
            fn(*panel);
    }
}

GraphPanel* GraphPanel::FindLiveByKind(std::string_view kindId)
{
    for (GraphPanel* panel : g_LiveGraphPanels)
    {
        if (panel && panel->m_PanelKindId == kindId)
            return panel;
    }
    return nullptr;
}

GraphPanel* GraphPanel::FindLiveByPath(const std::filesystem::path& path)
{
    if (path.empty())
        return nullptr;
    for (GraphPanel* panel : g_LiveGraphPanels)
    {
        if (panel && panel->m_CurrentPath == path)
            return panel;
    }
    return nullptr;
}

GraphPanel* GraphPanel::FindEmptyLiveByKind(std::string_view kindId)
{
    for (GraphPanel* panel : g_LiveGraphPanels)
    {
        if (panel && panel->m_PanelKindId == kindId && panel->m_CurrentPath.empty() &&
            !panel->HasUnsavedChanges())
            return panel;
    }
    return nullptr;
}

GraphPanel* GraphPanel::FindLiveLastOpened()
{
    return g_LastOpenedGraphPanel;
}

void GraphPanel::NoteAsLastOpened()
{
    g_LastOpenedGraphPanel = this;
}

GraphPanel* GraphPanel::InspectorTarget()
{
    return g_InspectorGraphPanel;
}

bool GraphPanel::TryReadKindId(const std::filesystem::path& path, std::string& outKindId)
{
    Graph::Model model;
    if (!LoadGraphModelFromPath(path, model) || model.KindId.empty())
        return false;
    outKindId = model.KindId;
    return true;
}

GraphPanel::~GraphPanel()
{
    if (g_LastOpenedGraphPanel == this)
        g_LastOpenedGraphPanel = nullptr;
    if (g_InspectorGraphPanel == this)
        g_InspectorGraphPanel = nullptr;
    g_LiveGraphPanels.erase(this);
    if (m_PaletteSearchField && m_PaletteSearchField->GetParent())
        EditorSearchBars::UnregisterFocusTarget(m_PaletteSearchField->GetParent());
    if (m_VariablesSearchField && m_VariablesSearchField->GetParent())
        EditorSearchBars::UnregisterFocusTarget(m_VariablesSearchField->GetParent());
    HidePaletteNodeDragGhost();
}

bool GraphPanel::GetPanelScrollbarsPreference()
{
    if (!g_PanelScrollbarsPreferenceInitialized)
    {
        g_PanelScrollbarsPreference = LoadPanelScrollbarsPreference();
        g_PanelScrollbarsPreferenceInitialized = true;
    }
    return g_PanelScrollbarsPreference;
}

void GraphPanel::SetPanelScrollbarsPreference(bool enabled)
{
    g_PanelScrollbarsPreference = enabled;
    g_PanelScrollbarsPreferenceInitialized = true;
    for (GraphPanel* panel : g_LiveGraphPanels)
        panel->ApplyPanelScrollbarPreference();
}

bool GraphPanel::GetVariablesScrollbarsPreference()
{
    if (!g_VariablesScrollbarsPreferenceInitialized)
    {
        g_VariablesScrollbarsPreference = LoadVariablesScrollbarsPreference();
        g_VariablesScrollbarsPreferenceInitialized = true;
    }
    return g_VariablesScrollbarsPreference;
}

void GraphPanel::SetVariablesScrollbarsPreference(bool enabled)
{
    g_VariablesScrollbarsPreference = enabled;
    g_VariablesScrollbarsPreferenceInitialized = true;
    for (GraphPanel* panel : g_LiveGraphPanels)
        panel->ApplyPanelScrollbarPreference();
}

void GraphPanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    /* Asset services must wire here, not at SetupUI: the context does not
       exist yet at build time, which would leave the texture picker dead. */
    if (m_Canvas)
        m_Canvas->SetAssetServices(
            m_Context && m_Context->Assets ? &m_Context->Assets->GetRegistry() : nullptr,
            m_Context ? m_Context->Thumbnails : nullptr);
    Notify(GraphPanelEvent::ContextChanged);
}

void GraphPanel::Update()
{
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.f / 60.f;
    if (m_UpdateLastTick.time_since_epoch().count() != 0)
        dt = std::clamp(std::chrono::duration<float>(now - m_UpdateLastTick).count(), 0.f, 0.1f);
    m_UpdateLastTick = now;

    if (m_KindHooks.OnUpdate)
        m_KindHooks.OnUpdate(dt);
    if (m_Canvas)
        m_Canvas->FlushDeferredModelMutation();
    /* A drag that ends without its callback (the dragged widget destroyed
       mid-gesture, a programmatic node delete, capture loss) leaves the latch
       set and freezes every deferred refresh below. A drag cannot be in
       progress without a captured pointer: self-heal. */
    if (m_NodeDragInProgress && m_Canvas)
    {
        UIManager* ui = m_Canvas->GetOwnerManager();
        if (!ui || !ui->IsMouseCaptured())
            m_NodeDragInProgress = false;
    }
    if (m_InspectorRefreshDeferred)
    {
        /* Node-side edits mirror into the inspector while it shows that node.
           Inspector-side edits go through RefreshBoundNodeValues instead and
           never raise this flag, so a focused inspector field is never
           rebuilt mid-keystroke. Held while a pointer gesture owns the mouse:
           a rebuild mid-gesture destroys elements under the pointer and
           cancels the capture, which killed node drags and scrubs outright.
           Keyed on capture, not IsMouseDown — a lost release wedges the
           latter forever, capture clears on release and on captured-element
           destruction. The flag stays set, so release delivers the refresh. */
        UIManager* ui = m_Canvas ? m_Canvas->GetOwnerManager() : nullptr;
        const bool gestureActive = (ui && ui->IsMouseCaptured()) || m_NodeDragInProgress;
        if (!gestureActive)
        {
            m_InspectorRefreshDeferred = false;
            if (m_Canvas)
            {
                const std::string selected = m_Canvas->GetSelectedNodeId();
                if (!selected.empty())
                    OnCanvasSelectionChanged(selected);
            }
        }
    }
    if (m_Canvas)
        m_Canvas->TickRuntimeTransitionAnimation(dt);
}

void GraphPanel::OnPostLayout()
{
    ApplyPanelScrollbarPreference();
    ApplyNodePaletteBounds();
    ApplyVariablesPanelBounds();
    UpdatePanelOverflowIndicators();
    Notify(GraphPanelEvent::PostLayout);

    /* When panel is redocked or resized, canvas must redraw and use new layout (hit-test/draw use parent bounds). */
    if (m_Canvas)
    {
        m_Canvas->MarkDirty(UIElement::VisualDirty);
        if (m_FrameNodesOnNextLayout)
        {
            UIElement* boundsEl = m_Canvas->GetParent();
            float visW = boundsEl ? boundsEl->GetLayoutWidth() : 0.f;
            float visH = boundsEl ? boundsEl->GetLayoutHeight() : 0.f;
            if (visW >= 100.f && visH >= 100.f)
            {
                m_FrameNodesOnNextLayout = false;
                m_FrameRetries = 0;
                m_Canvas->FrameNodesToFit();
                SyncViewportToModel();
            }
            else if (++m_FrameRetries > 10)
            {
                /* Give up after a few layout passes so the flag doesn't fire unexpectedly later */
                m_FrameNodesOnNextLayout = false;
                m_FrameRetries = 0;
            }
        }
    }
}

void GraphPanel::ApplyPanelScrollbarPreference()
{
    auto apply = [](ScrollView* scroll, bool showScrollbars)
    {
        if (!scroll)
            return;
        scroll->Overrides()
            .Set(Style::OverflowXProp, Overflow::Hidden)
            .Set(Style::OverflowYProp, showScrollbars ? Overflow::Visible : Overflow::Hidden);
        scroll->MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty | UIElement::VisualDirty);
    };

    const bool showPanelScrollbars = GetPanelScrollbarsPreference();
    if (showPanelScrollbars != m_LastPanelScrollbarsVisible)
    {
        m_LastPanelScrollbarsVisible = showPanelScrollbars;
        apply(m_PaletteScroll, showPanelScrollbars);
    }

    const bool showVariablesScrollbars = GetVariablesScrollbarsPreference();
    if (showVariablesScrollbars != m_LastVariablesScrollbarsVisible)
    {
        m_LastVariablesScrollbarsVisible = showVariablesScrollbars;
        apply(m_VariablesScroll, showVariablesScrollbars);
    }
}

void GraphPanel::UpdatePanelOverflowIndicators()
{
    auto setVisible = [](UIElement* indicator, bool shouldShow)
    {
        if (!indicator)
            return;

        const bool isHidden = indicator->HasClass("hidden");
        if (shouldShow == isHidden)
        {
            if (shouldShow)
                indicator->RemoveClass("hidden");
            else
                indicator->AddClass("hidden");
            indicator->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    };

    auto update = [setVisible](UIElement* topIndicator, UIElement* bottomIndicator,
                               const ScrollView* scroll, const UIElement* content)
    {
        if (!topIndicator || !bottomIndicator || !scroll)
            return;

        UIElement* parent = topIndicator->GetParent();
        if (!parent)
            return;

        constexpr float kIndicatorHeight = 28.0f;
        float localLeft = std::round(scroll->GetLayoutX() - parent->GetLayoutX());
        const float localTop = std::round(scroll->GetLayoutY() - parent->GetLayoutY());
        float scrollW = std::round(scroll->GetLayoutWidth());
        if (content && content->GetLayoutWidth() > 1.0f)
        {
            localLeft = std::round(content->GetLayoutX() - parent->GetLayoutX());
            scrollW = std::round(content->GetLayoutWidth());
        }
        const float viewportH = std::round(scroll->GetViewportHeight());
        if (scrollW <= 1.0f || viewportH <= 1.0f)
        {
            setVisible(topIndicator, false);
            setVisible(bottomIndicator, false);
            return;
        }

        auto place = [&](UIElement* indicator, float top)
        {
            indicator->Overrides()
                .Set(Style::Position, PositionType::Absolute)
                .Set(Style::PositionLeft, StyleLength::Px(localLeft))
                .Set(Style::PositionTop, StyleLength::Px(std::round(top)))
                .Set(Style::Width, StyleLength::Px(scrollW))
                .Set(Style::Height, StyleLength::Px(kIndicatorHeight));
        };
        place(topIndicator, localTop);
        place(bottomIndicator, localTop + viewportH - kIndicatorHeight);

        const float maxY = std::max(0.0f, scroll->GetContentHeight() - scroll->GetViewportHeight());
        const float scrollY = scroll->GetScrollY();
        setVisible(topIndicator, maxY > 1.0f && scrollY > 1.0f);
        setVisible(bottomIndicator, maxY > 1.0f && scrollY < maxY - 1.0f);
    };

    update(m_PaletteTopOverflowIndicator, m_PaletteBottomOverflowIndicator, m_PaletteScroll, m_PaletteContent);
    update(m_VariablesTopOverflowIndicator, m_VariablesBottomOverflowIndicator, m_VariablesScroll, m_VariablesContent);
}

void GraphPanel::ApplyNodePaletteBounds()
{
    if (!m_Palette)
        return;
    if (!m_NodePalettePanelVisible)
        return;

    UIElement* bodyEl = m_Palette->GetParent();
    if (!bodyEl)
        return;

    constexpr float kPalettePaddingY = 16.f;
    constexpr float kPaletteTitleHeight = 40.f;
    constexpr float kPaletteSearchHeight = 28.f;
    constexpr float kPaletteOuterGapY = 16.f;
    constexpr float kPaletteContentPaddingY = 8.f;
    constexpr float kPaletteContentGapY = 4.f;
    constexpr float kPaletteSectionTitleHeight = 20.f;
    constexpr float kPaletteSectionButtonHeight = 24.f;
    constexpr float kPaletteSectionGapY = 4.f;
    constexpr float kPaletteSectionMarginBottom = 8.f;
    constexpr float kPaletteVisibleGrabArea = 48.f;
    constexpr int kPaletteZIndex = 10;

    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    if (bodyW <= 1.f || bodyH <= 1.f)
        return;

    if (m_PaletteCollapsed)
    {
        if (m_PaletteHeightOverrideApplied)
        {
            m_Palette->Overrides()
                .Set(Style::Height, StyleLength::Px(kNodePaletteCollapsedHeight))
                .Set(Style::MaxHeight, StyleLength::Px(kNodePaletteCollapsedHeight));
            m_PaletteHeightOverrideApplied = false;
            m_LastAppliedPaletteHeight = -1.f;
            m_Palette->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        return;
    }

    float left = m_Palette->GetLayoutX() - bodyEl->GetLayoutX();
    float top = m_Palette->GetLayoutY() - bodyEl->GetLayoutY();
    if (left <= 0.f)
        left = kFloatingPanelMargin;
    if (top <= 0.f)
        top = kFloatingPanelMargin;

    const float maxLeft = std::max(kFloatingPanelMargin, bodyW - kNodePaletteWidth - kFloatingPanelMargin);
    const float maxTop = std::max(kFloatingPanelMargin, bodyH - kPaletteVisibleGrabArea);
    left = std::clamp(left, kFloatingPanelMargin, maxLeft);
    top = std::clamp(top, kFloatingPanelMargin, maxTop);

    float contentH = kPaletteContentPaddingY;
    size_t sectionCount = 0;
    if (m_PaletteContent)
    {
        for (const auto& section : m_PaletteContent->GetChildren())
        {
            const size_t childCount = section ? section->GetChildren().size() : 0;
            if (childCount == 0)
                continue;
            const size_t buttonCount = childCount > 0 ? childCount - 1 : 0;
            contentH += kPaletteSectionTitleHeight;
            contentH += static_cast<float>(buttonCount) * kPaletteSectionButtonHeight;
            contentH += static_cast<float>(childCount - 1) * kPaletteSectionGapY;
            contentH += kPaletteSectionMarginBottom;
            ++sectionCount;
        }
    }
    if (sectionCount > 1)
        contentH += static_cast<float>(sectionCount - 1) * kPaletteContentGapY;
    if (sectionCount == 0)
        contentH += kPaletteSectionTitleHeight;

    float desiredPaletteH = kPalettePaddingY + kPaletteTitleHeight + kPaletteSearchHeight +
                            kPaletteOuterGapY + contentH;
    if (m_PaletteUserHeight > 0.f)
        desiredPaletteH = m_PaletteUserHeight;
    const float maxPaletteH = std::max(kNodePaletteMinHeight,
                                       std::min(bodyH * kFloatingPanelMaxBodyRatio,
                                                bodyH - top - kFloatingPanelMargin));
    const float paletteH = std::clamp(desiredPaletteH, kNodePaletteMinHeight, maxPaletteH);

    const bool heightChanged = !m_PaletteHeightOverrideApplied ||
                               std::fabs(m_LastAppliedPaletteHeight - paletteH) > 0.5f;
    if (!heightChanged && !m_DraggingPalette)
        return;

    m_Palette->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(std::round(left)))
        .Set(Style::PositionTop, StyleLength::Px(std::round(top)))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(kNodePaletteWidth))
        .Set(Style::Height, StyleLength::Px(std::round(paletteH)))
        .Set(Style::MaxHeight, StyleLength::Px(std::round(paletteH)))
        .Set(Style::ZIndex, kPaletteZIndex);

    m_PaletteHeightOverrideApplied = true;
    m_LastAppliedPaletteHeight = paletteH;
    UILayoutAccess::SetLastLayoutRect(*m_Palette, bodyEl->GetLayoutX() + std::round(left),
                                 bodyEl->GetLayoutY() + std::round(top),
                                 kNodePaletteWidth,
                                 std::round(paletteH));
    m_Palette->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    if (m_PaletteScroll)
        m_PaletteScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_Palette->RequestRelayout();
}

void GraphPanel::ApplyVariablesPanelBounds()
{
    if (!m_VariablesPanel)
        return;
    if (!m_VariablesPanelVisible)
        return;

    UIElement* bodyEl = m_VariablesPanel->GetParent();
    if (!bodyEl)
        return;

    constexpr float kPanelPaddingY = 16.f;
    constexpr float kHeaderHeight = 28.f;
    constexpr float kSearchHeight = 28.f;
    constexpr float kOuterGapY = 16.f;
    constexpr float kContentPaddingY = 4.f;
    constexpr float kVisibleGrabArea = 48.f;
    constexpr int kZIndex = 9;

    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    if (bodyW <= 1.f || bodyH <= 1.f)
        return;

    if (m_VariablesCollapsed)
    {
        if (m_VariablesHeightOverrideApplied)
        {
            m_VariablesPanel->Overrides()
                .Set(Style::Height, StyleLength::Px(kVariablesPanelCollapsedHeight))
                .Set(Style::MaxHeight, StyleLength::Px(kVariablesPanelCollapsedHeight));
            m_VariablesHeightOverrideApplied = false;
            m_LastAppliedVariablesHeight = -1.f;
            m_VariablesPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        return;
    }

    float left = m_VariablesPanel->GetLayoutX() - bodyEl->GetLayoutX();
    float top = m_VariablesPanel->GetLayoutY() - bodyEl->GetLayoutY();
    if (left <= 0.f)
        left = 252.f;
    if (top <= 0.f)
        top = kFloatingPanelMargin;

    const float maxLeft = std::max(kFloatingPanelMargin, bodyW - kVariablesPanelWidth - kFloatingPanelMargin);
    const float maxTop = std::max(kFloatingPanelMargin, bodyH - kVisibleGrabArea);
    left = std::clamp(left, kFloatingPanelMargin, maxLeft);
    top = std::clamp(top, kFloatingPanelMargin, maxTop);

    const size_t visibleRows = m_VariablesContent ? std::max<size_t>(1, m_VariablesContent->GetChildren().size()) : 1;
    float desiredPanelH = kPanelPaddingY + kHeaderHeight + kSearchHeight + kOuterGapY +
                          kContentPaddingY + static_cast<float>(visibleRows) * kVariableRowHeight;
    if (m_VariablesUserHeight > 0.f)
        desiredPanelH = m_VariablesUserHeight;
    const float maxPanelH = std::max(kVariablesPanelMinHeight,
                                     std::min(bodyH * kFloatingPanelMaxBodyRatio,
                                              bodyH - top - kFloatingPanelMargin));
    const float panelH = std::clamp(desiredPanelH, kVariablesPanelMinHeight, maxPanelH);

    const bool heightChanged = !m_VariablesHeightOverrideApplied ||
                               std::fabs(m_LastAppliedVariablesHeight - panelH) > 0.5f;
    if (!heightChanged && !m_DraggingVariablesPanel)
        return;

    m_VariablesPanel->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(std::round(left)))
        .Set(Style::PositionTop, StyleLength::Px(std::round(top)))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(kVariablesPanelWidth))
        .Set(Style::Height, StyleLength::Px(std::round(panelH)))
        .Set(Style::MaxHeight, StyleLength::Px(std::round(panelH)))
        .Set(Style::ZIndex, kZIndex);

    m_VariablesHeightOverrideApplied = true;
    m_LastAppliedVariablesHeight = panelH;
    UILayoutAccess::SetLastLayoutRect(*m_VariablesPanel, bodyEl->GetLayoutX() + std::round(left),
                                        bodyEl->GetLayoutY() + std::round(top),
                                        kVariablesPanelWidth,
                                        std::round(panelH));
    m_VariablesPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    if (m_VariablesScroll)
        m_VariablesScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_VariablesPanel->RequestRelayout();
}

bool GraphPanel::MatchSearch(const std::string& displayName, const std::string& typeId,
                                  const std::string& search)
{
    if (search.empty())
        return true;
    std::string q = search;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string d = displayName;
    std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string t = typeId;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return d.find(q) != std::string::npos || t.find(q) != std::string::npos;
}

void GraphPanel::RebuildPalette()
{
    if (!m_PaletteContent)
        return;
    // RemoveAllChildren, not a while-empty loop: RemoveChild defers during
    // event dispatch, so the live list does not shrink and the loop never ends.
    // A palette rebuild reached from a node double-click runs inside dispatch.
    m_PaletteContent->RemoveAllChildren();

    KindNodeFactory();

    const std::string search = m_PaletteSearchField ? m_PaletteSearchField->GetValue() : "";
    const std::string kind = m_Model.KindId;
    const std::vector<std::string> categories = GraphNodeRegistry::Get().GetCategories(kind);

    const GraphKindSample& sample = m_KindHooks.Sample;
    if (!sample.Label.empty() && MatchSearch(sample.Label, sample.SearchKey, search))
    {
        auto testSection = std::make_unique<UIElement>();
        testSection->AddClass("node-graph-palette-section");

        auto testTitle = std::make_unique<Label>();
        testTitle->SetText("Test Graphs");
        testTitle->AddClass("node-graph-palette-section-title");
        testSection->AddChild(std::move(testTitle));

        auto testBtn = std::make_unique<Button>();
        testBtn->SetText(sample.Label);
        testBtn->AddClass("node-graph-palette-node-button");
        // The row belongs to the kind that offered it, which is the one already
        // open, so loading it needs no kind forced.
        testBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ScheduleLoadTestGraph(); });
        testSection->AddChild(std::move(testBtn));

        m_PaletteContent->AddChild(std::move(testSection));
    }

    for (const std::string& cat : categories)
    {
        if (!IsPaletteCategoryVisible(cat))
            continue;
        std::vector<NodeTypeMeta> types = GraphNodeRegistry::Get().GetTypesInCategory(kind, cat);
        auto section = std::make_unique<UIElement>();
        section->AddClass("node-graph-palette-section");
        auto sectionTitle = std::make_unique<Label>();
        sectionTitle->SetText(cat);
        sectionTitle->AddClass("node-graph-palette-section-title");
        section->AddChild(std::move(sectionTitle));
        int added = 0;
        for (const NodeTypeMeta& meta : types)
        {
            if (!MatchSearch(meta.DisplayName, meta.TypeId, search))
                continue;
            auto item = std::make_unique<UIElement>();
            item->AddClass("button");
            item->AddClass("node-graph-palette-node-button");
            if (m_ColorPaletteNodes)
            {
                const uint32_t nodeColor = NodeColorSettings::GetNodeColorArgb(kind, meta.TypeId);
                const uint32_t borderColor = WithAlpha(nodeColor, 0xFFu);
                item->AddClass("node-graph-palette-node-button-colored");
                item->Overrides()
                    .Set(Style::BackgroundColor, WithAlpha(nodeColor, 0xEEu))
                    .Set(Style::BorderWidth, Box4{1.f, 1.f, 1.f, 1.f})
                    .Set(Style::BorderColor, BorderColorsTRBL{borderColor, borderColor, borderColor, borderColor});
            }
            auto label = std::make_unique<Label>();
            label->SetText(meta.DisplayName);
            label->AddClass("button-text");
            if (m_ColorPaletteNodes)
                label->Overrides().Set(Style::Color, 0xFFFFFFFFu);
            auto icon = std::make_unique<UIElement>();
            icon->AddClass("graph-node-palette-icon");
            GraphNodeIcons::ApplyIconClass(*icon, meta.IconStem);
            item->AddChild(std::move(icon));
            item->AddChild(std::move(label));
            UIElement* itemRaw = item.get();
            std::string typeId = meta.TypeId;
            itemRaw->RegisterEventHandler(kEventMouseDown, [this, itemRaw, typeId](UIEvent& e) {
                if (e.Button != 0)
                    return;
                m_PaletteNodeDragPending = true;
                m_DraggingPaletteNode = false;
                m_PaletteNodeDragTypeId = typeId;
                m_PaletteNodeDragStartX = e.X;
                m_PaletteNodeDragStartY = e.Y;
                itemRaw->AddClass("pressed");
                e.Capture(itemRaw);
                e.Stop();
            });
            itemRaw->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
                if (!m_PaletteNodeDragPending && !m_DraggingPaletteNode)
                    return;
                constexpr float kDragThresholdPx = 4.f;
                const float dx = e.X - m_PaletteNodeDragStartX;
                const float dy = e.Y - m_PaletteNodeDragStartY;
                if (m_PaletteNodeDragPending && dx * dx + dy * dy > kDragThresholdPx * kDragThresholdPx)
                {
                    m_PaletteNodeDragPending = false;
                    m_DraggingPaletteNode = true;
                    ShowPaletteNodeDragGhost(m_PaletteNodeDragTypeId, e.X, e.Y);
                }
                if (m_DraggingPaletteNode)
                    UpdatePaletteNodeDragGhost(e.X, e.Y);
                e.Stop();
            });
            itemRaw->RegisterEventHandler(kEventMouseUp, [this, itemRaw](UIEvent& e) {
                if (e.Button != 0 || (!m_PaletteNodeDragPending && !m_DraggingPaletteNode))
                    return;
                const bool wasDrag = m_DraggingPaletteNode;
                const std::string typeId = m_PaletteNodeDragTypeId;
                m_PaletteNodeDragPending = false;
                m_DraggingPaletteNode = false;
                m_PaletteNodeDragTypeId.clear();
                HidePaletteNodeDragGhost();
                itemRaw->RemoveClass("pressed");
                if (wasDrag)
                    AddNodeFromPaletteAtScreen(typeId, e.X, e.Y, true, true);
                else
                    AddNodeFromPalette(typeId);
                e.Stop();
            });
            section->AddChild(std::move(item));
            ++added;
        }
        if (added > 0)
            m_PaletteContent->AddChild(std::move(section));
    }

    if (m_PaletteScroll)
        m_PaletteScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::SchedulePaletteRebuild()
{
    if (m_PaletteRebuildScheduled)
        return;

    m_PaletteRebuildScheduled = true;
    Scheduler::IScheduler* sched = GetScheduler();
    if (!sched)
    {
        m_PaletteRebuildScheduled = false;
        if (m_NodePalettePanelVisible)
        {
            RebuildPalette();
            ApplyNodePaletteBounds();
        }
        return;
    }

    sched->ScheduleNext([this]()
    {
        m_PaletteRebuildScheduled = false;
        if (!m_NodePalettePanelVisible)
            return;
        RebuildPalette();
        ApplyNodePaletteBounds();
        if (m_PaletteScroll)
            m_PaletteScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        RequestRelayout();
    });
}

Graph::Variable* GraphPanel::FindGraphVariable(const std::string& name)
{
    for (auto& variable : m_Model.Variables)
    {
        if (variable.Name == name)
            return &variable;
    }
    return nullptr;
}

const Graph::Variable* GraphPanel::FindGraphVariable(const std::string& name) const
{
    for (const auto& variable : m_Model.Variables)
    {
        if (variable.Name == name)
            return &variable;
    }
    return nullptr;
}

std::uint64_t GraphPanel::NextGraphVariableCreatedOrder() const
{
    std::uint64_t next = 0;
    for (const auto& variable : m_Model.Variables)
        next = std::max(next, variable.CreatedOrder + 1);
    return next;
}

void GraphPanel::AssignMissingVariableCreatedOrders()
{
    std::unordered_set<std::uint64_t> seen;
    std::uint64_t next = 0;
    for (auto& variable : m_Model.Variables)
    {
        while (seen.count(next))
            ++next;
        if (seen.count(variable.CreatedOrder))
            variable.CreatedOrder = next;
        seen.insert(variable.CreatedOrder);
    }
}

void GraphPanel::SetVariablesSortMode(VariablesSortMode mode)
{
    if (m_VariablesSortMode == mode)
        return;
    m_VariablesSortMode = mode;
    ScheduleVariablesPanelRebuild();
}

void GraphPanel::SetVariablesSortAscending(bool ascending)
{
    if (m_VariablesSortAscending == ascending)
        return;
    m_VariablesSortAscending = ascending;
    ScheduleVariablesPanelRebuild();
}

void GraphPanel::ShowVariablesSortContextMenu(float screenX, float screenY)
{
    if (!m_Context || !m_Context->MainWindow)
        return;
    if (!m_VariablesContextMenu)
        m_VariablesContextMenu = CreateContextMenu();
    if (!m_VariablesContextMenu)
        return;

    constexpr uint32_t kCmdSortCreatedAsc = 51001;
    constexpr uint32_t kCmdSortCreatedDesc = 51002;
    constexpr uint32_t kCmdSortNameAsc = 51003;
    constexpr uint32_t kCmdSortNameDesc = 51004;
    constexpr uint32_t kCmdSortCustomAsc = 51005;
    constexpr uint32_t kCmdSortCustomDesc = 51006;

    m_VariablesContextMenu->Clear();
    ContextMenuBuilder builder;
    const auto checked = [this](VariablesSortMode mode, bool ascending) -> uint32_t {
        return MenuItemFlag_Radio |
               ((m_VariablesSortMode == mode && m_VariablesSortAscending == ascending)
                    ? MenuItemFlag_Checked
                    : MenuItemFlag_None);
    };
    builder.AddItem("Date Created", 0, MenuItemFlag_None, 0, EditorIcons::kAlarm);
    builder.AddItem("ABC", 0, MenuItemFlag_None, 0, EditorIcons::kSortList);
    builder.AddItem("Custom", 0, MenuItemFlag_None, 0, EditorIcons::kMove);
    builder.AddItem("Date Created/Oldest First", kCmdSortCreatedAsc, checked(VariablesSortMode::Created, true), 0,
                    EditorIcons::kArrowUp);
    builder.AddItem("Date Created/Newest First", kCmdSortCreatedDesc, checked(VariablesSortMode::Created, false), 0,
                    EditorIcons::kArrowDown);
    builder.AddItem("ABC/A to Z", kCmdSortNameAsc, checked(VariablesSortMode::Name, true), 0,
                    EditorIcons::kArrowUp);
    builder.AddItem("ABC/Z to A", kCmdSortNameDesc, checked(VariablesSortMode::Name, false), 0,
                    EditorIcons::kArrowDown);
    builder.AddItem("Custom/Top to Bottom", kCmdSortCustomAsc, checked(VariablesSortMode::Custom, true), 0,
                    EditorIcons::kArrowDown);
    builder.AddItem("Custom/Bottom to Top", kCmdSortCustomDesc, checked(VariablesSortMode::Custom, false), 0,
                    EditorIcons::kArrowUp);
    builder.Build(m_VariablesContextMenu.get());

    m_VariablesContextMenu->SetCommandHandler([this](uint32_t commandId)
    {
        switch (commandId)
        {
        case kCmdSortCreatedAsc:
            m_VariablesSortMode = VariablesSortMode::Created;
            m_VariablesSortAscending = true;
            break;
        case kCmdSortCreatedDesc:
            m_VariablesSortMode = VariablesSortMode::Created;
            m_VariablesSortAscending = false;
            break;
        case kCmdSortNameAsc:
            m_VariablesSortMode = VariablesSortMode::Name;
            m_VariablesSortAscending = true;
            break;
        case kCmdSortNameDesc:
            m_VariablesSortMode = VariablesSortMode::Name;
            m_VariablesSortAscending = false;
            break;
        case kCmdSortCustomAsc:
            m_VariablesSortMode = VariablesSortMode::Custom;
            m_VariablesSortAscending = true;
            break;
        case kCmdSortCustomDesc:
            m_VariablesSortMode = VariablesSortMode::Custom;
            m_VariablesSortAscending = false;
            break;
        default:
            return;
        }
        ScheduleVariablesPanelRebuild();
    });
    m_VariablesContextMenu->Show(m_Context->MainWindow, static_cast<int>(screenX), static_cast<int>(screenY));
}

bool GraphPanel::ReorderGraphVariable(const std::string& name, size_t targetDisplayIndex)
{
    const std::string cleanName = TrimCopy(name);
    if (cleanName.empty())
        return false;

    std::vector<Graph::Variable> displayed;
    const std::string search = m_VariablesSearchField ? m_VariablesSearchField->GetValue() : "";
    for (const Graph::Variable& variable : CollectVariablesForDisplay())
    {
        std::string searchableType = variable.Type;
        if (variable.IsPublic)
            searchableType += " public";
        if (variable.IsGlobal)
            searchableType += " global";
        if (!variable.Value.empty())
            searchableType += " " + variable.Value;
        if (MatchSearch(variable.Name, searchableType, search))
            displayed.push_back(variable);
    }
    if (displayed.empty())
        return false;

    std::vector<std::string> displayNames;
    displayNames.reserve(displayed.size());
    for (const auto& variable : displayed)
        displayNames.push_back(variable.Name);

    auto sourceDisplayIt = std::find(displayNames.begin(), displayNames.end(), cleanName);
    if (sourceDisplayIt == displayNames.end())
        return false;
    const size_t oldDisplayIndex = static_cast<size_t>(std::distance(displayNames.begin(), sourceDisplayIt));
    displayNames.erase(sourceDisplayIt);
    targetDisplayIndex = std::min(targetDisplayIndex, displayNames.size());
    displayNames.insert(displayNames.begin() + static_cast<std::ptrdiff_t>(targetDisplayIndex), cleanName);
    if (oldDisplayIndex == targetDisplayIndex)
        return false;

    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));

    for (const auto& variable : displayed)
        (void)EnsureGraphVariable(variable.Name);

    std::vector<Graph::Variable> reordered;
    reordered.reserve(m_Model.Variables.size());
    std::unordered_set<std::string> placed;
    for (const std::string& variableName : displayNames)
    {
        auto it = std::find_if(m_Model.Variables.begin(), m_Model.Variables.end(),
            [&variableName](const Graph::Variable& variable) { return variable.Name == variableName; });
        if (it == m_Model.Variables.end())
            continue;
        reordered.push_back(*it);
        placed.insert(variableName);
    }
    for (const auto& variable : m_Model.Variables)
    {
        if (!placed.count(variable.Name))
            reordered.push_back(variable);
    }

    if (reordered.size() != m_Model.Variables.size())
        return false;
    m_Model.Variables = std::move(reordered);
    m_VariablesSortMode = VariablesSortMode::Custom;

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Reorder Graph Variables");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
    return true;
}

Graph::Variable* GraphPanel::EnsureGraphVariable(const std::string& name)
{
    const std::string cleanName = TrimCopy(name);
    if (cleanName.empty())
        return nullptr;
    if (Graph::Variable* existing = FindGraphVariable(cleanName))
        return existing;

    Graph::Variable variable;
    variable.Name = cleanName;
    variable.Type = "float";
    variable.Value = DefaultGraphVariableValue(variable.Type);
    variable.CreatedOrder = NextGraphVariableCreatedOrder();
    m_Model.Variables.push_back(std::move(variable));
    return &m_Model.Variables.back();
}

std::vector<Graph::Variable> GraphPanel::CollectVariablesForDisplay() const
{
    std::vector<Graph::Variable> variables = m_Model.Variables;
    std::unordered_set<std::string> names;
    for (auto& variable : variables)
    {
        variable.Name = TrimCopy(variable.Name);
        variable.Type = NormalizeGraphVariableType(variable.Type);
        variable.Value = NormalizeGraphVariableValue(variable.Type, variable.Value.empty() ? DefaultGraphVariableValue(variable.Type) : variable.Value);
        if (!variable.Name.empty())
            names.insert(variable.Name);
    }

    if (m_KindHooks.CollectImpliedVariables)
        m_KindHooks.CollectImpliedVariables(variables, names);

    variables.erase(std::remove_if(variables.begin(), variables.end(), [](const Graph::Variable& variable) {
        return variable.Name.empty();
    }), variables.end());
    if (m_VariablesSortMode == VariablesSortMode::Name)
    {
        std::sort(variables.begin(), variables.end(), [](const Graph::Variable& a, const Graph::Variable& b) {
            const std::string aName = LowerAsciiCopy(a.Name);
            const std::string bName = LowerAsciiCopy(b.Name);
            return aName == bName ? a.Name < b.Name : aName < bName;
        });
    }
    else if (m_VariablesSortMode == VariablesSortMode::Created)
    {
        std::stable_sort(variables.begin(), variables.end(), [](const Graph::Variable& a, const Graph::Variable& b) {
            return a.CreatedOrder < b.CreatedOrder;
        });
    }
    if (!m_VariablesSortAscending)
        std::reverse(variables.begin(), variables.end());
    return variables;
}

void GraphPanel::AddGraphVariable()
{
    std::unordered_set<std::string> existing;
    for (const auto& variable : CollectVariablesForDisplay())
        existing.insert(variable.Name);

    std::string name = "variable";
    for (int i = 1; existing.count(name); ++i)
        name = "variable" + std::to_string(i);

    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    Graph::Variable variable;
    variable.Name = name;
    variable.Type = "float";
    variable.Value = DefaultGraphVariableValue(variable.Type);
    variable.CreatedOrder = NextGraphVariableCreatedOrder();
    m_Model.Variables.push_back(std::move(variable));

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Add Graph Variable");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
}

void GraphPanel::RenameGraphVariable(const std::string& oldName, const std::string& newName)
{
    const std::string cleanOldName = TrimCopy(oldName);
    const std::string cleanNewName = TrimCopy(newName);
    if (cleanOldName.empty() || cleanNewName.empty() || cleanOldName == cleanNewName)
    {
        ScheduleVariablesPanelRebuild();
        return;
    }

    for (const auto& variable : CollectVariablesForDisplay())
    {
        if (variable.Name == cleanNewName && variable.Name != cleanOldName)
        {
            ScheduleVariablesPanelRebuild();
            return;
        }
    }

    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));

    Graph::Variable* variable = EnsureGraphVariable(cleanOldName);
    if (variable)
        variable->Name = cleanNewName;

    for (auto& node : m_Model.Nodes)
    {
        if (NodeReferencesGraphVariable(node, cleanOldName))
            node.Parameters["variableName"] = cleanNewName;
    }

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Rename Graph Variable");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    if (m_Canvas)
        m_Canvas->MarkDirty(UIElement::VisualDirty);
    MarkDirty();
    ScheduleVariablesPanelRebuild();
}

int GraphPanel::CountGraphVariableUsages(const std::string& name) const
{
    const std::string cleanName = TrimCopy(name);
    if (cleanName.empty())
        return 0;

    int usages = 0;
    for (const auto& node : m_Model.Nodes)
    {
        if (NodeReferencesGraphVariable(node, cleanName))
            ++usages;
    }
    return usages;
}

void GraphPanel::EnsureVariableTypeWarningModal()
{
    if (m_VariableTypeWarningModal)
        return;

    auto modal = std::make_unique<ConfirmActionModal>();
    m_VariableTypeWarningModal = modal.get();
    UIManager* ui = GetOwnerManager();
    UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
    if (uiRoot)
        uiRoot->AddChild(std::move(modal));
    else
        AddChild(std::move(modal));
}

void GraphPanel::RequestSetGraphVariableType(const std::string& name, const std::string& type)
{
    const std::string cleanName = TrimCopy(name);
    const std::string normalized = NormalizeGraphVariableType(type);
    if (cleanName.empty())
    {
        ScheduleVariablesPanelRebuild();
        return;
    }

    std::string currentType = "float";
    for (const auto& variable : CollectVariablesForDisplay())
    {
        if (variable.Name == cleanName)
        {
            currentType = NormalizeGraphVariableType(variable.Type);
            break;
        }
    }
    if (currentType == normalized)
        return;

    const int usages = CountGraphVariableUsages(cleanName);
    if (usages <= 0)
    {
        SetGraphVariableType(cleanName, normalized);
        return;
    }

    m_PendingVariableTypeName = cleanName;
    m_PendingVariableTypeValue = normalized;
    EnsureVariableTypeWarningModal();
    if (!m_VariableTypeWarningModal)
    {
        SetGraphVariableType(cleanName, normalized);
        return;
    }

    m_VariableTypeWarningModal->SetOnConfirm([this]()
    {
        const std::string nameToApply = m_PendingVariableTypeName;
        const std::string typeToApply = m_PendingVariableTypeValue;
        m_PendingVariableTypeName.clear();
        m_PendingVariableTypeValue.clear();
        SetGraphVariableType(nameToApply, typeToApply);
    });
    m_VariableTypeWarningModal->SetOnCancel([this]()
    {
        m_PendingVariableTypeName.clear();
        m_PendingVariableTypeValue.clear();
        ScheduleVariablesPanelRebuild();
    });

    const std::string nodeWord = usages == 1 ? "node" : "nodes";
    m_VariableTypeWarningModal->Show(
        "Change Variable Type?",
        "Variable \"" + cleanName + "\" is already used by " + std::to_string(usages) + " " + nodeWord + ".\n"
        "Changing its type may break existing graph logic or connections.",
        "Change Type");
}

void GraphPanel::SetGraphVariableType(const std::string& name, const std::string& type)
{
    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    Graph::Variable* variable = EnsureGraphVariable(name);
    if (!variable)
        return;
    const std::string normalized = NormalizeGraphVariableType(type);
    if (variable->Type == normalized)
        return;
    variable->Type = normalized;
    variable->Value = NormalizeGraphVariableValue(normalized, variable->Value.empty()
        ? DefaultGraphVariableValue(normalized)
        : variable->Value);

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Edit Graph Variable");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
}

bool GraphPanel::ApplyGraphVariableValue(const std::string& name,
                                             const std::string& value,
                                             bool syncPublicMaterials)
{
    Graph::Variable* variable = EnsureGraphVariable(name);
    if (!variable)
        return false;

    const std::string normalized = NormalizeGraphVariableValue(variable->Type, value);
    if (variable->Value == normalized)
        return true;

    variable->Value = normalized;
    m_Dirty = true;
    UpdateTitleLabel();
    if (m_Canvas)
        m_Canvas->MarkDirty(UIElement::VisualDirty);
    if (variable->IsPublic && syncPublicMaterials)
        if (m_KindHooks.OnPublicVariablesEdited)
            m_KindHooks.OnPublicVariablesEdited(false);
    Notify(GraphPanelEvent::VariablesEdited);
    return true;
}

bool GraphPanel::SetGraphVariableValue(const std::string& name, const std::string& value, bool commitUndo)
{
    const std::string cleanName = TrimCopy(name);
    if (cleanName.empty())
        return false;

    if (!m_Undo)
    {
        const bool ok = ApplyGraphVariableValue(cleanName, value, false);
        if (ok && commitUndo)
        {
            if (const Graph::Variable* variable = FindGraphVariable(cleanName))
            {
                if (variable->IsPublic)
                    if (m_KindHooks.OnPublicVariablesEdited)
                        m_KindHooks.OnPublicVariablesEdited(true);
            }
            Notify(GraphPanelEvent::VariablesEdited);
            /* Parameter nodes mirror variable values on the node (Value row /
               swatch); rebind so board edits reflect there immediately. */
            if (m_Canvas)
                m_Canvas->RefreshBoundNodeValues();
        }
        if (commitUndo)
            ScheduleVariablesPanelRebuild();
        return ok;
    }

    const bool sameGesture = m_GraphVariableEdit && m_GraphVariableEditName == cleanName;
    if (!commitUndo)
    {
        if (!sameGesture)
        {
            if (m_GraphVariableEdit)
            {
                const std::string previousEditName = m_GraphVariableEditName;
                m_GraphVariableEdit.Commit();
                if (const Graph::Variable* variable = FindGraphVariable(previousEditName))
                {
                    if (variable->IsPublic)
                        if (m_KindHooks.OnPublicVariablesEdited)
                            m_KindHooks.OnPublicVariablesEdited(true);
                }
                Notify(GraphPanelEvent::VariablesEdited);
                /* Parameter nodes mirror variable values on the node (Value row /
                   swatch); rebind so board edits reflect there immediately. */
                if (m_Canvas)
                    m_Canvas->RefreshBoundNodeValues();
            }
            m_GraphVariableEditName = cleanName;
            m_GraphVariableEdit = BeginGraphVariableEdit("Edit Graph Variable Value");
        }

        if (m_GraphVariableEdit)
        {
            m_GraphVariableEdit.Preview([this, cleanName, value]() {
                (void)ApplyGraphVariableValue(cleanName, value, false);
            });
            return true;
        }
        return ApplyGraphVariableValue(cleanName, value, false);
    }

    if (sameGesture)
    {
        m_GraphVariableEdit.Preview([this, cleanName, value]() {
            (void)ApplyGraphVariableValue(cleanName, value, false);
        });
        m_GraphVariableEdit.Commit();
        m_GraphVariableEdit = {};
        m_GraphVariableEditName.clear();
        ScheduleVariablesPanelRebuild();
        if (const Graph::Variable* variable = FindGraphVariable(cleanName))
        {
            if (variable->IsPublic)
                if (m_KindHooks.OnPublicVariablesEdited)
                    m_KindHooks.OnPublicVariablesEdited(true);
        }
        Notify(GraphPanelEvent::VariablesEdited);
        /* Parameter nodes mirror variable values on the node (Value row /
           swatch); rebind so board edits reflect there immediately. */
        if (m_Canvas)
            m_Canvas->RefreshBoundNodeValues();
        return true;
    }

    std::vector<std::uint8_t> before;
    const bool haveBefore = CaptureGraphSnapshot(before);
    if (!ApplyGraphVariableValue(cleanName, value, false))
        return false;
    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Edit Graph Variable Value");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    ScheduleVariablesPanelRebuild();
    if (const Graph::Variable* variable = FindGraphVariable(cleanName))
    {
        if (variable->IsPublic)
            if (m_KindHooks.OnPublicVariablesEdited)
                m_KindHooks.OnPublicVariablesEdited(true);
    }
    Notify(GraphPanelEvent::VariablesEdited);
    /* Parameter nodes mirror variable values on the node (Value row /
       swatch); rebind so board edits reflect there immediately. */
    if (m_Canvas)
        m_Canvas->RefreshBoundNodeValues();
    return true;
}

void GraphPanel::SetGraphVariablePublic(const std::string& name, bool isPublic)
{
    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    Graph::Variable* variable = EnsureGraphVariable(name);
    if (!variable || variable->IsPublic == isPublic)
        return;
    variable->IsPublic = isPublic;

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Edit Graph Variable");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
    if (m_KindHooks.OnPublicVariablesEdited)
        m_KindHooks.OnPublicVariablesEdited(true);
    Notify(GraphPanelEvent::VariablesEdited);
}

void GraphPanel::SetGraphVariableGlobal(const std::string& name, bool isGlobal)
{
    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    Graph::Variable* variable = EnsureGraphVariable(name);
    if (!variable || variable->IsGlobal == isGlobal)
        return;
    variable->IsGlobal = isGlobal;

    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after) && after != before)
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                              "Edit Graph Variable");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
}

void GraphPanel::RebuildVariablesPanel()
{
    if (!m_VariablesPanel || !m_VariablesContent)
        return;

    m_VariablesPanel->Overrides()
        .Set(Style::Display, m_VariablesPanelVisible ? DisplayMode::Flex : DisplayMode::None);
    if (!m_VariablesPanelVisible)
        return;

    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();

    m_VariablesContent->RemoveAllChildren();

    const std::string search = m_VariablesSearchField ? m_VariablesSearchField->GetValue() : "";
    std::vector<Graph::Variable> variables;
    for (const Graph::Variable& variable : CollectVariablesForDisplay())
    {
        std::string searchableType = variable.Type;
        if (variable.IsPublic)
            searchableType += " public";
        if (variable.IsGlobal)
            searchableType += " global";
        if (!variable.Value.empty())
            searchableType += " " + variable.Value;
        if (MatchSearch(variable.Name, searchableType, search))
            variables.push_back(variable);
    }
    if (variables.empty())
    {
        auto empty = std::make_unique<Label>();
        empty->SetText(search.empty() ? "No variables" : "No matches");
        empty->AddClass("node-graph-variable-empty");
        m_VariablesContent->AddChild(std::move(empty));
    }

    const std::vector<Dropdown::Option> typeOptions = GraphVariableTypeOptions();
    for (size_t variableIndex = 0; variableIndex < variables.size(); ++variableIndex)
    {
        const Graph::Variable& variable = variables[variableIndex];
        auto row = std::make_unique<UIElement>();
        UIElement* rowRaw = row.get();
        row->AddClass("node-graph-variable-row");
        if (m_VariablesSortMode == VariablesSortMode::Custom)
        {
            row->AddClass("node-graph-variable-row-draggable");
            row->SetTooltip("Drag to reorder");
            rowRaw->RegisterEventHandler(kEventMouseDown, [this, rowRaw, name = variable.Name, variableIndex](UIEvent& ev)
            {
                if (ev.Button != 0 || m_VariablesSortMode != VariablesSortMode::Custom)
                    return;
                m_VariableRowDragPending = true;
                m_DraggingVariableRow = false;
                m_VariableRowDragName = name;
                m_VariableRowDragStartY = ev.Y;
                m_VariableRowDragTargetIndex = variableIndex;
                rowRaw->AddClass("dragging");
                ev.Capture(rowRaw);
                ev.Stop();
            });
            rowRaw->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev)
            {
                if (!m_VariableRowDragPending && !m_DraggingVariableRow)
                    return;

                constexpr float kDragThresholdPx = 4.f;
                const float dy = ev.Y - m_VariableRowDragStartY;
                if (!m_DraggingVariableRow && std::fabs(dy) <= kDragThresholdPx)
                {
                    ev.Stop();
                    return;
                }
                m_DraggingVariableRow = true;

                size_t visibleRows = 1;
                if (m_VariablesContent)
                    visibleRows = std::max<size_t>(1, m_VariablesContent->GetChildren().size());
                float localY = ev.Y;
                if (m_VariablesContent)
                    localY -= m_VariablesContent->GetLayoutY();
                int target = static_cast<int>(std::floor(localY / kVariableRowHeight));
                target = std::clamp(target, 0, static_cast<int>(visibleRows) - 1);
                m_VariableRowDragTargetIndex = static_cast<size_t>(target);
                ev.Stop();
            });
            rowRaw->RegisterEventHandler(kEventMouseUp, [this, rowRaw](UIEvent& ev)
            {
                if (ev.Button != 0 || (!m_VariableRowDragPending && !m_DraggingVariableRow))
                    return;
                const bool shouldReorder = m_DraggingVariableRow;
                const std::string name = m_VariableRowDragName;
                const size_t targetIndex = m_VariableRowDragTargetIndex;
                m_VariableRowDragPending = false;
                m_DraggingVariableRow = false;
                m_VariableRowDragName.clear();
                rowRaw->RemoveClass("dragging");
                if (shouldReorder)
                    ReorderGraphVariable(name, targetIndex);
                ev.Stop();
            });
        }

        auto nameField = std::make_unique<TextField>();
        nameField->SetValue(variable.Name);
        nameField->AddClass("node-graph-variable-name");
        nameField->SetTooltip("Variable name");
        nameField->SetOnValueChanged([this, oldName = variable.Name](const std::string& value) {
            RenameGraphVariable(oldName, value);
        });
        row->AddChild(std::move(nameField));

        auto valueGroup = std::make_unique<UIElement>();
        valueGroup->AddClass("node-graph-variable-value-group");

        const std::string variableType = NormalizeGraphVariableType(variable.Type);
        const std::string variableValue = NormalizeGraphVariableValue(variableType, variable.Value);
        if (variableType == "bool")
        {
            auto toggle = std::make_unique<Toggle>();
            toggle->AddClass("node-graph-variable-value-toggle");
            toggle->SetTooltip("Variable value");
            toggle->SetChecked(variableValue == "true");
            toggle->SetOnValueChanged([this, name = variable.Name](const bool& value) {
                SetGraphVariableValue(name, value ? "true" : "false", true);
            });
            valueGroup->AddChild(std::move(toggle));
        }
        else if (variableType == "int")
        {
            int parsed = 0;
            (void)IntField::TryParseInt(variableValue, parsed);
            valueGroup->AddClass("node-graph-variable-value-group-draggable");
            auto field = std::make_unique<VariableIntField>();
            field->AddClass("node-graph-variable-value-field");
            field->AddClass("node-graph-variable-value-field-draggable");
            field->SetTooltip("Variable value");
            field->SetValue(parsed);
            field->SetOnValueChanging([this, name = variable.Name](const int& value) {
                SetGraphVariableValue(name, std::to_string(value), false);
            });
            field->SetOnValueChanged([this, name = variable.Name](const int& value) {
                SetGraphVariableValue(name, std::to_string(value), true);
            });
            valueGroup->AddChild(std::move(field));
        }
        else if (variableType == "float")
        {
            float parsed = 0.f;
            (void)FloatField::TryParseFloat(variableValue, parsed);
            valueGroup->AddClass("node-graph-variable-value-group-draggable");
            auto field = std::make_unique<VariableFloatField>();
            field->AddClass("node-graph-variable-value-field");
            field->AddClass("node-graph-variable-value-field-draggable");
            field->SetTooltip("Variable value");
            field->SetValue(parsed);
            field->SetOnValueChanging([this, name = variable.Name](const float& value) {
                SetGraphVariableValue(name, FormatGraphVariableFloat(value), false);
            });
            field->SetOnValueChanged([this, name = variable.Name](const float& value) {
                SetGraphVariableValue(name, FormatGraphVariableFloat(value), true);
            });
            valueGroup->AddChild(std::move(field));
        }
        else if (variableType == "float3" || variableType == "float4")
        {
            const int componentCount = variableType == "float3" ? 3 : 4;
            const std::vector<float> components = ParseGraphVariableVectorValue(variableValue, componentCount);
            const float r = components[0];
            const float g = components[1];
            const float b = components[2];
            const float intensity = std::max(1.0f, std::max(r, std::max(g, b)));
            const uint32_t argb = GraphVariableVectorToArgb(components, componentCount);

            valueGroup->AddClass("node-graph-variable-value-group-color");

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            swatch->AddClass("node-graph-variable-color-swatch");
            StyleGraphVariableColorSwatch(swatchRaw, argb);
            swatch->SetTooltip(variableValue);
            valueGroup->AddChild(std::move(swatch));

            auto applyColor = [this, name = variable.Name, variableType, componentCount, swatchRaw](
                                   uint32_t newArgb, float newIntensity, bool commitUndo) {
                float nr = 0.f;
                float ng = 0.f;
                float nb = 0.f;
                ArgbIntensityToGraphVariableVector(newArgb, newIntensity, nr, ng, nb);
                std::vector<float> values = {nr, ng, nb};
                if (componentCount > 3)
                {
                    float preservedAlpha = 1.0f;
                    if (Graph::Variable* variable = EnsureGraphVariable(name))
                    {
                        const std::vector<float> current = ParseGraphVariableVectorValue(
                            NormalizeGraphVariableValue(variableType, variable->Value), componentCount);
                        preservedAlpha = current[3];
                    }
                    values.push_back(preservedAlpha);
                }
                const std::string formatted = FormatGraphVariableVectorValue(values, componentCount);
                StyleGraphVariableColorSwatch(swatchRaw, GraphVariableVectorToArgb(values, componentCount));
                if (swatchRaw)
                    swatchRaw->SetTooltip(formatted);
                SetGraphVariableValue(name, formatted, commitUndo);
            };

            auto clickHandler = [this, argb, intensity, applyColor](UIEvent& e) {
                if (e.Button != 0)
                    return;
                e.Stop();
                if (!m_OpenColorPickerWindow)
                    return;
                ColorPickerCallbacks cbs;
                cbs.onApply = [applyColor](uint32_t newArgb, float newIntensity) {
                    applyColor(newArgb, newIntensity, true);
                };
                cbs.onCancel = [applyColor, argb, intensity]() {
                    applyColor(argb, intensity, true);
                };
                cbs.onValueChanging = [applyColor](uint32_t newArgb, float newIntensity) {
                    applyColor(newArgb, newIntensity, false);
                };
                m_OpenColorPickerWindow(argb, intensity, std::move(cbs));
            };
            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
        }
        else
        {
            auto field = std::make_unique<TextField>();
            field->AddClass("node-graph-variable-value-field");
            field->SetTooltip(variableType == "string" ? "Variable value" : "Comma-separated vector value");
            field->SetValue(variableValue);
            field->SetOnValueChanged([this, name = variable.Name](const std::string& value) {
                SetGraphVariableValue(name, value, true);
            });
            valueGroup->AddChild(std::move(field));
        }
        row->AddChild(std::move(valueGroup));

        auto typeDropdown = std::make_unique<Dropdown>();
        typeDropdown->AddClass("node-graph-variable-type");
        typeDropdown->AddClass("inspector-dropdown");
        typeDropdown->SetTooltip("Variable type");
        typeDropdown->SetOptions(typeOptions, GraphVariableTypeIndex(variable.Type));
        typeDropdown->SetOnValueChanged([this, name = variable.Name](const std::string& value) {
            RequestSetGraphVariableType(name, value);
        });
        row->AddChild(std::move(typeDropdown));

        auto publicSpacer = std::make_unique<UIElement>();
        publicSpacer->AddClass("node-graph-variable-row-spacer");
        row->AddChild(std::move(publicSpacer));

        auto publicGroup = std::make_unique<UIElement>();
        publicGroup->AddClass("node-graph-variable-toggle-group");
        auto publicLabel = std::make_unique<Label>();
        publicLabel->SetText("Public");
        publicLabel->AddClass("node-graph-variable-toggle-label");
        publicGroup->AddChild(std::move(publicLabel));
        auto publicToggle = std::make_unique<Toggle>();
        publicToggle->AddClass("node-graph-variable-toggle");
        publicToggle->SetTooltip("Expose this variable outside the graph");
        publicToggle->SetChecked(variable.IsPublic);
        publicToggle->SetOnValueChanged([this, name = variable.Name](const bool& value) {
            SetGraphVariablePublic(name, value);
        });
        publicGroup->AddChild(std::move(publicToggle));
        row->AddChild(std::move(publicGroup));

        if (m_KindHooks.ShowsGlobalVariableToggle)
        {
            auto globalGroup = std::make_unique<UIElement>();
            globalGroup->AddClass("node-graph-variable-toggle-group");
            auto globalLabel = std::make_unique<Label>();
            globalLabel->SetText("Global");
            globalLabel->AddClass("node-graph-variable-toggle-label");
            globalGroup->AddChild(std::move(globalLabel));
            auto globalToggle = std::make_unique<Toggle>();
            globalToggle->AddClass("node-graph-variable-toggle");
            globalToggle->SetTooltip("Share this variable globally");
            globalToggle->SetChecked(variable.IsGlobal);
            globalToggle->SetOnValueChanged([this, name = variable.Name](const bool& value) {
                SetGraphVariableGlobal(name, value);
            });
            globalGroup->AddChild(std::move(globalToggle));
            row->AddChild(std::move(globalGroup));
        }

        m_VariablesContent->AddChild(std::move(row));
    }

    if (m_VariablesScroll)
        m_VariablesScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_VariablesPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::ScheduleVariablesPanelRebuild()
{
    if (m_VariablesRebuildScheduled)
        return;
    m_VariablesRebuildScheduled = true;
    Scheduler::IScheduler* sched = GetScheduler();
    if (!sched)
    {
        m_VariablesRebuildScheduled = false;
        if (m_VariablesPanelVisible)
        {
            RebuildVariablesPanel();
            ApplyVariablesPanelBounds();
        }
        return;
    }
    sched->ScheduleNext([this]()
    {
        m_VariablesRebuildScheduled = false;
        RebuildVariablesPanel();
        if (m_VariablesPanelVisible)
        {
            ApplyVariablesPanelBounds();
            RequestRelayout();
        }
    });
}

void GraphPanel::AddNodeFromPalette(const std::string& typeId)
{
    if (!m_Canvas)
        return;

    float canvasX = 0.f;
    float canvasY = 0.f;
    float canvasW = 0.f;
    float canvasH = 0.f;
    m_Canvas->GetHitTestBounds(canvasX, canvasY, canvasW, canvasH);
    if (canvasW <= 1.f || canvasH <= 1.f)
        return;

    float panX = 0.f;
    float panY = 0.f;
    float zoom = 1.f;
    m_Canvas->GetPanZoom(panX, panY, zoom);
    zoom = std::max(0.2f, zoom);

    const float nodeW = GraphCanvas::kNodeWidth * zoom;
    const float nodeH = GraphCanvas::kNodeHeight * zoom;
    constexpr float margin = 24.f;

    float targetX = canvasX + canvasW * 0.5f - nodeW * 0.5f;
    float targetY = canvasY + canvasH * 0.5f - nodeH * 0.5f;

    if (m_Palette)
    {
        const float paletteX = m_Palette->GetLayoutX();
        const float paletteY = m_Palette->GetLayoutY();
        const float paletteW = m_Palette->GetLayoutWidth();
        const float paletteH = m_Palette->GetLayoutHeight();

        auto overlapsPalette = [&](float x, float y) {
            return x < paletteX + paletteW && x + nodeW > paletteX &&
                   y < paletteY + paletteH && y + nodeH > paletteY;
        };
        auto insideCanvas = [&](float x, float y) {
            return x >= canvasX + margin && y >= canvasY + margin &&
                   x + nodeW <= canvasX + canvasW - margin &&
                   y + nodeH <= canvasY + canvasH - margin;
        };

        const std::array<Mathematics::Vector2, 5> candidates = {{
            {paletteX + paletteW + margin, std::max(canvasY + margin, paletteY + 40.f)},
            {paletteX - nodeW - margin, std::max(canvasY + margin, paletteY + 40.f)},
            {paletteX, paletteY + paletteH + margin},
            {paletteX, paletteY - nodeH - margin},
            {targetX, targetY},
        }};
        for (const Mathematics::Vector2& candidate : candidates)
        {
            if (insideCanvas(candidate.x, candidate.y) && !overlapsPalette(candidate.x, candidate.y))
            {
                targetX = candidate.x;
                targetY = candidate.y;
                break;
            }
        }
    }

    targetX = std::clamp(targetX, canvasX + margin, std::max(canvasX + margin, canvasX + canvasW - nodeW - margin));
    targetY = std::clamp(targetY, canvasY + margin, std::max(canvasY + margin, canvasY + canvasH - nodeH - margin));
    AddNodeFromPaletteAtScreen(typeId, targetX, targetY, false, false);
}

void GraphPanel::AddNodeFromPaletteAtScreen(const std::string& typeId, float screenX, float screenY,
                                                bool centerOnPointer, bool rejectPaletteArea)
{
    if (!m_Canvas || !m_Canvas->GetModel())
        return;

    float canvasX = 0.f;
    float canvasY = 0.f;
    float canvasW = 0.f;
    float canvasH = 0.f;
    m_Canvas->GetHitTestBounds(canvasX, canvasY, canvasW, canvasH);
    if (canvasW <= 1.f || canvasH <= 1.f)
        return;

    if (screenX < canvasX || screenY < canvasY || screenX > canvasX + canvasW || screenY > canvasY + canvasH)
        return;

    if (rejectPaletteArea && m_Palette)
    {
        const float paletteX = m_Palette->GetLayoutX();
        const float paletteY = m_Palette->GetLayoutY();
        const float paletteW = m_Palette->GetLayoutWidth();
        const float paletteH = m_Palette->GetLayoutHeight();
        if (screenX >= paletteX && screenY >= paletteY && screenX <= paletteX + paletteW && screenY <= paletteY + paletteH)
            return;
    }

    float panX = 0.f;
    float panY = 0.f;
    float zoom = 1.f;
    m_Canvas->GetPanZoom(panX, panY, zoom);
    zoom = std::max(0.2f, zoom);

    if (centerOnPointer)
    {
        screenX -= GraphCanvas::kNodeWidth * zoom * 0.5f;
        screenY -= GraphCanvas::kNodeHeight * zoom * 0.5f;
    }

    Graph::Model* model = m_Canvas->GetModel();
    std::vector<std::uint8_t> before;
    bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    std::string nodeId = model->GenerateNodeId();
    float px = (screenX - canvasX - panX) / zoom;
    float py = (screenY - canvasY - panY) / zoom;
    GraphCanvas::SnapGraphPosition(px, py);
    Graph::Node node = GraphNodeRegistry::Get().CreateNode(model->KindId, typeId, nodeId, px, py);
    if (m_Canvas)
    {
        float resolvedX = px;
        float resolvedY = py;
        m_Canvas->FindNearestNonOverlappingPosition(node, px, py, {}, resolvedX, resolvedY);
        node.PositionX = resolvedX;
        node.PositionY = resolvedY;
    }
    model->Nodes.push_back(std::move(node));
    m_Canvas->SetSelectedNodeId(nodeId);
    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();
    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after))
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before),
                                                              std::move(after), "Add Node");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
    m_Canvas->RebuildNodeWidgets();
    m_Canvas->MarkDirty(UIElement::VisualDirty);
}

void GraphPanel::AddNodeFromWireDropAtScreen(const std::string& anchorNodeId, const std::string& anchorPortId,
                                                 const std::string& typeId, const std::string& connectPortId,
                                                 float screenX, float screenY, bool fromInputPort)
{
    if (!m_Canvas || !m_Canvas->GetModel())
        return;

    Graph::Model* model = m_Canvas->GetModel();
    const Graph::Node* anchorNode = model->FindNode(anchorNodeId);
    const Graph::Port* anchorPort = nullptr;
    if (anchorNode)
    {
        for (const auto& port : anchorNode->Ports)
        {
            if (port.Id == anchorPortId)
            {
                anchorPort = &port;
                break;
            }
        }
    }
    if (!anchorNode || !anchorPort)
        return;
    if (fromInputPort)
    {
        if (anchorPort->Direction != Graph::PortDirection::In)
            return;
    }
    else if (anchorPort->Direction != Graph::PortDirection::Out)
    {
        return;
    }

    float canvasX = 0.f;
    float canvasY = 0.f;
    float canvasW = 0.f;
    float canvasH = 0.f;
    m_Canvas->GetHitTestBounds(canvasX, canvasY, canvasW, canvasH);
    if (canvasW <= 1.f || canvasH <= 1.f)
        return;
    if (screenX < canvasX || screenY < canvasY || screenX > canvasX + canvasW || screenY > canvasY + canvasH)
        return;

    float panX = 0.f;
    float panY = 0.f;
    float zoom = 1.f;
    m_Canvas->GetPanZoom(panX, panY, zoom);
    zoom = std::max(0.2f, zoom);

    screenX -= GraphCanvas::kNodeWidth * zoom * 0.5f;
    screenY -= GraphCanvas::kNodeHeight * zoom * 0.5f;

    std::vector<std::uint8_t> before;
    const bool haveBefore = (m_Undo && CaptureGraphSnapshot(before));
    const std::string nodeId = model->GenerateNodeId();
    float px = (screenX - canvasX - panX) / zoom;
    float py = (screenY - canvasY - panY) / zoom;
    GraphCanvas::SnapGraphPosition(px, py);

    Graph::Node node = GraphNodeRegistry::Get().CreateNode(model->KindId, typeId, nodeId, px, py);
    const Graph::Port* newConnectPort = nullptr;
    for (const auto& port : node.Ports)
    {
        if (port.Id != connectPortId)
            continue;
        if (fromInputPort)
        {
            if (port.Direction != Graph::PortDirection::Out ||
                !AreNodeGraphPortTypesCompatible(port.DataType, anchorPort->DataType))
                continue;
        }
        else if (port.Direction != Graph::PortDirection::In ||
                 !AreNodeGraphPortTypesCompatible(anchorPort->DataType, port.DataType))
        {
            continue;
        }
        newConnectPort = &port;
        break;
    }
    if (!newConnectPort)
        return;

    float resolvedX = px;
    float resolvedY = py;
    m_Canvas->FindNearestNonOverlappingPosition(node, px, py, {}, resolvedX, resolvedY);
    node.PositionX = resolvedX;
    node.PositionY = resolvedY;
    model->Nodes.push_back(std::move(node));

    if (fromInputPort)
    {
        model->Links.erase(std::remove_if(model->Links.begin(), model->Links.end(),
                                          [&](const Graph::Edge& existing) {
                                              return existing.TargetNodeId == anchorNodeId &&
                                                     existing.TargetPortId == anchorPortId;
                                          }),
                           model->Links.end());
    }

    Graph::Edge link;
    link.Id = model->GenerateLinkId();
    if (fromInputPort)
    {
        link.SourceNodeId = nodeId;
        link.SourcePortId = connectPortId;
        link.TargetNodeId = anchorNodeId;
        link.TargetPortId = anchorPortId;
    }
    else
    {
        link.SourceNodeId = anchorNodeId;
        link.SourcePortId = anchorPortId;
        link.TargetNodeId = nodeId;
        link.TargetPortId = connectPortId;
    }
    model->Links.push_back(std::move(link));

    m_Canvas->SetSelectedNodeId(nodeId);
    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();
    if (haveBefore)
    {
        std::vector<std::uint8_t> after;
        if (CaptureGraphSnapshot(after))
        {
            auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before),
                                                              std::move(after), "Add Connected Node");
            m_Undo->CommitAlreadyApplied(std::move(cmd));
        }
    }
    MarkDirty();
    ScheduleVariablesPanelRebuild();
    m_Canvas->RebuildNodeWidgets();
    m_Canvas->MarkDirty(UIElement::VisualDirty);
}

void GraphPanel::ShowPaletteNodeDragGhost(const std::string& typeId, float screenX, float screenY)
{
    UIElement* bodyEl = m_Body ? m_Body : (m_Palette ? m_Palette->GetParent() : nullptr);
    if (!bodyEl)
        return;

    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(m_Model.KindId, typeId);
    const std::string displayName = meta ? meta->DisplayName : typeId;
    const std::string valueSummary = (displayName != typeId) ? typeId : std::string();

    if (!m_PaletteNodeDragGhost)
    {
        auto ghost = std::make_unique<UIElement>();
        ghost->SetId("NodeGraphPaletteNodeDragGhost");
        ghost->AddClass("node-graph-palette-node-drag-ghost");
        ghost->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::JustifyContent, JustifyContent::Center)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::Width, StyleLength::Px(GraphCanvas::kNodeWidth))
            .Set(Style::Height, StyleLength::Px(GraphCanvas::kNodeHeight))
            .Set(Style::Opacity, 0.82f)
            .Set(Style::ZIndex, 30)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.f, 4.f, 4.f, 4.f})
            .Set(Style::BorderWidth, Box4{1.f, 1.f, 1.f, 1.f})
            .Set(Style::BorderColor, BorderColorsTRBL{0xFF3A8FFFu, 0xFF3A8FFFu, 0xFF3A8FFFu, 0xFF3A8FFFu});

        auto label = std::make_unique<Label>();
        label->AddClass("node-graph-palette-node-drag-ghost-label");
        label->SetText(displayName);
        label->Overrides()
            .Set(Style::FontSize, StyleLength::Px(13.f))
            .Set(Style::Color, 0xFFFFFFFFu)
            .Set(Style::FlexShrink, 1.f)
            .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap);
        m_PaletteNodeDragGhostLabel = label.get();
        ghost->AddChild(std::move(label));

        auto valueLabel = std::make_unique<Label>();
        valueLabel->AddClass("node-graph-palette-node-drag-ghost-value");
        valueLabel->SetText(valueSummary);
        valueLabel->Overrides()
            .Set(Style::FontSize, StyleLength::Px(11.f))
            .Set(Style::Color, 0xFFCFCFCFu)
            .Set(Style::FlexShrink, 1.f)
            .Set(Style::WhiteSpaceProp, WhiteSpace::NoWrap)
            .Set(Style::Display, valueSummary.empty() ? DisplayMode::None : DisplayMode::Flex);
        m_PaletteNodeDragGhostValueLabel = valueLabel.get();
        ghost->AddChild(std::move(valueLabel));

        m_PaletteNodeDragGhost = ghost.get();
        bodyEl->AddChild(std::move(ghost));
    }
    else
    {
        m_PaletteNodeDragGhost->Overrides().Set(Style::Display, DisplayMode::Flex);
        if (m_PaletteNodeDragGhostLabel)
            m_PaletteNodeDragGhostLabel->SetText(displayName);
        if (m_PaletteNodeDragGhostValueLabel)
        {
            m_PaletteNodeDragGhostValueLabel->SetText(valueSummary);
            m_PaletteNodeDragGhostValueLabel->Overrides()
                .Set(Style::Display, valueSummary.empty() ? DisplayMode::None : DisplayMode::Flex);
        }
    }

    const uint32_t nodeColor = NodeColorSettings::GetNodeColorArgb(m_Model.KindId, typeId);
    m_PaletteNodeDragGhost->Overrides().Set(Style::BackgroundColor, WithAlpha(nodeColor, 0xDDu));
    UpdatePaletteNodeDragGhost(screenX, screenY);
}

void GraphPanel::UpdatePaletteNodeDragGhost(float screenX, float screenY)
{
    if (!m_PaletteNodeDragGhost)
        return;

    UIElement* bodyEl = m_PaletteNodeDragGhost->GetParent();
    if (!bodyEl)
        return;

    const float bodyX = bodyEl->GetLayoutX();
    const float bodyY = bodyEl->GetLayoutY();
    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    constexpr float kCursorOffsetX = 18.f;
    bool overCanvas = false;
    float canvasX = 0.f;
    float canvasY = 0.f;
    float canvasW = 0.f;
    float canvasH = 0.f;
    if (m_Canvas)
    {
        m_Canvas->GetHitTestBounds(canvasX, canvasY, canvasW, canvasH);
        overCanvas = canvasW > 1.f && canvasH > 1.f &&
                     screenX >= canvasX && screenY >= canvasY &&
                     screenX <= canvasX + canvasW && screenY <= canvasY + canvasH;
    }

    float zoom = 1.f;
    float panX = 0.f;
    float panY = 0.f;
    if (overCanvas && m_Canvas)
    {
        m_Canvas->GetPanZoom(panX, panY, zoom);
        zoom = std::max(0.2f, zoom);
    }

    const float ghostW = GraphCanvas::kNodeWidth * zoom;
    const float ghostH = GraphCanvas::kNodeHeight * zoom;
    const float fontSize = std::max(4.f, 13.f * zoom);
    const float valueFontSize = std::max(3.5f, 11.f * zoom);
    const float nameRowH = std::max(4.f, 24.f * zoom);
    const float valueRowH = std::max(3.5f, 18.f * zoom);

    float left = overCanvas ? (screenX - bodyX - ghostW * 0.5f)
                            : (screenX - bodyX + kCursorOffsetX);
    float top = screenY - bodyY - ghostH * 0.5f;
    if (overCanvas)
    {
        float graphX = (bodyX + left - canvasX - panX) / zoom;
        float graphY = (bodyY + top - canvasY - panY) / zoom;
        GraphCanvas::SnapGraphPosition(graphX, graphY);
        if (m_Canvas && !m_PaletteNodeDragTypeId.empty())
        {
            Graph::Node previewNode = GraphNodeRegistry::Get().CreateNode(m_Model.KindId, m_PaletteNodeDragTypeId,
                                                                       "__palette_preview__", graphX, graphY);
            m_Canvas->FindNearestNonOverlappingPosition(previewNode, graphX, graphY, {}, graphX, graphY);
        }
        left = canvasX + panX + graphX * zoom - bodyX;
        top = canvasY + panY + graphY * zoom - bodyY;
    }
    else if (bodyW > 1.f)
    {
        left = std::clamp(left, 0.f, std::max(0.f, bodyW - ghostW));
        if (bodyH > 1.f)
            top = std::clamp(top, 0.f, std::max(0.f, bodyH - ghostH));
    }

    const float roundedLeft = std::round(left);
    const float roundedTop = std::round(top);
    const float absoluteX = bodyX + roundedLeft;
    const float absoluteY = bodyY + roundedTop;
    m_PaletteNodeDragGhost->Overrides()
        .Set(Style::Width, StyleLength::Px(std::round(ghostW)))
        .Set(Style::Height, StyleLength::Px(std::round(ghostH)))
        .Set(Style::PositionLeft, StyleLength::Px(roundedLeft))
        .Set(Style::PositionTop, StyleLength::Px(roundedTop));
    UILayoutAccess::SetLastLayoutRect(*m_PaletteNodeDragGhost, absoluteX, absoluteY,
                                              std::round(ghostW), std::round(ghostH));
    if (m_PaletteNodeDragGhostLabel)
    {
        m_PaletteNodeDragGhostLabel->Overrides()
            .Set(Style::FontSize, StyleLength::Px(fontSize))
            .Set(Style::Width, StyleLength::Px(std::round(ghostW)))
            .Set(Style::Height, StyleLength::Px(std::round(nameRowH)));
        UILayoutAccess::SetLastLayoutRect(*m_PaletteNodeDragGhostLabel, absoluteX, absoluteY + std::round(8.f * zoom),
                                                       std::round(ghostW), std::round(nameRowH));
    }
    if (m_PaletteNodeDragGhostValueLabel)
    {
        m_PaletteNodeDragGhostValueLabel->Overrides()
            .Set(Style::FontSize, StyleLength::Px(valueFontSize))
            .Set(Style::Width, StyleLength::Px(std::round(ghostW)))
            .Set(Style::Height, StyleLength::Px(std::round(valueRowH)));
        UILayoutAccess::SetLastLayoutRect(*m_PaletteNodeDragGhostValueLabel, absoluteX, absoluteY + std::round(34.f * zoom),
                                                            std::round(ghostW), std::round(valueRowH));
    }

    const float dx = absoluteX - m_PaletteNodeDragGhost->GetLayoutX();
    const float dy = absoluteY - m_PaletteNodeDragGhost->GetLayoutY();
    ShiftLayoutSubtree(m_PaletteNodeDragGhost, dx, dy);
    m_PaletteNodeDragGhost->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::HidePaletteNodeDragGhost()
{
    if (!m_PaletteNodeDragGhost)
        return;
    if (UIElement* parent = m_PaletteNodeDragGhost->GetParent())
        parent->RemoveChild(m_PaletteNodeDragGhost);
    m_PaletteNodeDragGhost = nullptr;
    m_PaletteNodeDragGhostLabel = nullptr;
    m_PaletteNodeDragGhostValueLabel = nullptr;
}

void GraphPanel::ApplyNodePaletteDrag(float x, float y)
{
    if (!m_Palette)
        return;

    if (m_PaletteDragPending && !m_DraggingPalette)
    {
        constexpr float kDragThresholdPx = 4.f;
        const float dx = x - m_PaletteDragStartX;
        const float dy = y - m_PaletteDragStartY;
        if (dx * dx + dy * dy <= kDragThresholdPx * kDragThresholdPx)
            return;
        m_PaletteDragPending = false;
        m_DraggingPalette = true;
    }

    if (!m_DraggingPalette)
        return;

    UIElement* bodyEl = m_Palette->GetParent();
    if (!bodyEl)
        return;

    constexpr float kPaletteVisibleGrabArea = 48.f;
    constexpr int kPaletteDragZIndex = 10;
    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();

    float newLeft = m_PaletteDragStartLeft + (x - m_PaletteDragStartX);
    float newTop = m_PaletteDragStartTop + (y - m_PaletteDragStartY);
    const float maxLeft = std::max(kFloatingPanelMargin, bodyW - kNodePaletteWidth - kFloatingPanelMargin);
    const float maxTop = std::max(kFloatingPanelMargin, bodyH - kPaletteVisibleGrabArea);
    newLeft = std::clamp(newLeft, kFloatingPanelMargin, maxLeft);
    newTop = std::clamp(newTop, kFloatingPanelMargin, maxTop);
    const int leftPx = static_cast<int>(std::round(newLeft));
    const int topPx = static_cast<int>(std::round(newTop));

    m_Palette->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(static_cast<float>(leftPx)))
        .Set(Style::PositionTop, StyleLength::Px(static_cast<float>(topPx)))
        .Set(Style::Width, StyleLength::Px(kNodePaletteWidth))
        .Set(Style::ZIndex, kPaletteDragZIndex);

    m_Palette->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_Palette->RequestRelayout();
}

void GraphPanel::BeginNodePaletteResize(float x, float y)
{
    (void)x;
    if (!m_Palette || m_PaletteCollapsed)
        return;

    m_PaletteDragPending = false;
    m_DraggingPalette = false;
    m_ResizingPalettePanel = true;
    m_PaletteResizeStartY = y;
    m_PaletteResizeStartHeight = m_PaletteUserHeight > 0.f
                                     ? m_PaletteUserHeight
                                     : std::max(m_Palette->GetLayoutHeight(), kNodePaletteMinHeight);
    if (m_PaletteResizeHandle && !m_PaletteResizeHandle->HasClass("dragging"))
        m_PaletteResizeHandle->AddClass("dragging");
}

void GraphPanel::UpdateNodePaletteResize(float x, float y)
{
    (void)x;
    if (!m_ResizingPalettePanel || !m_Palette)
        return;

    UIElement* bodyEl = m_Palette->GetParent();
    if (!bodyEl)
        return;

    const float bodyH = bodyEl->GetLayoutHeight();
    const float top = m_Palette->GetLayoutY() - bodyEl->GetLayoutY();
    if (bodyH <= 1.f)
        return;

    const float maxHeight = std::max(kNodePaletteMinHeight,
                                     std::min(bodyH * kFloatingPanelMaxBodyRatio,
                                              bodyH - top - kFloatingPanelMargin));
    const float rawHeight = m_PaletteResizeStartHeight + (y - m_PaletteResizeStartY);
    m_PaletteUserHeight = std::clamp(rawHeight, kNodePaletteMinHeight, maxHeight);
    ApplyNodePaletteBounds();
    MarkPanelToggleDirty(m_Palette, m_Body, m_PaletteScroll);
    UpdatePanelOverflowIndicators();
}

void GraphPanel::EndNodePaletteResize()
{
    if (!m_ResizingPalettePanel)
        return;
    m_ResizingPalettePanel = false;
    if (m_PaletteResizeHandle)
        m_PaletteResizeHandle->RemoveClass("dragging");
    if (m_Palette)
        m_Palette->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::ApplyNodePaletteCollapsedState()
{
    if (!m_Palette)
        return;

    m_PaletteDragPending = false;
    m_DraggingPalette = false;
    EndNodePaletteResize();
    m_PaletteTitleLastClickTime = {};

    if (m_PaletteCollapsed)
    {
        m_Palette->AddClass("node-graph-palette-collapsed");
        m_Palette->Overrides()
            .Set(Style::Height, StyleLength::Px(kNodePaletteCollapsedHeight))
            .Set(Style::MaxHeight, StyleLength::Px(kNodePaletteCollapsedHeight));
        UILayoutAccess::SetLastLayoutRect(*m_Palette, m_Palette->GetLayoutX(), m_Palette->GetLayoutY(),
                                     m_Palette->GetLayoutWidth(), kNodePaletteCollapsedHeight);
        m_PaletteHeightOverrideApplied = false;
        m_LastAppliedPaletteHeight = -1.f;
    }
    else
    {
        m_Palette->RemoveClass("node-graph-palette-collapsed");
        m_PaletteHeightOverrideApplied = false;
        m_LastAppliedPaletteHeight = -1.f;
        ApplyNodePaletteBounds();
    }

    MarkLayoutVisualDirtySubtree(m_Palette);
    if (m_Body)
        m_Body->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_Palette->RequestRelayout();
    RequestRelayout();
}

void GraphPanel::ApplyVariablesPanelDrag(float x, float y)
{
    if (!m_VariablesPanel)
        return;

    if (m_VariablesDragPending && !m_DraggingVariablesPanel)
    {
        constexpr float kDragThresholdPx = 4.f;
        const float dx = x - m_VariablesDragStartX;
        const float dy = y - m_VariablesDragStartY;
        if (dx * dx + dy * dy <= kDragThresholdPx * kDragThresholdPx)
            return;
        m_VariablesDragPending = false;
        m_DraggingVariablesPanel = true;
    }

    if (!m_DraggingVariablesPanel)
        return;

    UIElement* bodyEl = m_VariablesPanel->GetParent();
    if (!bodyEl)
        return;

    constexpr float kVisibleGrabArea = 48.f;
    constexpr int kDragZIndex = 9;
    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();

    float newLeft = m_VariablesDragStartLeft + (x - m_VariablesDragStartX);
    float newTop = m_VariablesDragStartTop + (y - m_VariablesDragStartY);
    const float maxLeft = std::max(kFloatingPanelMargin, bodyW - kVariablesPanelWidth - kFloatingPanelMargin);
    const float maxTop = std::max(kFloatingPanelMargin, bodyH - kVisibleGrabArea);
    newLeft = std::clamp(newLeft, kFloatingPanelMargin, maxLeft);
    newTop = std::clamp(newTop, kFloatingPanelMargin, maxTop);

    m_VariablesPanel->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(std::round(newLeft)))
        .Set(Style::PositionTop, StyleLength::Px(std::round(newTop)))
        .Set(Style::Width, StyleLength::Px(kVariablesPanelWidth))
        .Set(Style::ZIndex, kDragZIndex);

    m_VariablesPanel->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_VariablesPanel->RequestRelayout();
}

void GraphPanel::BeginVariablesPanelResize(float x, float y)
{
    (void)x;
    if (!m_VariablesPanel || m_VariablesCollapsed)
        return;

    m_VariablesDragPending = false;
    m_DraggingVariablesPanel = false;
    m_ResizingVariablesPanel = true;
    m_VariablesResizeStartY = y;
    m_VariablesResizeStartHeight = m_VariablesUserHeight > 0.f
                                      ? m_VariablesUserHeight
                                      : std::max(m_VariablesPanel->GetLayoutHeight(), kVariablesPanelMinHeight);
    if (m_VariablesResizeHandle && !m_VariablesResizeHandle->HasClass("dragging"))
        m_VariablesResizeHandle->AddClass("dragging");
}

void GraphPanel::UpdateVariablesPanelResize(float x, float y)
{
    (void)x;
    if (!m_ResizingVariablesPanel || !m_VariablesPanel)
        return;

    UIElement* bodyEl = m_VariablesPanel->GetParent();
    if (!bodyEl)
        return;

    const float bodyH = bodyEl->GetLayoutHeight();
    const float top = m_VariablesPanel->GetLayoutY() - bodyEl->GetLayoutY();
    if (bodyH <= 1.f)
        return;

    const float maxHeight = std::max(kVariablesPanelMinHeight,
                                     std::min(bodyH * kFloatingPanelMaxBodyRatio,
                                              bodyH - top - kFloatingPanelMargin));
    const float rawHeight = m_VariablesResizeStartHeight + (y - m_VariablesResizeStartY);
    m_VariablesUserHeight = std::clamp(rawHeight, kVariablesPanelMinHeight, maxHeight);
    ApplyVariablesPanelBounds();
    MarkPanelToggleDirty(m_VariablesPanel, m_Body, m_VariablesScroll);
    UpdatePanelOverflowIndicators();
}

void GraphPanel::EndVariablesPanelResize()
{
    if (!m_ResizingVariablesPanel)
        return;
    m_ResizingVariablesPanel = false;
    if (m_VariablesResizeHandle)
        m_VariablesResizeHandle->RemoveClass("dragging");
    if (m_VariablesPanel)
        m_VariablesPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::ApplyVariablesPanelCollapsedState()
{
    if (!m_VariablesPanel)
        return;

    m_VariablesDragPending = false;
    m_DraggingVariablesPanel = false;
    EndVariablesPanelResize();
    m_VariablesTitleLastClickTime = {};

    if (m_VariablesCollapsed)
    {
        m_VariablesPanel->AddClass("node-graph-variables-collapsed");
        m_VariablesPanel->Overrides()
            .Set(Style::Height, StyleLength::Px(kVariablesPanelCollapsedHeight))
            .Set(Style::MaxHeight, StyleLength::Px(kVariablesPanelCollapsedHeight));
        UILayoutAccess::SetLastLayoutRect(*m_VariablesPanel, m_VariablesPanel->GetLayoutX(), m_VariablesPanel->GetLayoutY(),
                                            m_VariablesPanel->GetLayoutWidth(), kVariablesPanelCollapsedHeight);
        m_VariablesHeightOverrideApplied = false;
        m_LastAppliedVariablesHeight = -1.f;
    }
    else
    {
        m_VariablesPanel->RemoveClass("node-graph-variables-collapsed");
        m_VariablesHeightOverrideApplied = false;
        m_LastAppliedVariablesHeight = -1.f;
        ApplyVariablesPanelBounds();
    }

    MarkLayoutVisualDirtySubtree(m_VariablesPanel);
    if (m_Body)
        m_Body->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_VariablesPanel->RequestRelayout();
    RequestRelayout();
}

void GraphPanel::SyncCanvasNodeFactory()
{
    if (!m_Canvas)
        return;
    m_Canvas->SetNodeFactory(KindNodeFactory());
}

const GraphCanvas::NodeFactoryFn& GraphPanel::KindNodeFactory()
{
    if (!m_KindNodeFactory && m_KindHooks.NodeCatalog)
        m_KindNodeFactory = m_KindHooks.NodeCatalog();
    return m_KindNodeFactory;
}

void GraphPanel::ScheduleLoadTestGraph()
{
    Scheduler::IScheduler* sched = GetScheduler();
    auto load = [this]() { LoadTestGraph(); };

    if (sched)
        sched->ScheduleNext(std::move(load));
    else
        load();
}

void GraphPanel::LoadTestGraph()
{
    if (!m_Canvas || !m_Canvas->GetModel())
        return;

    Graph::Model& model = *m_Canvas->GetModel();

    // A kind that ships a sample as an asset wins: it is the real thing, kept
    // in the project, rather than a copy built here that can drift from it.
    if (m_Context && !m_Context->AssetsRoot.empty())
    {
        const std::filesystem::path sample =
            m_KindHooks.Sample.AssetPath
                ? m_KindHooks.Sample.AssetPath(m_Context->AssetsRoot)
                : std::filesystem::path{};
        if (!sample.empty() && std::filesystem::exists(sample) && OpenGraph(sample))
            return;
    }

    if (m_KindHooks.Sample.Build && m_KindHooks.Sample.Build(model))
    {
        SyncCanvasNodeFactory();
        m_CurrentPath.clear();
        if (m_Canvas)
        {
            m_Canvas->SetSelectedNodeId(std::string());
            m_Canvas->MarkDirty(VisualDirty);
        }
        RebuildPalette();
        ScheduleVariablesPanelRebuild();
        MarkDirty();
        Notify(GraphPanelEvent::Shown);
        m_FrameNodesOnNextLayout = true;
        m_FrameRetries = 0;
        return;
    }
}

void GraphPanel::SetupUI()
{
    auto container = std::make_unique<UIElement>();
    container->AddClass("node-graph-panel-container");

    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("node-graph-toolbar");

    auto newButton = std::make_unique<Button>();
    newButton->SetId("NodeGraphNewButton");
    newButton->AddClass("secondary");
    newButton->AddClass("icon-button");
    newButton->AddClass("plus-icon");
    newButton->SetText("");
    newButton->SetTooltip("New graph");
    newButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnNewGraphButtonClicked(); });
    toolbar->AddChild(std::move(newButton));

    auto openButton = std::make_unique<Button>();
    openButton->SetId("NodeGraphOpenButton");
    openButton->AddClass("secondary");
    openButton->AddClass("icon-button");
    openButton->AddClass("folder-open-icon");
    openButton->SetText("");
    openButton->SetTooltip("Open graph");
    openButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnLoadGraphButtonClicked(); });
    toolbar->AddChild(std::move(openButton));

    auto saveButton = std::make_unique<Button>();
    saveButton->SetId("NodeGraphSaveButton");
    saveButton->AddClass("secondary");
    saveButton->AddClass("save-icon");
    saveButton->AddClass("icon-button");
    saveButton->SetText("");
    saveButton->SetTooltip("Save graph");
    saveButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSaveButtonClicked(); });
    m_SaveButton = saveButton.get();

    auto saveDirtyIndicator = std::make_unique<Label>();
    saveDirtyIndicator->AddClass("save-dirty-indicator");
    saveDirtyIndicator->AddClass("hidden");
    saveDirtyIndicator->SetText("*");
    m_SaveDirtyIndicator = saveDirtyIndicator.get();
    m_SaveButton->AddChild(std::move(saveDirtyIndicator));

    toolbar->AddChild(std::move(saveButton));

    auto titleLabel = std::make_unique<Label>();
    titleLabel->SetId("NodeGraphTitleLabel");
    titleLabel->SetText("(No graph open)");
    m_TitleLabel = titleLabel.get();
    titleLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || m_Nest.empty())
            return;
        ExitNest(true);
        ev.Stop();
    });
    toolbar->AddChild(std::move(titleLabel));

    auto subgraphBack = std::make_unique<Label>();
    subgraphBack->SetId("NodeGraphSubgraphBack");
    subgraphBack->SetText("Back");
    subgraphBack->AddClass("hidden");
    m_SubgraphBackLabel = subgraphBack.get();
    subgraphBack->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || m_Nest.empty())
            return;
        ExitNest(true);
        ev.Stop();
    });
    toolbar->AddChild(std::move(subgraphBack));

    if (m_KindHooks.ContributeChrome)
        m_KindHooks.ContributeChrome(GraphPanelRegion::StatusLabel, *toolbar);

    auto toolbarSpacer = std::make_unique<UIElement>();
    toolbarSpacer->AddClass("toolbar-spacer");
    toolbar->AddChild(std::move(toolbarSpacer));

    /* Kind-neutral expand/grid before kind chrome so they stay hit-testable
       when the kind cluster overflows. */
    auto toolbarRight = std::make_unique<UIElement>();
    toolbarRight->AddClass("node-graph-toolbar-right");

    auto nodesPanelToggleBtn = std::make_unique<Button>();
    nodesPanelToggleBtn->SetId("NodeGraphNodesPanelToggle");
    nodesPanelToggleBtn->AddClass("small");
    nodesPanelToggleBtn->AddClass("secondary");
    nodesPanelToggleBtn->AddClass("node-graph-toolbar-panel-toggle");
    nodesPanelToggleBtn->SetText("Nodes");
    nodesPanelToggleBtn->SetTooltip("Toggle node palette");
    nodesPanelToggleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ToggleNodePalettePanel(); });
    m_NodePalettePanelToggleButton = nodesPanelToggleBtn.get();
    toolbarRight->AddChild(std::move(nodesPanelToggleBtn));

    auto variablesPanelToggleBtn = std::make_unique<Button>();
    variablesPanelToggleBtn->SetId("NodeGraphVariablesPanelToggle");
    variablesPanelToggleBtn->AddClass("small");
    variablesPanelToggleBtn->AddClass("secondary");
    variablesPanelToggleBtn->AddClass("node-graph-toolbar-panel-toggle");
    variablesPanelToggleBtn->SetText("Vars");
    variablesPanelToggleBtn->SetTooltip("Toggle variables panel");
    variablesPanelToggleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ToggleVariablesPanel(); });
    m_VariablesPanelToggleButton = variablesPanelToggleBtn.get();
    toolbarRight->AddChild(std::move(variablesPanelToggleBtn));

    auto paletteColorToggleBtn = std::make_unique<Button>();
    paletteColorToggleBtn->SetId("NodeGraphPaletteColorToggle");
    paletteColorToggleBtn->AddClass("small");
    paletteColorToggleBtn->AddClass("secondary");
    paletteColorToggleBtn->AddClass("icon-button");
    paletteColorToggleBtn->AddClass("color-filter-icon");
    paletteColorToggleBtn->AddClass("node-graph-toolbar-palette-color");
    paletteColorToggleBtn->SetText("");
    paletteColorToggleBtn->SetTooltip("Color node palette entries");
    paletteColorToggleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePaletteNodeColors(); });
    m_PaletteNodeColorToggleButton = paletteColorToggleBtn.get();
    toolbarRight->AddChild(std::move(paletteColorToggleBtn));

    auto gridToggleBtn = std::make_unique<Button>();
    gridToggleBtn->SetId("NodeGraphGridToggle");
    gridToggleBtn->AddClass("small");
    gridToggleBtn->AddClass("secondary");
    gridToggleBtn->AddClass("icon-button");
    gridToggleBtn->AddClass("grid-icon");
    gridToggleBtn->AddClass("node-graph-toolbar-grid");
    gridToggleBtn->SetText("");
    gridToggleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
        UIElement& btn = *e.CurrentTarget;
        if (m_Canvas)
        {
            bool on = !m_Canvas->GetShowGrid();
            m_Canvas->SetShowGrid(on);
            if (on)
                btn.AddClass("icon-active");
            else
                btn.RemoveClass("icon-active");
        }
    });
    m_GridToggleButton = gridToggleBtn.get();
    toolbarRight->AddChild(std::move(gridToggleBtn));

    auto connectionStyleBtn = std::make_unique<Button>();
    connectionStyleBtn->SetId("NodeGraphConnectionStyleToggle");
    connectionStyleBtn->AddClass("small");
    connectionStyleBtn->AddClass("secondary");
    connectionStyleBtn->AddClass("icon-button");
    connectionStyleBtn->AddClass("node-graph-toolbar-connection-style");
    connectionStyleBtn->SetText("");
    connectionStyleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (!m_Canvas)
            return;
        m_Canvas->SetUseStraightLines(!m_Canvas->GetUseStraightLines());
        SyncConnectionStyleButton();
    });
    m_ConnectionStyleButton = connectionStyleBtn.get();
    toolbarRight->AddChild(std::move(connectionStyleBtn));

    const std::vector<GraphToolbarEntry> kindEntries =
        m_KindHooks.ToolbarEntries ? m_KindHooks.ToolbarEntries() : std::vector<GraphToolbarEntry>{};
    auto addEntries = [this, &toolbarRight](const std::vector<GraphToolbarEntry>& entries,
                                            GraphToolbarEntry::Place place)
    {
        for (const GraphToolbarEntry& entry : entries)
        {
            if (entry.Where != place)
                continue;
            if (entry.Build)
            {
                toolbarRight->AddChild(entry.Build());
                continue;
            }
            auto button = std::make_unique<Button>();
            button->SetId(entry.Id);
            button->AddClass("small");
            button->AddClass("secondary");
            if (entry.Text.empty())
                button->AddClass("icon-button");
            for (const std::string& css : entry.Classes)
                button->AddClass(css);
            button->SetText(entry.Text);
            if (!entry.Tooltip.empty())
                button->SetTooltip(entry.Tooltip);
            if (entry.Activate)
            {
                button->RegisterEventHandler(kEventButtonClick,
                                             [activate = entry.Activate](UIEvent&) { activate(); });
            }
            toolbarRight->AddChild(std::move(button));
        }
    };
    addEntries(kindEntries, GraphToolbarEntry::Place::Icon);

    /* Last of the icons in every graph kind: the toggle that shows each node's
       preview ends the run, and the kind's own actions follow it. */
    auto expandToggleButton = std::make_unique<Button>();
    expandToggleButton->SetId("NodeGraphExpandedNodesToggle");
    expandToggleButton->AddClass("small");
    expandToggleButton->AddClass("secondary");
    expandToggleButton->AddClass("icon-button");
    expandToggleButton->AddClass("node-graph-toolbar-preview-button");
    expandToggleButton->AddClass("node-graph-toolbar-expand-toggle");
    expandToggleButton->SetText("");
    expandToggleButton->SetTooltip("Expand nodes");
    expandToggleButton->RegisterEventHandler(kEventButtonClick,
                                             [this](UIEvent&) { ToggleExpandedNodes(); });
    m_ExpandedNodesToggleButton = expandToggleButton.get();
    toolbarRight->AddChild(std::move(expandToggleButton));

    addEntries(kindEntries, GraphToolbarEntry::Place::Action);

    auto searchSpacer = std::make_unique<UIElement>();
    searchSpacer->AddClass("node-graph-toolbar-search-spacer");
    toolbarRight->AddChild(std::move(searchSpacer));

    auto searchBuilt = BuildPanelSearchBar(
        "nodegraph-search-field",
        []() { return SettingsPanel::GetSearchBarsVisible(); },
        {},
        [this](const std::string& value) {
            if (m_Canvas)
                m_Canvas->SetNodeSearchFilter(value);
        },
        {{"all", "All fields"}, {"name", "Display name"}, {"type", "Type ID"}},
        [this](const std::string& field) {
            if (m_Canvas)
                m_Canvas->SetNodeSearchField(field);
        });
    m_GraphSearchField = searchBuilt.FieldPtr;
    toolbarRight->AddChild(std::move(searchBuilt.Root));
    SettingsPanel::RegisterSearchBar(searchBuilt.RootPtr, searchBuilt.IconPtr);

    toolbar->AddChild(std::move(toolbarRight));

    container->AddChild(std::move(toolbar));

    if (m_KindHooks.ContributeChrome)
        m_KindHooks.ContributeChrome(GraphPanelRegion::BelowToolbar, *container);

    auto body = std::make_unique<UIElement>();
    body->AddClass("node-graph-body");
    m_Body = body.get();

    auto palette = std::make_unique<UIElement>();
    palette->AddClass("node-graph-palette");
    m_Palette = palette.get();

    auto paletteTitle = std::make_unique<Label>();
    paletteTitle->SetText("NODE PALETTE");
    paletteTitle->AddClass("node-graph-palette-title");
    paletteTitle->AddClass("node-graph-palette-drag-handle");
    paletteTitle->SetTooltip("Drag to move. Double-click to collapse or expand.");
    m_PaletteDragHandle = paletteTitle.get();
    paletteTitle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || !m_Palette)
            return;
        UIElement* bodyEl = m_Palette->GetParent();
        if (!bodyEl)
            return;
        ev.Capture(m_PaletteDragHandle); /* capture to title so it receives MouseUp */
        m_PaletteDragPending = true;
        m_PaletteDragStartX = ev.X;
        m_PaletteDragStartY = ev.Y;
        m_PaletteDragStartLeft = m_Palette->GetLayoutX() - bodyEl->GetLayoutX();
        m_PaletteDragStartTop = m_Palette->GetLayoutY() - bodyEl->GetLayoutY();
    });
    paletteTitle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev)
    {
        if (!m_PaletteDragPending && !m_DraggingPalette)
            return;
        ApplyNodePaletteDrag(ev.X, ev.Y);
    });
    paletteTitle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || !m_Palette)
            return;
        if (m_DraggingPalette)
        {
            m_DraggingPalette = false;
            m_PaletteDragPending = false;
            m_Palette->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            RequestRelayout();
            return;
        }
        if (!m_PaletteDragPending)
            return; /* didn't start on title */
        m_PaletteDragPending = false;
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        bool isDoubleClick = (now - m_PaletteTitleLastClickTime) < GameEngine::Platform::GetDoubleClickInterval();
        m_PaletteTitleLastClickTime = now;
        if (isDoubleClick)
        {
            m_PaletteCollapsed = !m_PaletteCollapsed;
            ApplyNodePaletteCollapsedState();
        }
    });
    palette->AddChild(std::move(paletteTitle));

    auto paletteSearchRow = std::make_unique<UIElement>();
    paletteSearchRow->AddClass("node-graph-palette-search-row");
    UIElement* paletteSearchRowPtr = paletteSearchRow.get();
    EditorSearchBars::RegisterFocusTarget(paletteSearchRowPtr);
    auto paletteSearchClearRef = std::make_shared<Button*>(nullptr);
    auto schedulePaletteSearchRebuild = [this]()
    {
        SchedulePaletteRebuild();
    };
    auto updatePaletteSearchClear = [paletteSearchClearRef, paletteSearchRowPtr](const std::string& value)
    {
        Button* clearButton = paletteSearchClearRef ? *paletteSearchClearRef : nullptr;
        if (!clearButton)
            return;
        if (value.empty())
        {
            clearButton->AddClass("hidden");
            paletteSearchRowPtr->RemoveClass("has-query");
        }
        else
        {
            clearButton->RemoveClass("hidden");
            paletteSearchRowPtr->AddClass("has-query");
        }
    };

    auto searchField = std::make_unique<TextField>();
    searchField->SetId("NodeGraphPaletteSearch");
    searchField->SetValue("");
    searchField->AddClass("node-graph-palette-search");
    searchField->SetOnValueChanging([schedulePaletteSearchRebuild, updatePaletteSearchClear](const std::string& value) {
        updatePaletteSearchClear(value);
        schedulePaletteSearchRebuild();
    });
    m_PaletteSearchField = searchField.get();
    paletteSearchRow->AddChild(std::move(searchField));
    auto paletteSearchClear = std::make_unique<Button>();
    paletteSearchClear->AddClass("node-graph-inline-search-clear");
    paletteSearchClear->AddClass("xclose-icon");
    paletteSearchClear->AddClass("hidden");
    paletteSearchClear->SetFocusable(false);
    paletteSearchClear->SetTooltip("Clear search");
    *paletteSearchClearRef = paletteSearchClear.get();
    auto clearPaletteSearch = [this, schedulePaletteSearchRebuild, updatePaletteSearchClear]()
    {
        if (!m_PaletteSearchField || m_PaletteSearchField->GetValue().empty())
            return;
        m_PaletteSearchField->SetValue("");
        updatePaletteSearchClear("");
        schedulePaletteSearchRebuild();
    };
    paletteSearchClear->RegisterEventHandler(kEventMouseDown, [clearPaletteSearch](UIEvent& ev)
    {
        if (ev.Button != 0)
            return;
        clearPaletteSearch();
        ev.Stop();
    });
    paletteSearchClear->RegisterEventHandler(kEventButtonClick, [clearPaletteSearch](UIEvent&) {
        clearPaletteSearch();
    });
    auto paletteSearchIcon = std::make_unique<UIElement>();
    paletteSearchIcon->AddClass("node-graph-inline-search-icon");
    paletteSearchRow->AddChild(std::move(paletteSearchIcon));
    paletteSearchRow->AddChild(std::move(paletteSearchClear));
    palette->AddChild(std::move(paletteSearchRow));

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->SetId("NodeGraphPaletteScroll");
    scrollView->AddClass("node-graph-palette-scroll");
    auto contentContainer = std::make_unique<UIElement>();
    contentContainer->AddClass("node-graph-palette-content");
    m_PaletteContent = contentContainer.get();
    scrollView->AddContent(std::move(contentContainer));
    m_PaletteScroll = scrollView.get();
    scrollView->SetOnScrollChanged([this](float, float)
    {
        UpdatePanelOverflowIndicators();
    });
    palette->AddChild(std::move(scrollView));

    auto paletteTopOverflowIndicator = std::make_unique<NodeGraphOverflowIndicator>(NodeGraphOverflowIndicator::Edge::Top);
    m_PaletteTopOverflowIndicator = paletteTopOverflowIndicator.get();
    palette->AddChild(std::move(paletteTopOverflowIndicator));

    auto paletteBottomOverflowIndicator = std::make_unique<NodeGraphOverflowIndicator>(NodeGraphOverflowIndicator::Edge::Bottom);
    m_PaletteBottomOverflowIndicator = paletteBottomOverflowIndicator.get();
    palette->AddChild(std::move(paletteBottomOverflowIndicator));

    auto paletteResizeHandle = std::make_unique<UIElement>();
    paletteResizeHandle->AddClass("node-graph-panel-resize-bottom");
    paletteResizeHandle->SetTooltip("Drag to resize");
    m_PaletteResizeHandle = paletteResizeHandle.get();
    paletteResizeHandle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button != 0)
            return;
        BeginNodePaletteResize(ev.X, ev.Y);
        if (m_PaletteResizeHandle)
            ev.Capture(m_PaletteResizeHandle);
        ev.Stop();
    });
    paletteResizeHandle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev)
    {
        if (!m_ResizingPalettePanel)
            return;
        UpdateNodePaletteResize(ev.X, ev.Y);
        ev.Stop();
    });
    paletteResizeHandle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || !m_ResizingPalettePanel)
            return;
        EndNodePaletteResize();
        ev.Stop();
    });
    palette->AddChild(std::move(paletteResizeHandle));
    body->AddChild(std::move(palette));

    auto variablesPanel = std::make_unique<UIElement>();
    variablesPanel->AddClass("node-graph-variables");
    m_VariablesPanel = variablesPanel.get();

    auto variablesHeader = std::make_unique<UIElement>();
    variablesHeader->AddClass("node-graph-variables-header");
    auto variablesTitle = std::make_unique<Label>();
    variablesTitle->SetText("VARIABLES");
    variablesTitle->AddClass("node-graph-variables-title");
    variablesTitle->AddClass("node-graph-variables-drag-handle");
    variablesTitle->SetTooltip("Drag to move. Double-click to collapse or expand. Right-click for sorting.");
    m_VariablesDragHandle = variablesTitle.get();
    variablesTitle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button == 1)
        {
            ShowVariablesSortContextMenu(ev.X, ev.Y);
            ev.Stop();
            return;
        }
        if (ev.Button != 0 || !m_VariablesPanel)
            return;
        UIElement* bodyEl = m_VariablesPanel->GetParent();
        if (!bodyEl)
            return;
        ev.Capture(m_VariablesDragHandle);
        m_VariablesDragPending = true;
        m_VariablesDragStartX = ev.X;
        m_VariablesDragStartY = ev.Y;
        m_VariablesDragStartLeft = m_VariablesPanel->GetLayoutX() - bodyEl->GetLayoutX();
        m_VariablesDragStartTop = m_VariablesPanel->GetLayoutY() - bodyEl->GetLayoutY();
    });
    variablesTitle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev)
    {
        if (!m_VariablesDragPending && !m_DraggingVariablesPanel)
            return;
        ApplyVariablesPanelDrag(ev.X, ev.Y);
    });
    variablesTitle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || !m_VariablesPanel)
            return;
        if (m_DraggingVariablesPanel)
        {
            m_DraggingVariablesPanel = false;
            m_VariablesDragPending = false;
            m_VariablesPanel->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            RequestRelayout();
            return;
        }
        if (!m_VariablesDragPending)
            return;
        m_VariablesDragPending = false;
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        bool isDoubleClick = (now - m_VariablesTitleLastClickTime) < GameEngine::Platform::GetDoubleClickInterval();
        m_VariablesTitleLastClickTime = now;
        if (isDoubleClick)
        {
            m_VariablesCollapsed = !m_VariablesCollapsed;
            ApplyVariablesPanelCollapsedState();
        }
    });
    variablesHeader->AddChild(std::move(variablesTitle));

    auto addVariableButton = std::make_unique<Button>();
    addVariableButton->SetText("+");
    addVariableButton->AddClass("node-graph-variables-add");
    addVariableButton->SetTooltip("Add variable");
    addVariableButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { AddGraphVariable(); });
    variablesHeader->AddChild(std::move(addVariableButton));
    variablesPanel->AddChild(std::move(variablesHeader));

    auto variablesSearchRow = std::make_unique<UIElement>();
    variablesSearchRow->AddClass("node-graph-variables-search-row");
    UIElement* variablesSearchRowPtr = variablesSearchRow.get();
    EditorSearchBars::RegisterFocusTarget(variablesSearchRowPtr);
    auto variablesSearchClearRef = std::make_shared<Button*>(nullptr);
    auto updateVariablesSearchClear = [variablesSearchClearRef, variablesSearchRowPtr](const std::string& value)
    {
        Button* clearButton = variablesSearchClearRef ? *variablesSearchClearRef : nullptr;
        if (!clearButton)
            return;
        if (value.empty())
        {
            clearButton->AddClass("hidden");
            variablesSearchRowPtr->RemoveClass("has-query");
        }
        else
        {
            clearButton->RemoveClass("hidden");
            variablesSearchRowPtr->AddClass("has-query");
        }
    };

    auto variablesSearch = std::make_unique<TextField>();
    variablesSearch->SetId("NodeGraphVariablesSearch");
    variablesSearch->SetValue("");
    variablesSearch->AddClass("node-graph-variables-search");
    variablesSearch->SetOnValueChanging([this, updateVariablesSearchClear](const std::string& value)
    {
        updateVariablesSearchClear(value);
        ScheduleVariablesPanelRebuild();
    });
    m_VariablesSearchField = variablesSearch.get();
    variablesSearchRow->AddChild(std::move(variablesSearch));
    auto variablesSearchClear = std::make_unique<Button>();
    variablesSearchClear->AddClass("node-graph-inline-search-clear");
    variablesSearchClear->AddClass("xclose-icon");
    variablesSearchClear->AddClass("hidden");
    variablesSearchClear->SetFocusable(false);
    variablesSearchClear->SetTooltip("Clear search");
    *variablesSearchClearRef = variablesSearchClear.get();
    auto clearVariablesSearch = [this, updateVariablesSearchClear]()
    {
        if (!m_VariablesSearchField || m_VariablesSearchField->GetValue().empty())
            return;
        m_VariablesSearchField->SetValue("");
        updateVariablesSearchClear("");
        ScheduleVariablesPanelRebuild();
    };
    variablesSearchClear->RegisterEventHandler(kEventMouseDown, [clearVariablesSearch](UIEvent& ev)
    {
        if (ev.Button != 0)
            return;
        clearVariablesSearch();
        ev.Stop();
    });
    variablesSearchClear->RegisterEventHandler(kEventButtonClick, [clearVariablesSearch](UIEvent&) {
        clearVariablesSearch();
    });
    auto variablesSearchIcon = std::make_unique<UIElement>();
    variablesSearchIcon->AddClass("node-graph-inline-search-icon");
    variablesSearchRow->AddChild(std::move(variablesSearchIcon));
    variablesSearchRow->AddChild(std::move(variablesSearchClear));
    variablesPanel->AddChild(std::move(variablesSearchRow));

    auto variablesScroll = std::make_unique<ScrollView>();
    variablesScroll->SetId("NodeGraphVariablesScroll");
    variablesScroll->AddClass("node-graph-variables-scroll");
    auto variablesContent = std::make_unique<UIElement>();
    variablesContent->AddClass("node-graph-variables-content");
    m_VariablesContent = variablesContent.get();
    variablesScroll->AddContent(std::move(variablesContent));
    m_VariablesScroll = variablesScroll.get();
    variablesScroll->SetOnScrollChanged([this](float, float)
    {
        UpdatePanelOverflowIndicators();
    });
    variablesPanel->AddChild(std::move(variablesScroll));

    auto variablesTopOverflowIndicator = std::make_unique<NodeGraphOverflowIndicator>(NodeGraphOverflowIndicator::Edge::Top);
    m_VariablesTopOverflowIndicator = variablesTopOverflowIndicator.get();
    variablesPanel->AddChild(std::move(variablesTopOverflowIndicator));

    auto variablesBottomOverflowIndicator = std::make_unique<NodeGraphOverflowIndicator>(NodeGraphOverflowIndicator::Edge::Bottom);
    m_VariablesBottomOverflowIndicator = variablesBottomOverflowIndicator.get();
    variablesPanel->AddChild(std::move(variablesBottomOverflowIndicator));

    auto variablesResizeHandle = std::make_unique<UIElement>();
    variablesResizeHandle->AddClass("node-graph-panel-resize-bottom");
    variablesResizeHandle->SetTooltip("Drag to resize");
    m_VariablesResizeHandle = variablesResizeHandle.get();
    variablesResizeHandle->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev)
    {
        if (ev.Button != 0)
            return;
        BeginVariablesPanelResize(ev.X, ev.Y);
        if (m_VariablesResizeHandle)
            ev.Capture(m_VariablesResizeHandle);
        ev.Stop();
    });
    variablesResizeHandle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& ev)
    {
        if (!m_ResizingVariablesPanel)
            return;
        UpdateVariablesPanelResize(ev.X, ev.Y);
        ev.Stop();
    });
    variablesResizeHandle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev)
    {
        if (ev.Button != 0 || !m_ResizingVariablesPanel)
            return;
        EndVariablesPanelResize();
        ev.Stop();
    });
    variablesPanel->AddChild(std::move(variablesResizeHandle));
    body->AddChild(std::move(variablesPanel));

    auto canvasWrapper = std::make_unique<UIElement>();
    canvasWrapper->AddClass("node-graph-canvas-wrapper");
    /* No inline style override: CSS gives wrapper flex 1 1 0%, width/height 100%, min 200px so it gets real size for hit-test */
    auto canvas = std::make_unique<GraphCanvas>(&m_Model);
    canvas->SetId("NodeGraphCanvas");
    canvas->SetUseStraightLines(true);
    canvas->SetFocusable(true); /* so Delete / Copy / Paste key events are received */
    /* No inline min-height: 0 so CSS .node-canvas min-width/min-height ensure canvas receives mouse events */
    canvas->SetOnSelectionChanged([this](const std::string& nodeId) { OnCanvasSelectionChanged(nodeId); });
    canvas->SetOnLinkSelected([this](const std::string& linkId) { OnCanvasLinkSelected(linkId); });
    canvas->SetOnNodeDoubleClicked([this](const std::string& nodeId) { OnCanvasNodeDoubleClicked(nodeId); });
    canvas->SetOnPrimaryPress([this]()
    {
        Notify(GraphPanelEvent::CanvasPrimaryPress);
    });
    canvas->SetOnRequestContextMenu([this](float x, float y, bool onNode, const std::string& nodeId)
    {
        OnCanvasContextMenu(x, y, onNode, nodeId);
    });
    canvas->SetOnRequestWireDropMenu([this](float x, float y,
                                            const std::string& anchorNodeId,
                                            const std::string& anchorPortId,
                                            bool fromInputPort)
    {
        OnCanvasWireDropMenu(x, y, anchorNodeId, anchorPortId, fromInputPort);
    });
    canvas->SetOnGraphChanged([this]()
    {
        /* A node drag only moves rects, so the inspector and the variables board
           have nothing to re-read. The canvas reports the drag's change after it
           has already cleared the latch (drag-ended runs first in the same
           post-update action), hence the second flag: without it the release
           rebuilt the whole inspector and left the canvas unresponsive while the
           node settled. */
        const bool fromNodeMove = m_NodeDragInProgress || m_ChangeFromNodeMove;
        m_ChangeFromNodeMove = false;
        MarkDirty();
        if (!fromNodeMove)
        {
            if (m_KindHooks.SyncGraphVariables)
                m_KindHooks.SyncGraphVariables();
            ScheduleVariablesPanelRebuild();
            m_InspectorRefreshDeferred = true;
        }
    });
    canvas->SetUndoScope([this](const std::string& actionName, std::function<void()> mutate)
    {
        if (!m_Undo || !m_Canvas)
        {
            if (mutate)
                mutate();
            return;
        }
        std::vector<std::uint8_t> before;
        if (!CaptureGraphSnapshot(before))
        {
            if (mutate)
                mutate();
            return;
        }
        if (mutate)
            mutate();
        std::vector<std::uint8_t> after;
        if (!CaptureGraphSnapshot(after))
            return;
        auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                          actionName);
        m_Undo->CommitAlreadyApplied(std::move(cmd));
    });
    canvas->SetOnNodeDragStarted([this]()
    {
        m_NodeDragInProgress = true;
        if (!m_Undo || !m_Canvas)
            return;
        Editor::UndoRedoService::SnapshotTarget target;
        target.debugLabel = "Graph";
        target.Capture = [this](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
        {
            return CaptureGraphSnapshot(out);
        };
        target.Apply = [this](const Editor::UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
        {
            return ApplyGraphSnapshot(in);
        };
        m_GraphDragEdit = m_Undo->BeginInteractiveEdit("Move Nodes", std::move(target));
    });
    canvas->SetOnNodeDragEnded([this]()
    {
        if (m_GraphDragEdit)
        {
            m_GraphDragEdit.Commit();
        }
        m_NodeDragInProgress = false;
        /* The canvas fires OnGraphChanged straight after this. */
        m_ChangeFromNodeMove = true;
        Notify(GraphPanelEvent::NodeDragEnded);
    });
    m_Canvas = canvas.get();
    canvasWrapper->AddChild(std::move(canvas));
    SyncCanvasNodeFactory();
    SyncConnectionStyleButton();

    if (m_KindHooks.ContributeChrome)
        m_KindHooks.ContributeChrome(GraphPanelRegion::CanvasOverlays, *canvasWrapper);
    body->AddChild(std::move(canvasWrapper));

    if (m_KindHooks.ContributeChrome)
        m_KindHooks.ContributeChrome(GraphPanelRegion::Body, *body);

    container->AddChild(std::move(body));
    AddChild(std::move(container));

    UpdatePaletteNodeColorToggleButton();
    Notify(GraphPanelEvent::Shown);
    UpdateNodePalettePanelToggleButton();
    UpdateVariablesPanelToggleButton();
    SchedulePaletteRebuild();
    ScheduleVariablesPanelRebuild();
}

void GraphPanel::OnSaveButtonClicked()
{
    SaveGraph();
}

void GraphPanel::PromptSaveBeforeQuit(std::function<void()> onProceed)
{
    if (!m_Dirty)
    {
        onProceed();
        return;
    }
    m_PendingQuitCallback = std::move(onProceed);
    EnsureUnsavedChangesModal();
    if (m_UnsavedChangesModal)
    {
        const std::string filename = m_CurrentPath.empty()
            ? UntitledGraphTitle(m_PanelKindId)
            : m_CurrentPath.filename().string();
        m_UnsavedChangesModal->Show(
            "Unsaved Graph Changes",
            "\"" + filename + "\" has unsaved changes.\nSave before closing?");
    }
}

void GraphPanel::OnNewGraphButtonClicked()
{
    if (m_Dirty)
    {
        m_HasPendingNewGraph = true;
        m_PendingNewGraphPath.clear();
        EnsureUnsavedChangesModal();
        if (m_UnsavedChangesModal)
        {
            const std::string filename = m_CurrentPath.empty()
                ? UntitledGraphTitle(m_PanelKindId)
                : m_CurrentPath.filename().string();
            m_UnsavedChangesModal->Show(
                "Unsaved Graph Changes",
                "\"" + filename + "\" has unsaved changes.\nSave before creating a new graph?");
        }
        return;
    }

    const std::filesystem::path newPath = PromptSaveAsPath();
    if (newPath.empty())
        return;
    CreateNewGraph(newPath);
}

void GraphPanel::CreateNewGraph(const std::filesystem::path& path)
{
    ExitAllNests(true);
    m_Model = Graph::Model{};
    m_Model.KindId = m_PanelKindId;
    m_CurrentPath = path;
    m_Dirty = false;

    if (m_Canvas)
    {
        SyncCanvasNodeFactory();
        m_Canvas->SetModel(&m_Model);
        m_Canvas->SetSelectedNodeId(std::string());
        m_Canvas->MarkDirty(VisualDirty);
    }
    UpdateTitleLabel();
    SchedulePaletteRebuild();
    ScheduleVariablesPanelRebuild();
    Notify(GraphPanelEvent::Shown);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);

    m_Dirty = true;
    UpdateTitleLabel();
}

void GraphPanel::DiscardCurrentGraphChanges()
{
    ExitAllNests(false);
    Graph::Model cleanModel;
    std::filesystem::path cleanPath = m_CurrentPath;

    if (cleanPath.empty() || !LoadGraphModelFromPath(cleanPath, cleanModel) ||
        cleanModel.KindId != m_PanelKindId)
    {
        cleanModel = Graph::Model{};
        cleanModel.KindId = m_PanelKindId;
        cleanPath.clear();
    }

    m_Model = std::move(cleanModel);
    m_CurrentPath = cleanPath;
    m_Dirty = false;
}

void GraphPanel::RefreshGraphViewAfterModelChange()
{
    if (m_Canvas)
        SyncCanvasNodeFactory();
    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();

    if (m_Canvas)
    {
        m_Canvas->SetModel(&m_Model);
        m_Canvas->SetSelectedNodeId(std::string());
        SyncViewportFromModel();
        m_Canvas->ReconcileStateWithModel();
        m_Canvas->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    RebuildPalette();
    RebuildVariablesPanel();
    Notify(GraphPanelEvent::Shown);
    UpdateTitleLabel();

    if (m_PaletteScroll)
        m_PaletteScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::EnsureUnsavedChangesModal()
{
    if (m_UnsavedChangesModal)
        return;

    auto modal = std::make_unique<SaveSceneChangesModal>();
    m_UnsavedChangesModal = modal.get();
    UIManager* ui = GetOwnerManager();
    UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
    if (uiRoot)
        uiRoot->AddChild(std::move(modal));
    else
        AddChild(std::move(modal));

    m_UnsavedChangesModal->SetOnSave([this]()
    {
        auto run = [this]()
        {
            if (m_HasPendingLoadGraph)
            {
                const std::filesystem::path loadPath = m_PendingLoadGraphPath;
                m_HasPendingLoadGraph = false;
                m_PendingLoadGraphPath.clear();
                if (SaveGraph())
                    (void)OpenGraph(loadPath);
                return;
            }
            if (m_HasPendingNewGraph)
            {
                m_HasPendingNewGraph = false;
                m_PendingNewGraphPath.clear();
                if (!SaveGraph())
                    return;
                const std::filesystem::path newPath = PromptSaveAsPath();
                if (!newPath.empty())
                    CreateNewGraph(newPath);
                return;
            }
            if (m_PendingQuitCallback)
            {
                auto cb = std::move(m_PendingQuitCallback);
                m_PendingQuitCallback = nullptr;
                if (SaveGraph())
                    cb();
                return;
            }
        };
        if (Scheduler::IScheduler* sched = GetScheduler())
            sched->ScheduleNext(std::move(run));
        else
            run();
    });

    m_UnsavedChangesModal->SetOnDontSave([this]()
    {
        auto run = [this]()
        {
            if (m_HasPendingLoadGraph)
            {
                const std::filesystem::path loadPath = m_PendingLoadGraphPath;
                m_HasPendingLoadGraph = false;
                m_PendingLoadGraphPath.clear();
                DiscardCurrentGraphChanges();
                (void)OpenGraph(loadPath);
                return;
            }
            if (m_HasPendingNewGraph)
            {
                m_HasPendingNewGraph = false;
                m_PendingNewGraphPath.clear();
                DiscardCurrentGraphChanges();
                const std::filesystem::path newPath = PromptSaveAsPath();
                if (!newPath.empty())
                    CreateNewGraph(newPath);
                return;
            }
            if (m_PendingQuitCallback)
            {
                auto cb = std::move(m_PendingQuitCallback);
                m_PendingQuitCallback = nullptr;
                cb();
                return;
            }
        };
        if (Scheduler::IScheduler* sched = GetScheduler())
            sched->ScheduleNext(std::move(run));
        else
            run();
    });

    m_UnsavedChangesModal->SetOnCancel([this]()
    {
        auto run = [this]()
        {
            m_HasPendingLoadGraph = false;
            m_PendingLoadGraphPath.clear();
            m_HasPendingNewGraph = false;
            m_PendingNewGraphPath.clear();
            m_PendingQuitCallback = nullptr;
        };
        if (Scheduler::IScheduler* sched = GetScheduler())
            sched->ScheduleNext(std::move(run));
        else
            run();
    });
}

void GraphPanel::UpdateTitleLabel()
{
    if (m_TitleLabel)
    {
        std::string title = m_CurrentPath.empty() ? "(Untitled)" : m_CurrentPath.filename().string();
        const Graph::Model* parent = &m_Model;
        for (const GraphNestFrame& frame : m_Nest)
        {
            title += " / ";
            std::string segment = NestKindTitle(frame.Kind);
            if (const Graph::Node* host = parent->FindNode(frame.HostNodeId))
            {
                if (frame.Kind == GraphNestKind::PoseGraph)
                {
                    const std::string stateTitle = host->Parameters.GetString("title");
                    if (!stateTitle.empty())
                        segment = stateTitle;
                }
            }
            title += segment;
            if (NestKindUsesSubgraphModel(frame.Kind))
                parent = &frame.Model;
        }
        if (m_Dirty)
            title += " *";
        m_TitleLabel->SetText(title);
        m_TitleLabel->SetTooltip(m_Nest.empty()
                                     ? std::string{}
                                     : std::string("Leave ") + NestKindTitle(CurrentNestKind()));
    }

    if (m_SubgraphBackLabel)
    {
        if (m_Nest.empty())
            m_SubgraphBackLabel->AddClass("hidden");
        else
            m_SubgraphBackLabel->RemoveClass("hidden");
        if (!m_Nest.empty())
            m_SubgraphBackLabel->SetTooltip(std::string("Leave ") + NestKindTitle(CurrentNestKind()));
        m_SubgraphBackLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_SaveDirtyIndicator)
    {
        if (m_Dirty)
            m_SaveDirtyIndicator->RemoveClass("hidden");
        else
            m_SaveDirtyIndicator->AddClass("hidden");
        m_SaveDirtyIndicator->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void GraphPanel::SyncViewportFromModel()
{
    if (!m_Canvas)
        return;
    const Graph::Model* model = m_Canvas->GetModel();
    if (!model)
        model = &m_Model;
    m_Canvas->SetPanZoom(model->Viewport.PanX, model->Viewport.PanY, model->Viewport.Zoom);
}

void GraphPanel::SyncViewportToModel()
{
    if (!m_Canvas)
        return;
    float px, py, zoom;
    m_Canvas->GetPanZoom(px, py, zoom);
    Graph::Model* model = m_Canvas->GetModel();
    if (!model)
        model = &m_Model;
    model->Viewport.PanX = px;
    model->Viewport.PanY = py;
    model->Viewport.Zoom = zoom;
}

Graph::Model* GraphPanel::ActiveGraphModel()
{
    for (int i = static_cast<int>(m_Nest.size()) - 1; i >= 0; --i)
    {
        if (NestKindUsesSubgraphModel(m_Nest[static_cast<size_t>(i)].Kind))
            return &m_Nest[static_cast<size_t>(i)].Model;
    }
    return &m_Model;
}

const Graph::Model* GraphPanel::ActiveGraphModel() const
{
    for (int i = static_cast<int>(m_Nest.size()) - 1; i >= 0; --i)
    {
        if (NestKindUsesSubgraphModel(m_Nest[static_cast<size_t>(i)].Kind))
            return &m_Nest[static_cast<size_t>(i)].Model;
    }
    return &m_Model;
}

GraphNestKind GraphPanel::CurrentNestKind() const
{
    return m_Nest.empty() ? GraphNestKind::Root : m_Nest.back().Kind;
}

void GraphPanel::WriteNestFramesInto(Graph::Model& root) const
{
    // Innermost-first writeback. Blend-space nests have no nested Model.
    std::vector<Graph::Model> copies;
    copies.reserve(m_Nest.size());
    for (const GraphNestFrame& frame : m_Nest)
        copies.push_back(frame.Model);

    for (int i = static_cast<int>(m_Nest.size()) - 1; i >= 0; --i)
    {
        const GraphNestFrame& frame = m_Nest[static_cast<size_t>(i)];
        Graph::Model* parent = &root;
        for (int j = i - 1; j >= 0; --j)
        {
            if (NestKindUsesSubgraphModel(m_Nest[static_cast<size_t>(j)].Kind))
            {
                parent = &copies[static_cast<size_t>(j)];
                break;
            }
        }
        Graph::Node* host = parent->FindNode(frame.HostNodeId);
        if (!host)
            continue;
        if (NestKindHidesCanvas(frame.Kind))
        {
            const bool liveTop = (static_cast<size_t>(i) + 1 == m_Nest.size());
            const Graph::Node* liveHost =
                NestParentModel(m_Nest, m_Model, static_cast<size_t>(i))->FindNode(frame.HostNodeId);
            if (m_KindHooks.Nest.WriteLiveHost)
                m_KindHooks.Nest.WriteLiveHost(*host, liveHost, frame.Kind, liveTop);
        }
        else if (NestKindUsesSubgraphModel(frame.Kind))
        {
            GraphSubgraphStore::StoreSubgraph(*host, copies[static_cast<size_t>(i)]);
        }
    }
}

void GraphPanel::WriteLiveNestToHosts()
{
    if (m_Canvas && !NestKindHidesCanvas(CurrentNestKind()))
        SyncViewportToModel();

    for (int i = static_cast<int>(m_Nest.size()) - 1; i >= 0; --i)
    {
        GraphNestFrame& frame = m_Nest[static_cast<size_t>(i)];
        Graph::Model* parent = NestParentModel(m_Nest, m_Model, static_cast<size_t>(i));
        Graph::Node* host = parent->FindNode(frame.HostNodeId);
        if (!host)
            continue;
        if (NestKindHidesCanvas(frame.Kind))
        {
            const bool liveTop = static_cast<size_t>(i) + 1 == m_Nest.size();
            if (m_KindHooks.Nest.WriteLiveHost)
                m_KindHooks.Nest.WriteLiveHost(*host, host, frame.Kind, liveTop);
        }
        else if (NestKindUsesSubgraphModel(frame.Kind))
        {
            GraphSubgraphStore::StoreSubgraph(*host, frame.Model);
        }
    }
}

void GraphPanel::EnterNest(const std::string& nodeId)
{
    if (m_Nest.size() >= kMaxNestDepth)
        return;
    Graph::Model* active = ActiveGraphModel();
    if (!active)
        return;
    const Graph::Node* host = active->FindNode(nodeId);
    if (!host)
        return;
    const GraphNestKind current = CurrentNestKind();
    const GraphNestKind next = NestKindForDoubleClick(current, host->TypeId);
    if (next == current)
        return;

    if (m_Canvas && !NestKindHidesCanvas(current))
        SyncViewportToModel();

    GraphNestFrame frame;
    frame.HostNodeId = nodeId;
    frame.Kind = next;
    if (NestKindUsesSubgraphModel(next))
    {
        if (!GraphSubgraphStore::TryLoadSubgraph(*host, frame.Model))
        {
            frame.Model = {};
            frame.Model.KindId = m_PanelKindId;
        }
        GraphNodeRegistry::Get().HydrateGraphModelPorts(frame.Model);
    }

    m_Nest.push_back(std::move(frame));
    ApplyNestView(host);
}

void GraphPanel::ExitNest(bool writeBack)
{
    if (m_Nest.empty())
        return;
    if (writeBack)
        WriteLiveNestToHosts();
    m_Nest.pop_back();
    ApplyNestView();
    if (writeBack)
        MarkDirty();
}

void GraphPanel::ExitAllNests(bool writeBack)
{
    if (m_Nest.empty())
        return;
    if (writeBack)
        WriteLiveNestToHosts();
    m_Nest.clear();
    ApplyNestView();
    if (writeBack)
        MarkDirty();
}

void GraphPanel::ApplyNestView(const Graph::Node* host)
{
    const GraphNestKind kind = CurrentNestKind();
    if (m_Canvas)
    {
        if (NestKindHidesCanvas(kind))
        {
            m_Canvas->AddClass("hidden");
        }
        else
        {
            m_Canvas->RemoveClass("hidden");
            Graph::Model* model = ActiveGraphModel();
            m_Canvas->SetModel(model);
            m_Canvas->SetSelectedNodeId(std::string());
            m_Canvas->SetPanZoom(model->Viewport.PanX, model->Viewport.PanY, model->Viewport.Zoom);
            m_Canvas->ReconcileStateWithModel();
            m_Canvas->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    }

    if (m_KindHooks.Nest.OnChanged)
        m_KindHooks.Nest.OnChanged(host, kind);
    RebuildPalette();
    UpdateTitleLabel();
}

bool GraphPanel::IsPaletteCategoryVisible(std::string_view category) const
{
    switch (CurrentNestKind())
    {
    case GraphNestKind::StateMachine:
        return category == kCategoryState;
    case GraphNestKind::BlendSpace1D:
    case GraphNestKind::BlendSpace2D:
        return false;
    default:
        return category != kCategoryState;
    }
}

void GraphPanel::OnCanvasNodeDoubleClicked(const std::string& nodeId)
{
    const Graph::Model* model = ActiveGraphModel();
    if (!model || !model->FindNode(nodeId))
        return;
    EnterNest(nodeId);
}

bool GraphPanel::CaptureGraphSnapshot(std::vector<std::uint8_t>& out) const
{
    Graph::Model snapshot = m_Model;
    WriteNestFramesInto(snapshot);
    std::string json = Graph::ToJson(snapshot);
    out.assign(json.begin(), json.end());
    return true;
}

bool GraphPanel::ApplyGraphSnapshot(const std::vector<std::uint8_t>& snapshot)
{
    if (snapshot.empty())
        return false;
    /* The camera is editor state, not document history: undo/redo swap the
       model but must not teleport the view to where it was when the edit
       happened. Carry the live canvas pan/zoom across the swap. */
    Graph::Viewport liveView = m_Model.Viewport;
    if (m_Canvas)
        m_Canvas->GetPanZoom(liveView.PanX, liveView.PanY, liveView.Zoom);
    std::string json(snapshot.begin(), snapshot.end());
    Graph::Model loaded;
    if (!Graph::FromJson(json, loaded) || loaded.KindId != m_PanelKindId)
    {
        Logger::Log::Error(
            "GraphPanel: failed to apply graph snapshot ({} bytes); model left unchanged.",
            snapshot.size());
        if (m_Canvas)
        {
            m_Canvas->ReconcileStateWithModel();
            m_Canvas->MarkDirty(UIElement::VisualDirty);
        }
        return false;
    }
    m_Model = std::move(loaded);
    m_Model.KindId = m_PanelKindId;
    /* Restore the live camera only after a successful parse: undo/redo must not
       teleport the view, and a failed parse returns above without touching it. */
    m_Model.Viewport = liveView;
    SyncCanvasNodeFactory();
    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();

    const Graph::Model* parent = &m_Model;
    size_t keep = m_Nest.size();
    for (size_t i = 0; i < m_Nest.size(); ++i)
    {
        const Graph::Node* host = parent->FindNode(m_Nest[i].HostNodeId);
        if (!host || !HostTypeMatchesNestKind(host->TypeId, m_Nest[i].Kind))
        {
            keep = i;
            break;
        }
        if (NestKindUsesSubgraphModel(m_Nest[i].Kind))
        {
            if (!GraphSubgraphStore::TryLoadSubgraph(*host, m_Nest[i].Model))
            {
                m_Nest[i].Model = {};
                m_Nest[i].Model.KindId = m_PanelKindId;
            }
            GraphNodeRegistry::Get().HydrateGraphModelPorts(m_Nest[i].Model);
            parent = &m_Nest[i].Model;
        }
    }
    m_Nest.resize(keep);

    const Graph::Node* nestHost =
        m_Nest.empty() ? nullptr : ActiveGraphModel()->FindNode(m_Nest.back().HostNodeId);

    ApplyNestView(nestHost);
    if (m_Canvas && !NestKindHidesCanvas(CurrentNestKind()))
    {
        m_Canvas->ReconcileStateWithModel();
        m_Canvas->MarkDirty(UIElement::VisualDirty);
    }
    MarkDirty();
    RebuildVariablesPanel();
    return true;
}

Editor::UndoRedoService::InteractiveEdit GraphPanel::BeginGraphParameterEdit(const char* actionName)
{
    if (!m_Undo)
        return {};

    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Graph";
    target.Capture = [this](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        return CaptureGraphSnapshot(out);
    };
    target.Apply = [this](const Editor::UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
    {
        return ApplyGraphSnapshot(in);
    };
    return m_Undo->BeginInteractiveEdit(actionName ? actionName : "Edit Node Parameter", std::move(target));
}

Editor::UndoRedoService::InteractiveEdit GraphPanel::BeginGraphVariableEdit(const char* actionName)
{
    if (!m_Undo)
        return {};

    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Graph";
    target.Capture = [this](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        return CaptureGraphSnapshot(out);
    };
    target.Apply = [this](const Editor::UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
    {
        return ApplyGraphSnapshot(in);
    };
    return m_Undo->BeginInteractiveEdit(actionName ? actionName : "Edit Graph Variable", std::move(target));
}

bool GraphPanel::ApplyNodeParameterValue(const std::string& nodeId, const std::string& key,
                                             const std::string& value)
{
    Graph::Node* node = ActiveGraphModel()->FindNode(nodeId);
    if (!node)
        return false;
    const auto existing = node->Parameters.find(key);
    const bool unchanged = existing != node->Parameters.end() && existing->second.EqualsString(value);
    if (!node->AssignParameterFromText(key, value))
        return false;
    if (unchanged)
        return true;
    if (m_Canvas)
    {
        m_Canvas->RefreshBoundNodeValues();
        m_Canvas->MarkDirty(UIElement::VisualDirty);
    }
    MarkDirty();
    const bool variablesChanged =
        m_KindHooks.OnNodeParamsEdited && m_KindHooks.OnNodeParamsEdited(*node);
    if (variablesChanged || key == "variableName")
        ScheduleVariablesPanelRebuild();
    return true;
}

bool GraphPanel::SetNodeParameter(const std::string& nodeId, const std::string& key,
                                      const std::string& value, bool commitUndo)
{
    if (!m_Undo)
        return ApplyNodeParameterValue(nodeId, key, value);

    const std::string editKey =
        (key == "r" || key == "g" || key == "b") ? std::string("__color__") : key;
    const bool sameGesture = m_GraphParameterEdit &&
                             m_GraphParameterEditNodeId == nodeId &&
                             m_GraphParameterEditKey == editKey;

    if (!commitUndo)
    {
        if (!sameGesture)
        {
            if (m_GraphParameterEdit)
                m_GraphParameterEdit.Commit();
            m_GraphParameterEditNodeId = nodeId;
            m_GraphParameterEditKey = editKey;
            m_GraphParameterEdit = BeginGraphParameterEdit("Edit Node Parameter");
        }

        if (m_GraphParameterEdit)
        {
            m_GraphParameterEdit.Preview([this, nodeId, key, value]() {
                (void)ApplyNodeParameterValue(nodeId, key, value);
            });
            return true;
        }
        return ApplyNodeParameterValue(nodeId, key, value);
    }

    if (sameGesture)
    {
        m_GraphParameterEdit.Preview([this, nodeId, key, value]() {
            (void)ApplyNodeParameterValue(nodeId, key, value);
        });
        m_GraphParameterEdit.Commit();
        m_GraphParameterEdit = {};
        m_GraphParameterEditNodeId.clear();
        m_GraphParameterEditKey.clear();
        return true;
    }

    std::vector<std::uint8_t> before;
    if (!CaptureGraphSnapshot(before))
        return ApplyNodeParameterValue(nodeId, key, value);

    if (!ApplyNodeParameterValue(nodeId, key, value))
        return false;

    std::vector<std::uint8_t> after;
    if (CaptureGraphSnapshot(after) && after != before)
    {
        auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                          "Edit Node Parameter");
        m_Undo->CommitAlreadyApplied(std::move(cmd));
    }
    return true;
}

bool GraphPanel::SetTransitionFromInspector(const std::string& linkId, const GraphTransitionDesc& desc,
                                            bool commitUndo)
{
    auto apply = [this, linkId, desc]() -> bool {
        Graph::Model* model = ActiveGraphModel();
        if (!model)
            return false;
        Graph::Edge* link = model->FindLink(linkId);
        if (!link)
            return false;
        GraphTransitionStore::Store(*link, desc);
        MarkDirty();
        return true;
    };

    if (!m_Undo)
        return apply();

    const bool sameGesture = m_GraphTransitionEdit && m_GraphTransitionEditLinkId == linkId;
    if (!commitUndo)
    {
        if (!sameGesture)
        {
            if (m_GraphTransitionEdit)
                m_GraphTransitionEdit.Commit();
            m_GraphTransitionEditLinkId = linkId;
            m_GraphTransitionEdit = BeginGraphParameterEdit("Edit Transition");
        }
        if (m_GraphTransitionEdit)
        {
            m_GraphTransitionEdit.Preview([apply]() { (void)apply(); });
            return true;
        }
        return apply();
    }

    if (sameGesture)
    {
        m_GraphTransitionEdit.Preview([apply]() { (void)apply(); });
        m_GraphTransitionEdit.Commit();
        m_GraphTransitionEdit = {};
        m_GraphTransitionEditLinkId.clear();
        return true;
    }

    std::vector<std::uint8_t> before;
    if (!CaptureGraphSnapshot(before))
        return apply();
    if (!apply())
        return false;
    std::vector<std::uint8_t> after;
    if (CaptureGraphSnapshot(after) && after != before)
    {
        auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                          "Edit Transition");
        m_Undo->CommitAlreadyApplied(std::move(cmd));
    }
    return true;
}

bool GraphPanel::EditGraphVariableFromInspector(const std::string& name, const std::string& value, bool commitUndo)
{
    return SetGraphVariableValue(name, value, commitUndo);
}

void GraphPanel::SyncViewStateToolbar()
{
    // Generic view state, not the material kind's: the canvas is rebuilt per
    // opened graph, so the panel's expansion state has to be pushed again
    // rather than assumed to have survived.
    if (m_Canvas)
        m_Canvas->SetExpandedNodes(m_ExpandedNodes);
    UpdateExpandedNodesToggleButton();
}

void GraphPanel::ToggleExpandedNodes()
{
    m_ExpandedNodes = !m_ExpandedNodes;
    if (m_Canvas)
        m_Canvas->SetExpandedNodes(m_ExpandedNodes);
    UpdateExpandedNodesToggleButton();
    // Cells only exist while expanded; collapsing releases them so a later
    // expand cannot show a plate belonging to a node that has since gone.
    Notify(GraphPanelEvent::ExpandedNodesToggled);
}

void GraphPanel::UpdateExpandedNodesToggleButton()
{
    if (!m_ExpandedNodesToggleButton)
        return;

    if (m_ExpandedNodes)
        m_ExpandedNodesToggleButton->AddClass("icon-active");
    else
        m_ExpandedNodesToggleButton->RemoveClass("icon-active");

    m_ExpandedNodesToggleButton->SetTooltip(m_ExpandedNodes ? "Expanded nodes: On"
                                                            : "Expanded nodes: Off");
    m_ExpandedNodesToggleButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::ToggleNodePalettePanel()
{
    m_PaletteDragPending = false;
    m_DraggingPalette = false;
    m_PaletteNodeDragPending = false;
    m_DraggingPaletteNode = false;
    m_PaletteNodeDragTypeId.clear();
    EndNodePaletteResize();
    HidePaletteNodeDragGhost();

    m_NodePalettePanelVisible = !m_NodePalettePanelVisible;
    UpdateNodePalettePanelToggleButton();
    if (m_NodePalettePanelVisible)
    {
        m_PaletteHeightOverrideApplied = false;
        m_LastAppliedPaletteHeight = -1.f;
        ApplyNodePaletteBounds();
        SchedulePaletteRebuild();
    }
    else
    {
        UpdatePanelOverflowIndicators();
    }
    MarkPanelToggleDirty(m_Palette, m_Body, m_PaletteScroll);
    RequestRelayout();
}

void GraphPanel::UpdateNodePalettePanelToggleButton()
{
    if (m_Palette)
    {
        m_Palette->Overrides()
            .Set(Style::Display, m_NodePalettePanelVisible ? DisplayMode::Flex : DisplayMode::None);
        m_Palette->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_NodePalettePanelToggleButton)
    {
        if (m_NodePalettePanelVisible)
            m_NodePalettePanelToggleButton->AddClass("icon-active");
        else
            m_NodePalettePanelToggleButton->RemoveClass("icon-active");

        m_NodePalettePanelToggleButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void GraphPanel::ToggleVariablesPanel()
{
    m_VariablesDragPending = false;
    m_DraggingVariablesPanel = false;
    EndVariablesPanelResize();

    m_VariablesPanelVisible = !m_VariablesPanelVisible;
    UpdateVariablesPanelToggleButton();
    if (m_VariablesPanelVisible)
    {
        m_VariablesHeightOverrideApplied = false;
        m_LastAppliedVariablesHeight = -1.f;
        ApplyVariablesPanelBounds();
        ScheduleVariablesPanelRebuild();
    }
    else
    {
        UpdatePanelOverflowIndicators();
    }
    MarkPanelToggleDirty(m_VariablesPanel, m_Body, m_VariablesScroll);
    RequestRelayout();
}

void GraphPanel::UpdateVariablesPanelToggleButton()
{
    if (m_VariablesPanel)
    {
        m_VariablesPanel->Overrides()
            .Set(Style::Display, m_VariablesPanelVisible ? DisplayMode::Flex : DisplayMode::None);
        m_VariablesPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_VariablesPanelToggleButton)
    {
        if (m_VariablesPanelVisible)
            m_VariablesPanelToggleButton->AddClass("icon-active");
        else
            m_VariablesPanelToggleButton->RemoveClass("icon-active");

        m_VariablesPanelToggleButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void GraphPanel::TogglePaletteNodeColors()
{
    m_ColorPaletteNodes = !m_ColorPaletteNodes;
    UpdatePaletteNodeColorToggleButton();

    auto apply = [this]()
    {
        SaveColorPaletteNodesPreference(m_ColorPaletteNodes);
        RebuildPalette();
        if (m_PaletteScroll)
            m_PaletteScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        if (m_Palette)
            m_Palette->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        this->UIElement::MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    };

    if (Scheduler::IScheduler* sched = GetScheduler())
        sched->ScheduleNext(std::move(apply));
    else
        apply();
}

void GraphPanel::UpdatePaletteNodeColorToggleButton()
{
    if (!m_PaletteNodeColorToggleButton)
        return;

    if (m_ColorPaletteNodes)
        m_PaletteNodeColorToggleButton->AddClass("icon-active");
    else
        m_PaletteNodeColorToggleButton->RemoveClass("icon-active");

    m_PaletteNodeColorToggleButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::SetOpenColorPickerWindow(OpenColorPickerWindowFn fn)
{
    m_OpenColorPickerWindow = std::move(fn);
    /* Node-side swatches open the same window as the variables board's; which
       nodes have one is the kind's business, not the canvas's. */
    if (m_KindHooks.OnColorPickerChanged)
        m_KindHooks.OnColorPickerChanged();
}

void GraphPanel::SyncConnectionStyleButton()
{
    if (!m_ConnectionStyleButton || !m_Canvas)
        return;

    /* The icon shows the style currently in use; clicking swaps to the other. */
    const bool straight = m_Canvas->GetUseStraightLines();
    m_ConnectionStyleButton->AddClass(straight ? "connection-straight-icon" : "connection-curved-icon");
    m_ConnectionStyleButton->RemoveClass(straight ? "connection-curved-icon" : "connection-straight-icon");
    m_ConnectionStyleButton->SetTooltip(straight ? "Straight connections (click for curved)"
                                                 : "Curved connections (click for straight)");
    m_ConnectionStyleButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void GraphPanel::Listen(GraphPanelEvent e, std::function<void()> handler)
{
    const std::size_t index = static_cast<std::size_t>(e);
    if (!handler || index >= static_cast<std::size_t>(GraphPanelEvent::Count))
        return;
    m_EventListeners[index].push_back(std::move(handler));
}

void GraphPanel::Notify(GraphPanelEvent e)
{
    const std::size_t index = static_cast<std::size_t>(e);
    if (index >= static_cast<std::size_t>(GraphPanelEvent::Count))
        return;
    for (const std::function<void()>& handler : m_EventListeners[index])
        handler();
}

void GraphPanel::MarkDirty()
{
    m_Dirty = true;
    UpdateTitleLabel();
    Notify(GraphPanelEvent::ModelChanged);
}

void GraphPanel::CommitLiveGraphMutation(const char* actionName, std::function<void()> mutate)
{
    if (!m_Undo)
    {
        if (mutate)
            mutate();
        return;
    }
    std::vector<std::uint8_t> before;
    if (!CaptureGraphSnapshot(before))
    {
        if (mutate)
            mutate();
        return;
    }
    if (mutate)
        mutate();
    std::vector<std::uint8_t> after;
    if (!CaptureGraphSnapshot(after))
        return;
    auto cmd = std::make_unique<GraphSnapshotCommand>(this, std::move(before), std::move(after),
                                                      actionName ? actionName : "Edit Graph");
    m_Undo->CommitAlreadyApplied(std::move(cmd));
}

void GraphPanel::OnCanvasSelectionChanged(const std::string& nodeId)
{
    Logger::Log::Info("GraphPanel::OnCanvasSelectionChanged called with nodeId='{}'", nodeId);
    
    if (!m_Canvas) {
        Logger::Log::Warning("GraphPanel: m_Canvas is null");
        return;
    }
    
    if (!m_Canvas->GetModel()) {
        Logger::Log::Warning("GraphPanel: m_Canvas->GetModel() is null");
        return;
    }
    
    if (nodeId.empty()) {
        // A legitimate deselect (click on empty canvas): whoever shows the
        // node outside the panel must drop it too, or it goes stale.
        if (m_OnNodeDeselected)
            m_OnNodeDeselected();
        return;
    }

    Graph::Node* node = m_Canvas->GetModel()->FindNode(nodeId);
    if (!node) {
        Logger::Log::Warning("GraphPanel: FindNode returned null for nodeId='{}'", nodeId);
        return;
    }

    // Get display name from type registry
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(m_Model.KindId, node->TypeId);
    std::string displayName = meta ? meta->DisplayName : node->TypeId;

    Logger::Log::Info("GraphPanel: Found node displayName='{}' typeId='{}' parameters={}",
                      displayName, node->TypeId, node->Parameters.size());

    // Invoke callback if set
    if (m_OnNodeSelected)
    {
        Logger::Log::Info("GraphPanel: Invoking m_OnNodeSelected callback for '{}'", displayName);
        auto inspectorParameters = GraphNodeRegistry::Get().GetDefaultParameters(m_Model.KindId, node->TypeId);
        for (const auto& [key, value] : node->Parameters)
            inspectorParameters[key] = value.ToString();
        if (node->TypeId == "ColorParameter")
        {
            auto varIt = node->Parameters.find("variableName");
            if (varIt != node->Parameters.end())
            {
                for (const Graph::Variable& variable : m_Model.Variables)
                {
                    if (variable.Name != varIt->second.ToString())
                        continue;
                    const std::vector<float> components = ParseGraphVariableVectorValue(variable.Value, 3);
                    inspectorParameters["r"] = FormatGraphVariableFloat(components[0]);
                    inspectorParameters["g"] = FormatGraphVariableFloat(components[1]);
                    inspectorParameters["b"] = FormatGraphVariableFloat(components[2]);
                    break;
                }
            }
        }
        else if (node->TypeId == "FloatParameter")
        {
            auto varIt = node->Parameters.find("variableName");
            if (varIt != node->Parameters.end())
            {
                for (const Graph::Variable& variable : m_Model.Variables)
                {
                    if (variable.Name == varIt->second.ToString())
                    {
                        inspectorParameters["value"] = variable.Value;
                        break;
                    }
                }
            }
        }
        else if (node->TypeId == "Vec2Parameter" || node->TypeId == "Vec3Parameter" ||
                 node->TypeId == "Vec4Parameter")
        {
            auto varIt = node->Parameters.find("variableName");
            if (varIt != node->Parameters.end())
            {
                const int componentCount = node->TypeId == "Vec2Parameter"   ? 2
                                           : node->TypeId == "Vec3Parameter" ? 3
                                                                             : 4;
                for (const Graph::Variable& variable : m_Model.Variables)
                {
                    if (variable.Name != varIt->second.ToString())
                        continue;
                    const std::vector<float> components =
                        ParseGraphVariableVectorValue(variable.Value, componentCount);
                    static const char* kKeys[] = {"x", "y", "z", "w"};
                    for (int i = 0; i < componentCount; ++i)
                        inspectorParameters[kKeys[i]] = FormatGraphVariableFloat(components[static_cast<size_t>(i)]);
                    break;
                }
            }
        }
        g_InspectorGraphPanel = this;
        m_OnNodeSelected(nodeId, node->TypeId, displayName, inspectorParameters, m_PanelKindId);
    }
    else
    {
        Logger::Log::Warning("GraphPanel: m_OnNodeSelected callback is NULL, cannot show node '{}'", displayName);
    }
}

void GraphPanel::OnCanvasLinkSelected(const std::string& linkId)
{
    g_InspectorGraphPanel = this;
    if (!m_OnTransitionSelected)
        return;
    Graph::Model* model = ActiveGraphModel();
    Graph::Edge* link = (model && !linkId.empty()) ? model->FindLink(linkId) : nullptr;
    if (!link || !GraphTransitionStore::IsStateTransition(*model, *link))
    {
        m_OnTransitionSelected({}, {}, {}, {});
        return;
    }

    auto stateName = [](const Graph::Node* node, const std::string& fallback) {
        if (!node)
            return fallback;
        const std::string title = node->Parameters.GetString("title");
        return title.empty() ? node->TypeId : title;
    };
    const Graph::Node* source = model->FindNode(link->SourceNodeId);
    const Graph::Node* target = model->FindNode(link->TargetNodeId);
    m_OnTransitionSelected(linkId, stateName(source, link->SourceNodeId),
                           stateName(target, link->TargetNodeId), GraphTransitionStore::Load(*link));
}

void GraphPanel::OnCanvasContextMenu(float screenX, float screenY, bool onNode, const std::string& nodeId)
{
    if (!m_Context || !m_Context->MainWindow || !m_Canvas)
        return;

    if (!m_ContextMenu)
        m_ContextMenu = CreateContextMenu();
    if (!m_ContextMenu)
        return;

    m_ContextMenu->Clear();
    m_CanvasContextMenuItems.clear();
    m_CanvasContextMenuScreenX = screenX;
    m_CanvasContextMenuScreenY = screenY;

    ContextMenuBuilder builder;
    constexpr uint32_t kCmdDeleteNode = 42001;
    constexpr uint32_t kCmdAddNodeBase = 42100;

    if (onNode && !nodeId.empty())
    {
        if (!m_Canvas->IsNodeSelected(nodeId))
            m_Canvas->SetSelectedNodeId(nodeId);
        builder.AddItem("Delete Node", kCmdDeleteNode, MenuItemFlag_None, 0, EditorIcons::kTrash);
    }
    else
    {
        const std::vector<std::string> categories = GraphNodeRegistry::Get().GetCategories(m_Model.KindId);
        for (const std::string& category : categories)
        {
            if (!IsPaletteCategoryVisible(category))
                continue;
            const std::vector<NodeTypeMeta> types =
                GraphNodeRegistry::Get().GetTypesInCategory(m_Model.KindId, category);
            for (const NodeTypeMeta& meta : types)
            {
                if (meta.TypeId == "Group" || meta.TypeId == "Comment" || meta.TypeId == "EngineInput")
                    continue;
                const uint32_t commandId =
                    kCmdAddNodeBase + static_cast<uint32_t>(m_CanvasContextMenuItems.size());
                m_CanvasContextMenuItems.push_back({commandId, meta.TypeId});
                const std::string path =
                    category.empty() ? meta.DisplayName : (category + "/" + meta.DisplayName);
                builder.AddItem(path, commandId, MenuItemFlag_None, 0,
                                MenuIconPathForStem(meta.IconStem));
            }
        }
    }

    m_ContextMenu->SetCommandHandler([this](uint32_t commandId)
    {
        if (commandId == kCmdDeleteNode)
        {
            /* Hide-first + deferred mutation; undo lands through the canvas's
               undo scope, graph-changed brings the variables panel and compile
               along. */
            if (m_Canvas && m_Canvas->GetModel())
                m_Canvas->DeleteSelectedNodes();
            return;
        }
        if (commandId < kCmdAddNodeBase)
            return;
        const auto it = std::find_if(m_CanvasContextMenuItems.begin(), m_CanvasContextMenuItems.end(),
                                     [commandId](const CanvasContextMenuItem& item)
                                     { return item.CommandId == commandId; });
        if (it == m_CanvasContextMenuItems.end())
            return;
        AddNodeFromPaletteAtScreen(it->TypeId, m_CanvasContextMenuScreenX, m_CanvasContextMenuScreenY,
                                   true, true);
    });

    if (onNode && !nodeId.empty())
        builder.Build(m_ContextMenu.get());
    else if (!m_CanvasContextMenuItems.empty())
        builder.Build(m_ContextMenu.get());
    else
    {
        m_ContextMenu->AddItem(0, "No nodes available", 1, MenuItemFlag_Disabled);
        m_ContextMenu->SetItemIcon(1, EditorIcons::kInfo);
    }

    m_ContextMenu->Show(m_Context->MainWindow, static_cast<int>(screenX), static_cast<int>(screenY));
}

void GraphPanel::OnLoadGraphButtonClicked()
{
    std::filesystem::path initialPath;
    if (m_Context && !m_Context->AssetsRoot.empty())
    {
        initialPath = m_KindHooks.DefaultSaveDirectory
                          ? m_KindHooks.DefaultSaveDirectory(m_Context->AssetsRoot)
                          : m_Context->AssetsRoot;
    }
    else if (!m_CurrentPath.empty())
        initialPath = m_CurrentPath.parent_path();

    const std::filesystem::path selected =
        Platform::SelectFile(initialPath, OpenGraphsFilterName().c_str(), OpenGraphsFilterPattern().c_str());
    if (selected.empty())
        return;

    if (m_Dirty)
    {
        m_PendingLoadGraphPath = selected;
        m_HasPendingLoadGraph = true;
        EnsureUnsavedChangesModal();
        if (m_UnsavedChangesModal)
        {
            const std::string filename = m_CurrentPath.empty()
                ? UntitledGraphTitle(m_PanelKindId)
                : m_CurrentPath.filename().string();
            m_UnsavedChangesModal->Show(
                "Unsaved Graph Changes",
                "\"" + filename + "\" has unsaved changes.\nSave before loading another graph?");
        }
        return;
    }

    (void)OpenGraph(selected);
}

void GraphPanel::OnCanvasWireDropMenu(float screenX, float screenY,
                                          const std::string& anchorNodeId, const std::string& anchorPortId,
                                          bool fromInputPort)
{
    if (!m_Context || !m_Context->MainWindow || !m_Canvas || !m_Canvas->GetModel())
        return;

    Graph::Model* model = m_Canvas->GetModel();
    const Graph::Node* anchorNode = model->FindNode(anchorNodeId);
    const Graph::Port* anchorPort = nullptr;
    if (anchorNode)
    {
        for (const auto& port : anchorNode->Ports)
        {
            if (port.Id == anchorPortId)
            {
                anchorPort = &port;
                break;
            }
        }
    }
    if (!anchorNode || !anchorPort)
        return;
    if (fromInputPort)
    {
        if (anchorPort->Direction != Graph::PortDirection::In)
            return;
    }
    else if (anchorPort->Direction != Graph::PortDirection::Out)
    {
        return;
    }

    if (!m_ContextMenu)
        m_ContextMenu = CreateContextMenu();
    if (!m_ContextMenu)
        return;

    m_ContextMenu->Clear();
    m_WireDropMenuItems.clear();
    m_WireDropAnchorNodeId = anchorNodeId;
    m_WireDropAnchorPortId = anchorPortId;
    m_WireDropFromInputPort = fromInputPort;
    m_WireDropScreenX = screenX;
    m_WireDropScreenY = screenY;

    constexpr uint32_t kWireDropCommandBase = 42000;
    ContextMenuBuilder builder;
    const std::vector<std::string> categories = GraphNodeRegistry::Get().GetCategories(model->KindId);
    for (const std::string& category : categories)
    {
        if (!IsPaletteCategoryVisible(category))
            continue;
        const std::vector<NodeTypeMeta> types = GraphNodeRegistry::Get().GetTypesInCategory(model->KindId, category);
        for (const NodeTypeMeta& meta : types)
        {
            std::string compatiblePortId;
            for (const NodePortTemplate& port : meta.Ports)
            {
                if (fromInputPort)
                {
                    if (port.Direction == Graph::PortDirection::Out &&
                        AreNodeGraphPortTypesCompatible(port.DataType, anchorPort->DataType))
                    {
                        compatiblePortId = port.Id;
                        break;
                    }
                }
                else if (port.Direction == Graph::PortDirection::In &&
                         AreNodeGraphPortTypesCompatible(anchorPort->DataType, port.DataType))
                {
                    compatiblePortId = port.Id;
                    break;
                }
            }
            if (compatiblePortId.empty())
                continue;

            const uint32_t commandId = kWireDropCommandBase + static_cast<uint32_t>(m_WireDropMenuItems.size());
            m_WireDropMenuItems.push_back({ commandId, meta.TypeId, compatiblePortId, fromInputPort });
            const std::string path = category.empty() ? meta.DisplayName : (category + "/" + meta.DisplayName);
            builder.AddItem(path, commandId, MenuItemFlag_None, 0,
                            MenuIconPathForStem(meta.IconStem));
        }
    }

    m_ContextMenu->SetCommandHandler([this](uint32_t commandId)
    {
        const auto it = std::find_if(m_WireDropMenuItems.begin(), m_WireDropMenuItems.end(),
            [commandId](const WireDropMenuItem& item) { return item.CommandId == commandId; });
        if (it == m_WireDropMenuItems.end())
            return;
        if (m_Canvas)
            m_Canvas->SetWireDropMenuOpen(false);
        AddNodeFromWireDropAtScreen(m_WireDropAnchorNodeId, m_WireDropAnchorPortId,
                                    it->TypeId, it->ConnectPortId,
                                    m_WireDropScreenX, m_WireDropScreenY,
                                    m_WireDropFromInputPort);
    });

    if (m_WireDropMenuItems.empty())
    {
        m_ContextMenu->AddItem(0, "No compatible nodes", 1, MenuItemFlag_Disabled);
        m_ContextMenu->SetItemIcon(1, EditorIcons::kInfo);
    }
    else
        builder.Build(m_ContextMenu.get());

    /* The wire stays drawn from its port to the menu for as long as the menu is
       up, and is retired by the menu's own close callback — Show returns before
       the menu closes on this platform, so nothing else can time it. */
    m_ContextMenu->SetCloseHandler([this]() {
        if (m_Canvas)
            m_Canvas->SetWireDropMenuOpen(false);
    });
    /* Armed after Show: opening a menu tears down whatever was open first, and
       that teardown reports a close — arming before it would be undone in the
       same breath. */
    m_ContextMenu->Show(m_Context->MainWindow, static_cast<int>(screenX), static_cast<int>(screenY));
    m_Canvas->SetWireDropMenuOpen(true);
}

bool GraphPanel::OpenGraph(const std::filesystem::path& path)
{
    if (path.empty())
        return false;
    Graph::Model loaded;
    if (!LoadGraphModelFromPath(path, loaded))
    {
        Logger::Log::Error("GraphPanel: Cannot open or parse graph file: {}", path.string());
        return false;
    }
    if (loaded.KindId != m_PanelKindId)
    {
        if (!m_RequestOpenGraphAsset || m_ForwardingOpenGraph)
        {
            Logger::Log::Error("GraphPanel: {} is kind '{}' but this panel is '{}'",
                               path.string(), loaded.KindId, m_PanelKindId);
            return false;
        }
        m_ForwardingOpenGraph = true;
        const bool opened = m_RequestOpenGraphAsset(path);
        m_ForwardingOpenGraph = false;
        return opened;
    }
    ExitAllNests(true);
    m_Model = std::move(loaded);
    m_Model.KindId = m_PanelKindId;
    AssignMissingVariableCreatedOrders();
    if (m_KindHooks.SyncGraphVariables)
        m_KindHooks.SyncGraphVariables();
    m_CurrentPath = path;
    m_Dirty = false;
    m_FrameNodesOnNextLayout = true; /* Frame all nodes to fit view when layout is ready */
    m_FrameRetries = 0;
    if (m_Canvas)
    {
        SyncCanvasNodeFactory();
        m_Canvas->SetModel(&m_Model);
        m_Canvas->SetSelectedNodeId(std::string());
        m_Canvas->MarkDirty(VisualDirty);
    }
    // SetSelectedNodeId is programmatic and fires no selection callback, so
    // tell the Inspector explicitly: the node it shows belongs to the graph
    // this open just replaced.
    if (m_OnNodeDeselected)
        m_OnNodeDeselected();
    SyncConnectionStyleButton();
    UpdateTitleLabel();
    SchedulePaletteRebuild();
    ScheduleVariablesPanelRebuild();
    Notify(GraphPanelEvent::Opened);
    Notify(GraphPanelEvent::Shown);
    NoteAsLastOpened();
    return true;
}

std::filesystem::path GraphPanel::PromptSaveAsPath()
{
    const GraphKindFileFilter filter = SaveFilterForKind(m_PanelKindId);

    std::filesystem::path initialPath;
    if (!m_CurrentPath.empty())
    {
        initialPath = m_CurrentPath;
    }
    else if (m_Context && !m_Context->AssetsRoot.empty())
    {
        const std::filesystem::path dir = m_KindHooks.DefaultSaveDirectory
                                          ? m_KindHooks.DefaultSaveDirectory(m_Context->AssetsRoot)
                                          : m_Context->AssetsRoot;
        initialPath = dir / (std::string("Untitled") + filter.DefaultExtension);
    }

    std::filesystem::path chosen =
        Platform::SaveFile(initialPath, filter.Name.c_str(), filter.Pattern.c_str());
    if (chosen.empty())
        return {};

    // Whatever the dialog returned, the file carries the kind's own extension:
    // the filter above already names it, so no kind needs testing for here.
    if (!filter.DefaultExtension.empty() && chosen.extension() != filter.DefaultExtension)
        chosen.replace_extension(filter.DefaultExtension);
    else if (chosen.extension().empty())
    {
        chosen.replace_extension(filter.DefaultExtension);
    }
    return chosen;
}

bool GraphPanel::SaveGraph()
{
    if (m_CurrentPath.empty())
    {
        const std::filesystem::path chosen = PromptSaveAsPath();
        if (chosen.empty())
            return false;
        m_CurrentPath = chosen;
    }
    WriteLiveNestToHosts();
    SyncViewportToModel();
    if (m_KindHooks.OnBeforeSave)
        m_KindHooks.OnBeforeSave(m_Model);
    if (!SaveGraphToPath(m_Model, m_CurrentPath))
    {
        Logger::Log::Error("GraphPanel: Failed to save graph: {}", m_CurrentPath.string());
        return false;
    }
    m_Dirty = false;
    UpdateTitleLabel();
    Notify(GraphPanelEvent::Saved);
    if (m_OnGraphSaved)
        m_OnGraphSaved(m_CurrentPath);
    return true;
}

} // namespace GameEngine
