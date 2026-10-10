#include "Inspectors/RenderPipelineInspector.h"
#include "AssetCore/SharedFileRead.h"

#include "InspectorRegistry.h"

#include "Assets/RenderPipelineAsset.h"

#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/EditorIcons.h"
#include "Logger/Logger.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Payload.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "UI/Controls/Label.h"
#include "UI/Controls/ReorderableSectionList.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Panels/SettingsPanel.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"
#include "Types/StringUtils.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine {

using InspectorUI::AddTextBlock;

namespace {

// Cached host window for native context menus. Set once per inspector build.
static Platform::Window* s_HostWindow = nullptr;
static Editor::UndoRedoService* s_UndoService = nullptr;

// Drag/drop payload: a string reference to a render graph resource (e.g. "ClusterParams")
// or other ref-style strings used in pipeline authoring (e.g. "View.Depth").
struct RenderGraphRefPayload
{
    std::string text;
};

static UI::Interaction::DragPayload MakeRenderGraphRefDragPayload(const std::string& text)
{
    UI::Interaction::DragPayload p = UI::Interaction::DragPayload::Create(RenderGraphRefPayload{text});
    p.DisplayLabel = text;
    p.GhostIconKind = UI::Interaction::DragGhostIconKind::None;
    return p;
}

class DropTextField final : public TextField, public UI::Interaction::IDropTarget
{
  public:
    DropTextField()
    {
        AddClass("drop-text-field");
    }

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override
    {
        return typeId == UI::Interaction::GetPayloadTypeId<RenderGraphRefPayload>();
    }

    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override
    {
        if (!ContainsPoint(x, y))
            return false;
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnItem;
        out.IndentDepth = 0;
        return true;
    }

    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override
    {
        UI::Interaction::DropFeedback fb{};
        const auto* p = request.payload.TryGet<RenderGraphRefPayload>();
        if (!p || p->text.empty())
        {
            fb.Allowed = false;
            fb.Reason = "empty payload";
            return fb;
        }
        fb.Allowed = true;
        return fb;
    }

    void PerformDrop(const UI::Interaction::DropRequest& request) override
    {
        const auto* p = request.payload.TryGet<RenderGraphRefPayload>();
        if (!p)
            return;
        SetValue(p->text);
        // Programmatic SetValue does not fire ValueChanged; emulate a user commit.
        NotifyValueChanged();
    }

    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override
    {
        if (!state.Visible)
        {
            RemoveClass("drop-preview-ok");
            RemoveClass("drop-preview-bad");
            return;
        }

        if (state.Allowed)
        {
            AddClass("drop-preview-ok");
            RemoveClass("drop-preview-bad");
        }
        else
        {
            AddClass("drop-preview-bad");
            RemoveClass("drop-preview-ok");
        }
    }
};

static std::unique_ptr<Label> MakeDraggableRefLabel(const std::string& text, const std::string& id)
{
    auto lbl = std::make_unique<Label>();
    lbl->AddClass("draggable-ref-label");
    lbl->SetText(text);
    if (!id.empty())
        lbl->SetId(id);

    struct DragState
    {
        bool candidate = false;
        float startX = 0.0f;
        float startY = 0.0f;
        std::string text;
    };
    auto st = std::make_shared<DragState>();
    st->text = text;

    Label* raw = lbl.get();
    raw->RegisterEventHandler(kEventMouseDown, [st, raw](UIEvent& e)
                              {
                                  if (e.Button != 0)
                                      return;
                                  st->candidate = true;
                                  st->startX = e.X;
                                  st->startY = e.Y;
                                  e.Capture(raw);
                                  e.Stop();
                              });

    raw->RegisterEventHandler(kEventMouseMove, [st, raw](UIEvent& e)
                              {
                                  if (!st->candidate)
                                      return;
                                  constexpr float kDragThresholdPx = 6.0f;
                                  const float dx = e.X - st->startX;
                                  const float dy = e.Y - st->startY;
                                  if ((dx * dx + dy * dy) < (kDragThresholdPx * kDragThresholdPx))
                                      return;

                                  if (UIManager* ui = raw->GetOwnerManager())
                                  {
                                      if (auto* dd = ui->GetDragDropManager())
                                      {
                                          UI::Interaction::DragPayload payload = MakeRenderGraphRefDragPayload(st->text);
                                          if (payload.IsValid())
                                          {
                                              UI::Interaction::DragSessionContext ctx{};
                                              ctx.SourceWidgetId = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(raw));
                                              dd->BeginDrag(std::move(payload), ctx);
                                          }
                                      }
                                  }

                                  st->candidate = false;
                                  e.Stop();
                              });

    raw->RegisterEventHandler(kEventMouseUp, [st](UIEvent& e)
                              {
                                  if (e.Button != 0)
                                      return;
                                  st->candidate = false;
                              });

    return lbl;
}

static std::string Join(const std::vector<std::string>& items, const char* sep)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i && sep)
            out += sep;
        out += items[i];
    }
    return out;
}

static std::string FormatShaderPkgValidation(const std::string& shaderPkgPathOrName,
                                              const std::vector<std::string>& inputBindingNames)
{
    using namespace GameEngine::Rendering;

    ShaderPackage pkg{};
    std::string err;
    // Reflection meta only — the stage bytes are never read here, so the form
    // they are served in does not matter.
    if (!LoadShaderPkg(shaderPkgPathOrName, ShaderSourceKind::SpirV, pkg, &err))
    {
        return "shaderPkg: " + shaderPkgPathOrName + "\nERROR: " + err;
    }

    std::ostringstream oss;
    oss << "shaderPkg: " << shaderPkgPathOrName << "\n";

    // Validate meta itself (push constant policy, etc.)
    const auto report = ValidateShaderMeta(pkg.meta, 128);
    if (!report.Issues.empty())
    {
        oss << "Meta validation:\n";
        for (const auto& i : report.Issues)
        {
            const char* sev = (i.Severity == IssueSeverity::Error) ? "ERROR" : (i.Severity == IssueSeverity::Warning) ? "WARN" : "INFO";
            oss << " - [" << sev << "] " << i.Code << ": " << i.Message << "\n";
        }
    }

    // Validate declared input names against set 0 bindings by name.
    if (!inputBindingNames.empty())
    {
        std::unordered_set<std::string> set0Names;
        for (const auto& s : pkg.meta.Sets)
        {
            if (s.Set != 0)
                continue;
            for (const auto& b : s.Bindings)
            {
                if (!b.Name.empty())
                    set0Names.insert(b.Name);
            }
            break;
        }

        std::vector<std::string> missing;
        for (const auto& n : inputBindingNames)
        {
            if (set0Names.find(n) == set0Names.end())
                missing.push_back(n);
        }
        if (!missing.empty())
        {
            oss << "Missing bindings (set 0) for inputs: " << Join(missing, ", ") << "\n";
        }
    }

    return oss.str();
}

static void SetActiveRenderPipelineInProjectSettings(const std::filesystem::path& pipelineAssetPath)
{
    // Pipeline path is stored as asset-root-relative (e.g. "RenderPipelines/ForwardPlus.rendergraph").
    // IMPORTANT: don't serialize absolute build-output paths (Debug/RelWithDebInfo) into project settings.
    auto& am = EngineCore::GetInstance().GetAssetManager();
    auto tryRel = [&](const std::filesystem::path& root) -> std::string
    {
        if (root.empty() || pipelineAssetPath.empty())
            return {};
        std::error_code ec;
        auto rel = std::filesystem::relative(pipelineAssetPath, root, ec);
        if (ec)
            return {};
        const std::string s = rel.generic_string();
        if (s.empty() || s.rfind("..", 0) == 0)
            return {};
        return s;
    };
    std::string value = tryRel(am.GetAssetRoot());
    if (value.empty())
    {
        for (const auto& source : am.GetRegisteredSources())
        {
            value = tryRel(source.Root);
            if (!value.empty())
                break;
        }
    }
    if (value.empty())
    {
        // Fall back, but keep it visible in logs so it's easier to diagnose.
        value = pipelineAssetPath.generic_string();
        Logger::Log::Warning("RenderPipelineInspector: storing non-relative pipeline path in settings: {}", value);
    }

    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    auto& r = root["rendering"];
    if (!r.is_object())
        r = nlohmann::json::object();
    r["activeRenderPipeline"] = value;

    if (!store.Save(&err))
    {
        Logger::Log::Error("RenderPipelineInspector: failed to save active pipeline setting: {}", err);
    }
    else
    {
        Logger::Log::Info("RenderPipelineInspector: saved active render pipeline: {}", value);
    }

    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        rs->Spine().SetActiveRenderPipelinePath(std::filesystem::path(value));
    }

    SettingsPanel::NotifyActivePipelineChanged(value);
}

class PassTypeSearchProvider final : public ISearchProvider
{
  public:
    explicit PassTypeSearchProvider(std::vector<std::string> types)
        : m_Types(std::move(types))
    {
    }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        std::vector<SearchResultItem> results;
        std::string lowerQuery;
        lowerQuery.reserve(query.size());
        for (char c : query)
            lowerQuery += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        SearchItemId nextId = 1;
        for (const auto& typeName : m_Types)
        {
            std::string lowerType;
            lowerType.reserve(typeName.size());
            for (char c : typeName)
                lowerType += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (!lowerQuery.empty() && lowerType.find(lowerQuery) == std::string::npos)
                continue;

            SearchResultItem item;
            item.Id = nextId++;
            item.Label = typeName;
            item.UserData = typeName;
            results.push_back(std::move(item));
        }

        sink(std::move(results), true);
    }

    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search pass type..."; }

  private:
    std::vector<std::string> m_Types;
};

static std::string GenerateDefaultPassId(const std::string& typeName, const nlohmann::json& passes)
{
    std::string base = typeName;
    if (base.size() > 6 && base.substr(base.size() - 6) == "Render")
        base = base.substr(0, base.size() - 6);
    else if (base.size() > 6 && base.substr(base.size() - 6) == "Shader")
        base = base.substr(0, base.size() - 6);
    else if (base.size() > 6 && base.substr(base.size() - 6) == "Upload")
        base = base.substr(0, base.size() - 6);

    if (base.empty())
        base = typeName;

    auto idExists = [&](const std::string& candidate)
    {
        for (const auto& p : passes)
        {
            if (p.is_object() && p.value("id", "") == candidate)
                return true;
        }
        return false;
    };

    if (!idExists(base))
        return base;

    for (int n = 2; n < 100; ++n)
    {
        std::string candidate = base + std::to_string(n);
        if (!idExists(candidate))
            return candidate;
    }

    return base + "_new";
}

static nlohmann::json MakeDefaultPassForType(const std::string& typeName, const nlohmann::json& passes)
{
    nlohmann::json p = nlohmann::json::object();
    p["id"] = GenerateDefaultPassId(typeName, passes);
    p["type"] = typeName;
    p["enabled"] = true;
    return p;
}

static std::unordered_map<std::string, bool>& GetFoldoutStateMap()
{
    static std::unordered_map<std::string, bool> s_State;
    return s_State;
}

static void ClearChildren(UIElement* root)
{
    if (!root)
        return;
    root->RemoveAllChildren();
}

static void BuildRenderPipelineEditorUI(UIElement* root,
                                        RenderPipelineAsset* pipe,
                                        Editor::UndoRedoService* undo);

static void RequestRebuild(UIElement* root,
                           RenderPipelineAsset* pipe,
                           Editor::UndoRedoService* undo = s_UndoService)
{
    if (!root || !pipe)
        return;
    auto action = [root, pipe, undo]()
    {
        ScrollView* scrollAncestor = nullptr;
        for (UIElement* p = root->GetParent(); p && !scrollAncestor; p = p->GetParent())
            scrollAncestor = dynamic_cast<ScrollView*>(p);
        const float savedScrollY = scrollAncestor ? scrollAncestor->GetScrollY() : 0.0f;

        ClearChildren(root);
        BuildRenderPipelineEditorUI(root, pipe, undo);

        if (scrollAncestor)
            scrollAncestor->SetScrollY(savedScrollY);
    };
    if (UIElement::IsInEventDispatch())
        root->PostAction(std::move(action));
    else
        action();
}

static void SaveJsonToDisk(RenderPipelineAsset* pipe, const nlohmann::json& j)
{
    if (!pipe)
        return;
    bool wrote = false;
    try
    {
        std::ofstream out(pipe->GetPath(), std::ios::binary);
        if (!out.is_open())
            return;
        out << j.dump(2);
        wrote = true;
    }
    catch (...)
    {
    }

    // Wake the VCS poll; poll-thread cooldown absorbs rapid-fire edits.
    if (wrote)
    {
        if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
            vcs->RefreshStatus();
    }
}

static void SaveReloadRebuild(UIElement* root, RenderPipelineAsset* pipe, const nlohmann::json& j)
{
    SaveJsonToDisk(pipe, j);
    if (pipe)
    {
        (void)pipe->Reload();
    }
    RequestRebuild(root, pipe, s_UndoService);
}

static Editor::UndoRedoService::SnapshotTarget MakeRenderPipelineSnapshotTarget(RenderPipelineAsset* pipe,
                                                                                const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [pipe](Editor::UndoRedoService::SnapshotTarget::Snapshot& outSnapshot) -> bool
    {
        if (!pipe)
            return false;
        std::string text = pipe->GetDocument().jsonText;
        if (text.empty())
        {
            GameEngine::String fileText;
            if (!GameEngine::ReadFileTextShared(pipe->GetPath(), fileText))
                return false;
            text = std::move(fileText);
        }
        outSnapshot.assign(text.begin(), text.end());
        return true;
    };

    target.Apply = [pipe](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot) -> bool
    {
        if (!pipe)
            return false;
        try
        {
            std::ofstream out(pipe->GetPath(), std::ios::binary);
            if (!out.is_open())
                return false;
            if (!snapshot.empty())
                out.write(reinterpret_cast<const char*>(snapshot.data()),
                          static_cast<std::streamsize>(snapshot.size()));
        }
        catch (...)
        {
            return false;
        }

        (void)pipe->Reload();
        if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
            vcs->RefreshStatus();
        return true;
    };

    return target;
}

static void SaveReloadRebuildUndoable(UIElement* root,
                                      RenderPipelineAsset* pipe,
                                      const nlohmann::json& j,
                                      Editor::UndoRedoService* undo,
                                      const std::string& label)
{
    if (undo && pipe)
    {
        Editor::UndoRedoService::InteractiveEdit edit =
            undo->BeginInteractiveEdit(std::string("Render Pipeline ") + label,
                                       MakeRenderPipelineSnapshotTarget(pipe, label));
        if (edit)
        {
            SaveJsonToDisk(pipe, j);
            (void)pipe->Reload();
            edit.Commit();
            RequestRebuild(root, pipe, undo);
            return;
        }
    }
    SaveReloadRebuild(root, pipe, j);
}

static Label* AddRowLabel(UIElement* row, const std::string& text)
{
    if (!row)
        return nullptr;
    auto cell = std::make_unique<UIElement>();
    cell->AddClass("inspector-label-cell");
    auto label = std::make_unique<Label>();
    label->AddClass("settings-row-label");
    label->AddClass("inspector-label");
    label->SetText(text);
    Label* raw = label.get();
    cell->AddChild(std::move(label));
    row->AddChild(std::move(cell));
    return raw;
}

static std::unique_ptr<UIElement> MakeSettingsRow()
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-row");
    row->AddClass("inspector-row");
    return row;
}

static void NormalizeRenderPipelineFoldoutContent(Foldout* foldout)
{
    if (!foldout)
        return;
    if (UIElement* content = foldout->GetContentContainer())
    {
        content->Overrides()
            .Set(Style::Gap, StyleLength::Px(0.0f))
            .Set(Style::PaddingTop, StyleLength::Px(0.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(0.0f));
    }
}

static void NormalizeRenderPipelineInspectorRows(UIElement* root)
{
    if (!root)
        return;

    float labelWidthPercent = 37.0f;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = labelWidthPercent;
        if (prefs.TryGetDouble("ui.inspectorLabelWidth", stored))
            labelWidthPercent = static_cast<float>(std::clamp(stored, 20.0, 60.0));
    }

    std::function<void(UIElement*)> visit = [&](UIElement* el)
    {
        if (!el)
            return;

        if (el->HasClass("inspector-row"))
        {
            el->Overrides()
                .Set(Style::FlexWrap, false)
                .Set(Style::AlignItems, AlignItems::Center);

            for (const auto& child : el->GetChildren())
            {
                UIElement* c = child.get();
                if (!c)
                    continue;

                if (c->HasClass("inspector-label-cell") || c->HasClass("inspector-label"))
                {
                    c->Overrides()
                        .Set(Style::FlexGrow, 0.0f)
                        .Set(Style::FlexShrink, 0.0f)
                        .Set(Style::FlexBasis, StyleLength::Percent(labelWidthPercent))
                        .Set(Style::Width, StyleLength::Percent(labelWidthPercent))
                        .Set(Style::MinWidth, StyleLength::Px(0.0f))
                        .Set(Style::MaxWidth, StyleLength::Percent(labelWidthPercent));
                }
                else if (c->HasClass("inspector-field") || c->HasClass("settings-row-field"))
                {
                    c->Overrides()
                        .Set(Style::FlexGrow, 1.0f)
                        .Set(Style::FlexShrink, 1.0f)
                        .Set(Style::FlexBasis, StyleLength::Px(0.0f))
                        .Set(Style::Width, StyleLength::Auto())
                        .Set(Style::MinWidth, StyleLength::Px(0.0f))
                        .Set(Style::MaxWidth, StyleLength::Auto())
                        .Set(Style::MarginRight, StyleLength::Px(0.0f))
                        .Set(Style::AlignSelf, AlignItems::Center);
                }
            }
        }

        for (const auto& child : el->GetChildren())
            visit(child.get());
    };

    visit(root);
}

static std::vector<Dropdown::Option> MakeOptionsFromLabels(const std::vector<std::string>& labels)
{
    std::vector<Dropdown::Option> opts;
    opts.reserve(labels.size());
    for (const auto& s : labels)
        opts.push_back(Dropdown::Option{s, s});
    return opts;
}

static TextField* AddInspectorTextRow(UIElement* parent,
                                      const std::string& labelText,
                                      const std::string& initial,
                                      const std::string& id,
                                      std::function<void(const std::string&)> onChanged)
{
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, labelText);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

    auto field = std::make_unique<TextField>();
    TextField* raw = field.get();
    if (!id.empty())
        raw->SetId(id);
    raw->SetValue(initial);
    raw->SetOnValueChanged(std::move(onChanged));
    fieldContainer->AddChild(std::move(field));
    return raw;
}

static Dropdown* AddInspectorDropdownRow(UIElement* parent,
                                         const std::string& labelText,
                                         const std::vector<Dropdown::Option>& options,
                                         int selectedIndex,
                                         const std::string& id,
                                         std::function<void(const std::string&)> onChanged)
{
    Dropdown* dd = InspectorUI::AddDropdownRow(parent, labelText, options, selectedIndex);
    if (!id.empty())
        dd->SetId(id);
    dd->SetOnValueChanged(std::move(onChanged));
    return dd;
}

static Button* AddInspectorButtonRow(UIElement* parent,
                                     const std::string& labelText,
                                     const std::string& text,
                                     const std::string& id,
                                     UIElement::EventHandler onClick)
{
    UIElement* row = InspectorUI::AddRow(parent);
    row->AddClass("rp-button-row");
    InspectorUI::AddLabel(row, labelText);
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->AddClass("rp-button-field");

    auto button = std::make_unique<Button>();
    Button* raw = button.get();
    raw->AddClass("rp-single-line-button");
    if (!id.empty())
        raw->SetId(id);
    raw->SetText(text);
    raw->RegisterEventHandler(kEventButtonClick, std::move(onClick));
    fieldContainer->AddChild(std::move(button));
    return raw;
}

static Toggle* AddInspectorToggleRow(UIElement* parent,
                                     const std::string& labelText,
                                     bool initial,
                                     const std::string& id,
                                     std::function<void(bool)> onChanged)
{
    Toggle* toggle = InspectorDrag::AddToggleRow(parent, labelText, initial, std::move(onChanged));
    if (!id.empty())
        toggle->SetId(id);
    return toggle;
}

static IntField* AddInspectorIntRow(UIElement* parent,
                                    const std::string& labelText,
                                    int initial,
                                    int resetValue,
                                    const std::string& id,
                                    std::function<void(int)> onChanged)
{
    IntField* field = InspectorDrag::AddIntRowWithDrag(parent, labelText, initial,
        [](int) {},
        std::move(onChanged),
        resetValue);
    if (!id.empty())
        field->SetId(id);
    return field;
}

static FloatField* AddInspectorFloatRow(UIElement* parent,
                                        const std::string& labelText,
                                        float initial,
                                        float resetValue,
                                        float minValue,
                                        float maxValue,
                                        const std::string& id,
                                        std::function<void(float)> onChanged)
{
    FloatField* field = InspectorDrag::AddFloatRowWithDrag(parent, labelText, initial,
        [](float) {},
        std::move(onChanged),
        resetValue,
        nullptr,
        minValue,
        maxValue);
    if (!id.empty())
        field->SetId(id);
    return field;
}

static std::string MakeUniqueKeyForObject(const nlohmann::json& obj, const std::string& base)
{
    if (!obj.is_object())
        return base.empty() ? std::string("New") : base;
    std::string k = base.empty() ? std::string("New") : base;
    if (!obj.contains(k))
        return k;
    for (int i = 2; i < 10000; ++i)
    {
        std::string cand = k + std::to_string(i);
        if (!obj.contains(cand))
            return cand;
    }
    return k + "_X";
}

static void EnsureObjectPath(nlohmann::json& doc, size_t passIndex, const char* field)
{
    if (!doc.contains("passes") || !doc["passes"].is_array())
        doc["passes"] = nlohmann::json::array();
    if (passIndex >= doc["passes"].size() || !doc["passes"][passIndex].is_object())
        return;
    auto& p = doc["passes"][passIndex];
    if (!p.contains(field) || !p[field].is_object())
        p[field] = nlohmann::json::object();
}

static void BuildStringMapEditor(UIElement* uiRoot,
                                 UIElement* rebuildRoot,
                                 const std::string& title,
                                 RenderPipelineAsset* pipe,
                                 nlohmann::json doc,
                                 size_t passIndex,
                                 const char* field,
                                 const std::string& idPrefix,
                                 bool allowDropRefs)
{
    if (!uiRoot || !rebuildRoot || !pipe)
        return;
    AddTextBlock(uiRoot, title);

    EnsureObjectPath(doc, passIndex, field);

    const auto& obj = (doc.contains("passes") && doc["passes"].is_array() && passIndex < doc["passes"].size() && doc["passes"][passIndex].is_object())
                          ? doc["passes"][passIndex][field]
                          : nlohmann::json::object();

    size_t rowIdx = 0;
    if (obj.is_object())
    {
        for (auto it = obj.begin(); it != obj.end(); ++it, ++rowIdx)
        {
            const std::string key = it.key();
            const std::string value = it.value().is_string() ? it.value().get<std::string>() : std::string();

            AddInspectorTextRow(uiRoot, "Key", key, idPrefix + "-key-" + std::to_string(rowIdx),
                [rebuildRoot, pipe, doc, passIndex, field, oldKey = key](const std::string& newKey) mutable
                {
                    if (newKey.empty() || newKey == oldKey)
                        return;
                    EnsureObjectPath(doc, passIndex, field);
                    auto& o = doc["passes"][passIndex][field];
                    if (!o.is_object() || !o.contains(oldKey))
                        return;
                    const nlohmann::json v = o[oldKey];
                    o.erase(oldKey);
                    o[newKey] = v;
                    SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string(field) + " Key");
                });

            // Value row
            {
                UIElement* row = InspectorUI::AddRow(uiRoot);
                InspectorUI::AddLabel(row, "Value");
                UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
                std::unique_ptr<TextField> fieldValue;
                if (allowDropRefs)
                    fieldValue = std::make_unique<DropTextField>();
                else
                    fieldValue = std::make_unique<TextField>();
                fieldValue->SetId(idPrefix + "-value-" + std::to_string(rowIdx));
                fieldValue->SetValue(value);
                fieldValue->SetOnValueChanged([rebuildRoot, pipe, doc, passIndex, field, key](const std::string& v) mutable
                                              {
                                                  EnsureObjectPath(doc, passIndex, field);
                                                  auto& o = doc["passes"][passIndex][field];
                                                  if (!o.is_object())
                                                      return;
                                                  o[key] = v;
                                                  SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string(field) + " Value");
                                              });
                fieldContainer->AddChild(std::move(fieldValue));

                auto xBtn = std::make_unique<Button>();
                Button* xBtnRaw = xBtn.get();
                xBtnRaw->AddClass("small");
                xBtnRaw->AddClass("secondary");
                xBtnRaw->AddClass("icon-button");
                xBtnRaw->AddClass("xclose-icon");
                xBtnRaw->AddClass("rp-entry-remove");
                xBtnRaw->SetTooltip("Remove Entry");
                xBtnRaw->SetId(idPrefix + "-remove-" + std::to_string(rowIdx));
                xBtnRaw->RegisterEventHandler(kEventButtonClick, [rebuildRoot, pipe, doc, passIndex, field, key](UIEvent&) mutable
                                    {
                                        EnsureObjectPath(doc, passIndex, field);
                                        auto& o = doc["passes"][passIndex][field];
                                        if (o.is_object())
                                            o.erase(key);
                                        SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string("Remove ") + field);
                                    });
                fieldContainer->AddChild(std::move(xBtn));
            }
        }
    }

    // Add entry button with + icon
    {
        AddInspectorButtonRow(uiRoot, "", "Add Entry", idPrefix + "-add",
            [rebuildRoot, pipe, doc, passIndex, field](UIEvent&) mutable
            {
                EnsureObjectPath(doc, passIndex, field);
                auto& o = doc["passes"][passIndex][field];
                const std::string k = MakeUniqueKeyForObject(o, "New");
                o[k] = "";
                SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string("Add ") + field);
            });
    }
}

static void BuildUsageMapEditor(UIElement* uiRoot,
                                UIElement* rebuildRoot,
                                const std::string& title,
                                RenderPipelineAsset* pipe,
                                nlohmann::json doc,
                                size_t passIndex,
                                const char* field,
                                const std::string& idPrefix,
                                const std::vector<std::string>& usageOptions,
                                const std::string& defaultUsage)
{
    if (!uiRoot || !rebuildRoot || !pipe)
        return;
    AddTextBlock(uiRoot, title);

    EnsureObjectPath(doc, passIndex, field);
    auto opts = MakeOptionsFromLabels(usageOptions);

    const auto& obj = (doc.contains("passes") && doc["passes"].is_array() && passIndex < doc["passes"].size() && doc["passes"][passIndex].is_object())
                          ? doc["passes"][passIndex][field]
                          : nlohmann::json::object();

    size_t rowIdx = 0;
    if (obj.is_object())
    {
        for (auto it = obj.begin(); it != obj.end(); ++it, ++rowIdx)
        {
            const std::string key = it.key();
            const std::string value = it.value().is_string() ? it.value().get<std::string>() : defaultUsage;

            AddInspectorTextRow(uiRoot, "Key", key, idPrefix + "-key-" + std::to_string(rowIdx),
                [rebuildRoot, pipe, doc, passIndex, field, oldKey = key](const std::string& newKey) mutable
                {
                    if (newKey.empty() || newKey == oldKey)
                        return;
                    EnsureObjectPath(doc, passIndex, field);
                    auto& o = doc["passes"][passIndex][field];
                    if (!o.is_object() || !o.contains(oldKey))
                        return;
                    const nlohmann::json v = o[oldKey];
                    o.erase(oldKey);
                    o[newKey] = v;
                    SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string(field) + " Key");
                });

            // Value row (dropdown)
            {
                UIElement* row = InspectorUI::AddRow(uiRoot);
                InspectorUI::AddLabel(row, "Usage");
                UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

                auto dd = std::make_unique<Dropdown>();
                auto* ddRaw = dd.get();
                ddRaw->AddClass("inspector-dropdown");
                ddRaw->SetId(idPrefix + "-usage-" + std::to_string(rowIdx));
                ddRaw->SetOptions(opts, 0);
                ddRaw->SetSelectedValue(value);
                ddRaw->SetOnValueChanged([rebuildRoot, pipe, doc, passIndex, field, key](const std::string& v) mutable
                                         {
                                             EnsureObjectPath(doc, passIndex, field);
                                             auto& o = doc["passes"][passIndex][field];
                                             if (!o.is_object())
                                                 return;
                                             o[key] = v;
                                             SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string(field) + " Value");
                                         });
                fieldContainer->AddChild(std::move(dd));

                auto xBtn = std::make_unique<Button>();
                Button* xBtnRaw = xBtn.get();
                xBtnRaw->AddClass("small");
                xBtnRaw->AddClass("secondary");
                xBtnRaw->AddClass("icon-button");
                xBtnRaw->AddClass("xclose-icon");
                xBtnRaw->AddClass("rp-entry-remove");
                xBtnRaw->SetTooltip("Remove Entry");
                xBtnRaw->SetId(idPrefix + "-remove-" + std::to_string(rowIdx));
                xBtnRaw->RegisterEventHandler(kEventButtonClick, [rebuildRoot, pipe, doc, passIndex, field, key](UIEvent&) mutable
                                    {
                                        EnsureObjectPath(doc, passIndex, field);
                                        auto& o = doc["passes"][passIndex][field];
                                        if (o.is_object())
                                            o.erase(key);
                                        SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string("Remove ") + field);
                                    });
                fieldContainer->AddChild(std::move(xBtn));
            }
        }
    }

    // + button to add entry
    {
        AddInspectorButtonRow(uiRoot, "", "Add Entry", idPrefix + "-add",
            [rebuildRoot, pipe, doc, passIndex, field, defaultUsage](UIEvent&) mutable
            {
                EnsureObjectPath(doc, passIndex, field);
                auto& o = doc["passes"][passIndex][field];
                const std::string k = MakeUniqueKeyForObject(o, "New");
                o[k] = defaultUsage;
                SaveReloadRebuildUndoable(rebuildRoot, pipe, doc, s_UndoService, std::string("Add ") + field);
            });
    }
}

static std::string MakeIdSafe(std::string s)
{
    for (char& c : s)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-' || c == '.'))
            c = '_';
    }
    return s;
}

static void EnsureArrayPath(nlohmann::json& doc, const std::vector<std::string>& path)
{
    if (path.empty())
        return;
    nlohmann::json* cur = &doc;
    for (size_t i = 0; i < path.size(); ++i)
    {
        const std::string& key = path[i];
        if (!cur->is_object())
            *cur = nlohmann::json::object();
        if (!cur->contains(key))
            (*cur)[key] = (i + 1 == path.size()) ? nlohmann::json::array() : nlohmann::json::object();
        cur = &(*cur)[key];
    }
    if (!cur->is_array())
        *cur = nlohmann::json::array();
}

static void BuildStringListEditor(UIElement* root,
                                  const std::string& title,
                                  RenderPipelineAsset* pipe,
                                  nlohmann::json doc,
                                  const std::vector<std::string>& path,
                                  const std::string& idPrefix,
                                  const std::vector<std::string>& suggestions = {})
{
    if (!root || !pipe)
        return;
    AddTextBlock(root, title);

    EnsureArrayPath(doc, path);

    // Navigate to array (safe, since EnsureArrayPath created it).
    const nlohmann::json* cur = &doc;
    for (const auto& k : path)
        cur = &(*cur)[k];
    const auto& arr = *cur;

    // Existing entries
    for (size_t i = 0; i < arr.size(); ++i)
    {
        std::string v = arr[i].is_string() ? arr[i].get<std::string>() : std::string();

        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "Value");

        if (!suggestions.empty())
        {
            auto dd = std::make_unique<Dropdown>();
            auto* ddRaw = dd.get();
            ddRaw->AddClass("settings-row-field");
            ddRaw->SetId(idPrefix + "-value-" + std::to_string(i));
            ddRaw->SetOptions(MakeOptionsFromLabels(suggestions), 0);
            ddRaw->SetSelectedValue(v);
            ddRaw->SetOnValueChanged([root, pipe, doc, path, i](const std::string& newV) mutable
                                     {
                                         EnsureArrayPath(doc, path);
                                         nlohmann::json* c = &doc;
                                         for (const auto& k : path)
                                             c = &(*c)[k];
                                         if (!c->is_array() || i >= c->size())
                                             return;
                                         (*c)[i] = newV;
                                         SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                     });
            row->AddChild(std::move(dd));
        }
        else
        {
            auto tf = std::make_unique<TextField>();
            tf->AddClass("settings-row-field");
            tf->SetId(idPrefix + "-value-" + std::to_string(i));
            tf->SetValue(v);
            tf->SetOnValueChanged([root, pipe, doc, path, i](const std::string& newV) mutable
                                  {
                                      EnsureArrayPath(doc, path);
                                      nlohmann::json* c = &doc;
                                      for (const auto& k : path)
                                          c = &(*c)[k];
                                      if (!c->is_array() || i >= c->size())
                                          return;
                                      (*c)[i] = newV;
                                      SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                  });
            row->AddChild(std::move(tf));
        }

        // X button to remove this entry
        auto rm = std::make_unique<Button>();
        rm->AddClass("small");
        rm->AddClass("secondary");
        rm->AddClass("icon-button");
        rm->AddClass("xclose-icon");
        rm->SetTooltip("Remove Entry");
        rm->SetId(idPrefix + "-remove-" + std::to_string(i));
        rm->RegisterEventHandler(kEventButtonClick, [root, pipe, doc, path, i](UIEvent&) mutable
                       {
                           EnsureArrayPath(doc, path);
                           nlohmann::json* c = &doc;
                           for (const auto& k : path)
                               c = &(*c)[k];
                           if (!c->is_array() || i >= c->size())
                               return;
                           c->erase(c->begin() + (nlohmann::json::difference_type)i);
                           SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                       });
        row->AddChild(std::move(rm));
        root->AddChild(std::move(row));
    }

    // Add entry button with + icon
    {
        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "");
        auto btn = std::make_unique<Button>();
        btn->AddClass("settings-row-field");
        btn->SetId(idPrefix + "-add");
        btn->SetText("Add Entry");
        btn->RegisterEventHandler(kEventButtonClick, [root, pipe, doc, path, suggestions](UIEvent&) mutable
                        {
                            EnsureArrayPath(doc, path);
                            nlohmann::json* c = &doc;
                            for (const auto& k : path)
                                c = &(*c)[k];
                            if (!c->is_array())
                                return;
                            if (!suggestions.empty())
                                c->push_back(suggestions.front());
                            else
                                c->push_back("");
                            SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                        });
        row->AddChild(std::move(btn));
        root->AddChild(std::move(row));
    }
}

static void BuildRenderPipelineEditorUI(UIElement* root,
                                        RenderPipelineAsset* pipe,
                                        Editor::UndoRedoService* undo)
{
    if (!root || !pipe)
        return;

    // Persist foldout expansion state across inspector rebuilds (e.g. pass toggles).
    struct FoldoutStateCache
    {
        static std::string Key(const RenderPipelineAsset* p, const std::string& foldoutId)
        {
            const std::string path = p ? p->GetPath().generic_string() : std::string{};
            return path + "|" + foldoutId;
        }

        static bool Get(const RenderPipelineAsset* p, const std::string& foldoutId, bool defaultExpanded)
        {
            const std::string k = Key(p, foldoutId);
            auto& state = GetFoldoutStateMap();
            auto it = state.find(k);
            if (it == state.end())
                return defaultExpanded;
            return it->second;
        }

        static void Set(const RenderPipelineAsset* p, const std::string& foldoutId, bool expanded)
        {
            GetFoldoutStateMap()[Key(p, foldoutId)] = expanded;
        }
    };

    auto header = std::make_unique<Label>();
    header->AddClass("inspector-header");
    header->AddClass("rp-inspector-header");
    header->SetText("Render Pipeline: " + pipe->GetName());
    root->AddChild(std::move(header));

    // Use foldouts to keep large pipelines navigable.
    auto settingsFoldout = std::make_unique<Foldout>();
    settingsFoldout->SetTitle("Settings");
    settingsFoldout->SetId("rp-foldout-settings");
    settingsFoldout->AddClass("rp-foldout");
    settingsFoldout->AddClass("rp-foldout-top");
    settingsFoldout->SetExpanded(FoldoutStateCache::Get(pipe, "rp-foldout-settings", true));
    NormalizeRenderPipelineFoldoutContent(settingsFoldout.get());
    settingsFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                          { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
    Foldout* settingsFoldoutPtr = settingsFoldout.get();
    root->AddChild(std::move(settingsFoldout));

    auto resourcesFoldout = std::make_unique<Foldout>();
    resourcesFoldout->SetTitle("Resources");
    resourcesFoldout->SetId("rp-foldout-resources");
    resourcesFoldout->AddClass("rp-foldout");
    resourcesFoldout->AddClass("rp-foldout-top");
    resourcesFoldout->SetExpanded(FoldoutStateCache::Get(pipe, "rp-foldout-resources", false));
    NormalizeRenderPipelineFoldoutContent(resourcesFoldout.get());
    resourcesFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                           { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
    Foldout* resourcesFoldoutPtr = resourcesFoldout.get();
    root->AddChild(std::move(resourcesFoldout));

    auto passesFoldout = std::make_unique<Foldout>();
    passesFoldout->SetTitle("Passes");
    passesFoldout->SetId("rp-foldout-passes");
    passesFoldout->AddClass("rp-foldout");
    passesFoldout->AddClass("rp-foldout-top");
    passesFoldout->SetExpanded(FoldoutStateCache::Get(pipe, "rp-foldout-passes", true));
    NormalizeRenderPipelineFoldoutContent(passesFoldout.get());
    passesFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                        { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
    Foldout* passesFoldoutPtr = passesFoldout.get();
    root->AddChild(std::move(passesFoldout));

    auto debugFoldout = std::make_unique<Foldout>();
    debugFoldout->SetTitle("Validation / Debug");
    debugFoldout->SetId("rp-foldout-debug");
    debugFoldout->AddClass("rp-foldout");
    debugFoldout->AddClass("rp-foldout-top");
    debugFoldout->SetExpanded(FoldoutStateCache::Get(pipe, "rp-foldout-debug", false));
    NormalizeRenderPipelineFoldoutContent(debugFoldout.get());
    debugFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                       { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
    Foldout* debugFoldoutPtr = debugFoldout.get();
    root->AddChild(std::move(debugFoldout));

    // Active pipeline control (Editor integration)
    {
        std::string activePath;
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            activePath = rs->Spine().GetActiveRenderPipelinePath().generic_string();
        }

        // Compare using resolved absolute paths so both relative ("RenderPipelines/..")
        // and absolute paths behave consistently across build configs.
        auto& am = EngineCore::GetInstance().GetAssetManager();
        const std::string activeAbs = am.ResolveAssetPath(std::filesystem::path(activePath)).generic_string();
        const std::string thisAbs = pipe->GetPath().generic_string();
        const bool isActive = (!activeAbs.empty() && activeAbs == thisAbs);

        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "Active");

        auto btn = std::make_unique<Button>();
        btn->AddClass("settings-row-field");
        btn->SetText(isActive ? "Active Pipeline" : "Set Active Pipeline");
        btn->SetEnabled(!isActive);

        const std::filesystem::path pipelinePath = pipe->GetPath();
        btn->RegisterEventHandler(kEventButtonClick, [pipelinePath](UIEvent&)
                        { SetActiveRenderPipelineInProjectSettings(pipelinePath); });

        row->AddChild(std::move(btn));
        settingsFoldoutPtr->AddChild(std::move(row));
    }

    // Path + basic info — each item on its own label so Yoga measures each line height separately.
    AddTextBlock(settingsFoldoutPtr, pipe->GetPath().string());
    AddTextBlock(settingsFoldoutPtr, "schemaVersion: " + std::to_string(pipe->GetDocument().schemaVersion));

    if (!pipe->GetErrors().empty())
    {
        std::ostringstream e;
        e << "Errors:\n";
        for (const auto& err : pipe->GetErrors())
            e << " - " << err << "\n";
        AddTextBlock(debugFoldoutPtr, e.str());
    }

    // Parse JSON so we can offer a basic form editor (no manual JSON editing required).
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(pipe->GetDocument().jsonText);
    }
    catch (const std::exception& e)
    {
        AddTextBlock(root, std::string("JSON parse failed: ") + e.what());
        return;
    }
    if (!doc.is_object())
    {
        AddTextBlock(root, "Invalid document: root must be a JSON object.");
        return;
    }

    // ---- Editor: pipelineName ----
    {
        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "Pipeline Name");

        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        field->SetValue(doc.value("pipelineName", pipe->GetName()));

        field->SetOnValueChanged([root, pipe, doc](const std::string& v) mutable
                                 {
                                     doc["pipelineName"] = v;
                                     SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                 });

        row->AddChild(std::move(field));
        settingsFoldoutPtr->AddChild(std::move(row));
    }

    // ---- Editor: outputs.FinalColor ----
    {
        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "Output: FinalColor");

        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        constexpr const char* kFinalColor = Engine::Renderer::Pipeline::Names::Output::FinalColor;
        std::string out = Engine::Renderer::Pipeline::Names::View::Resolve;
        if (doc.contains("outputs") && doc["outputs"].is_object() && doc["outputs"].contains(kFinalColor) && doc["outputs"][kFinalColor].is_string())
            out = doc["outputs"][kFinalColor].get<std::string>();
        field->SetValue(out);

        field->SetOnValueChanged([root, pipe, doc](const std::string& v) mutable
                                 {
                                     if (!doc.contains("outputs") || !doc["outputs"].is_object())
                                         doc["outputs"] = nlohmann::json::object();
                                     doc["outputs"][Engine::Renderer::Pipeline::Names::Output::FinalColor] = v;
                                     SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                 });

        row->AddChild(std::move(field));
        settingsFoldoutPtr->AddChild(std::move(row));
    }

    // ---- Editor: resources ----
    if (!doc.contains("resources") || !doc["resources"].is_object())
    {
        doc["resources"] = nlohmann::json::object();
    }

    const std::vector<std::string> resourceKinds = {"Buffer", "Texture"};
    const std::vector<std::string> resourceScopes = {"Frame", "PerView"};
    const std::vector<std::string> resourceUsages = {
        "ConstantBuffer",
        "Storage",
        "ShaderResource",
        "CopySource",
        "CopyDestination",
        "RenderTarget",
        "DepthStencil",
    };
    const std::vector<std::string> memoryUsages = {"DeviceLocal", "Upload", "Readback"};
    const std::vector<std::string> commonFlags = {"PersistentlyMapped"};

    // Add Resource button
    {
        auto row = MakeSettingsRow();
        AddRowLabel(row.get(), "");

        auto btn = std::make_unique<Button>();
        btn->AddClass("settings-row-field");
        btn->SetId("rp-res-add");
        btn->SetText("Add Resource");
        btn->RegisterEventHandler(kEventButtonClick, [root, pipe, doc](UIEvent&) mutable
                        {
                            if (!doc.contains("resources") || !doc["resources"].is_object())
                                doc["resources"] = nlohmann::json::object();
                            auto& r = doc["resources"];
                            const std::string name = MakeUniqueKeyForObject(r, "NewResource");
                            r[name] = nlohmann::json::object({{"kind", "Buffer"}, {"scope", "PerView"}, {"size", nlohmann::json::object({{"bytes", 16}})}});
                            SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                        });
        row->AddChild(std::move(btn));
        resourcesFoldoutPtr->AddChild(std::move(row));
    }

    // Resource entries
    if (doc["resources"].is_object())
    {
        size_t idx = 0;
        // Iterate over a stable snapshot of keys so edits don't invalidate iterators.
        std::vector<std::string> keys;
        keys.reserve(doc["resources"].size());
        for (auto it = doc["resources"].begin(); it != doc["resources"].end(); ++it)
            keys.push_back(it.key());

        for (const auto& resName : keys)
        {
            if (!doc["resources"].contains(resName) || !doc["resources"][resName].is_object())
                continue;
            auto& res = doc["resources"][resName];
            const std::string idRes = MakeIdSafe(resName);

            auto resFoldout = std::make_unique<Foldout>();
            resFoldout->SetId("rp-res-" + idRes + "-foldout");
            resFoldout->SetTitle(resName + " (Resource)");
            resFoldout->AddClass("rp-foldout");
            resFoldout->SetExpanded(FoldoutStateCache::Get(pipe, "rp-res-" + idRes + "-foldout", false));
            NormalizeRenderPipelineFoldoutContent(resFoldout.get());
            resFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                             { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
            Foldout* resFoldoutPtr = resFoldout.get();
            resourcesFoldoutPtr->AddChild(std::move(resFoldout));

            // Drag helper: allow dragging resource refs into other fields.
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "Drag");
                auto lbl = MakeDraggableRefLabel(resName, "rp-res-" + idRes + "-drag");
                lbl->AddClass("settings-row-field");
                row->AddChild(std::move(lbl));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // Rename resource key
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "Name");
                auto tf = std::make_unique<TextField>();
                tf->AddClass("settings-row-field");
                tf->SetId("rp-res-" + idRes + "-name");
                tf->SetValue(resName);
                tf->SetOnValueChanged([root, pipe, doc, oldKey = resName](const std::string& newKey) mutable
                                      {
                                          if (newKey.empty() || newKey == oldKey)
                                              return;
                                          if (!doc.contains("resources") || !doc["resources"].is_object())
                                              return;
                                          auto& r = doc["resources"];
                                          if (!r.contains(oldKey))
                                              return;
                                          const nlohmann::json v = r[oldKey];
                                          r.erase(oldKey);
                                          r[newKey] = v;
                                          SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                      });
                row->AddChild(std::move(tf));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // kind dropdown
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "kind");
                auto dd = std::make_unique<Dropdown>();
                auto* ddRaw = dd.get();
                ddRaw->AddClass("settings-row-field");
                ddRaw->SetId("rp-res-" + idRes + "-kind");
                ddRaw->SetOptions(MakeOptionsFromLabels(resourceKinds), 0);
                ddRaw->SetSelectedValue(res.value("kind", "Buffer"));
                ddRaw->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                         {
                                             if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                 return;
                                             doc["resources"][resName]["kind"] = v;
                                             SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                         });
                row->AddChild(std::move(dd));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // scope dropdown
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "scope");
                auto dd = std::make_unique<Dropdown>();
                auto* ddRaw = dd.get();
                ddRaw->AddClass("settings-row-field");
                ddRaw->SetId("rp-res-" + idRes + "-scope");
                ddRaw->SetOptions(MakeOptionsFromLabels(resourceScopes), 0);
                ddRaw->SetSelectedValue(res.value("scope", "PerView"));
                ddRaw->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                         {
                                             if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                 return;
                                             doc["resources"][resName]["scope"] = v;
                                             SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                         });
                row->AddChild(std::move(dd));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // size.bytes
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "size.bytes");

                int bytes = 0;
                if (res.contains("size") && res["size"].is_object())
                {
                    const auto& s = res["size"];
                    if (s.contains("bytes") && s["bytes"].is_number_integer())
                        bytes = (int)s["bytes"].get<int64_t>();
                }

                auto f = std::make_unique<IntField>();
                f->AddClass("settings-row-field");
                f->SetId("rp-res-" + idRes + "-size-bytes");
                f->SetValue(bytes);
                f->SetOnValueChanged([root, pipe, doc, resName](const int& v) mutable
                                     {
                                         if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                             return;
                                         auto& r = doc["resources"][resName];
                                         if (!r.contains("size") || !r["size"].is_object())
                                             r["size"] = nlohmann::json::object();
                                         r["size"]["bytes"] = std::max(0, v);
                                         SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                     });
                row->AddChild(std::move(f));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // size.expression
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "size.expression");
                std::string expr;
                if (res.contains("size") && res["size"].is_object())
                {
                    const auto& s = res["size"];
                    if (s.contains("expression") && s["expression"].is_string())
                        expr = s["expression"].get<std::string>();
                }
                auto tf = std::make_unique<TextField>();
                tf->AddClass("settings-row-field");
                tf->SetId("rp-res-" + idRes + "-size-expr");
                tf->SetValue(expr);
                tf->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                      {
                                          if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                              return;
                                          auto& r = doc["resources"][resName];
                                          if (!r.contains("size") || !r["size"].is_object())
                                              r["size"] = nlohmann::json::object();
                                          r["size"]["expression"] = v;
                                          SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                      });
                row->AddChild(std::move(tf));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // size.variables (object map: key -> number|string)
            {
                AddTextBlock(resFoldoutPtr, "size.variables:");
                if (!res.contains("size") || !res["size"].is_object())
                    res["size"] = nlohmann::json::object();
                if (!res["size"].contains("variables") || !res["size"]["variables"].is_object())
                    res["size"]["variables"] = nlohmann::json::object();

                const auto& vars = res["size"]["variables"];
                std::vector<std::string> varKeys;
                if (vars.is_object())
                {
                    varKeys.reserve(vars.size());
                    for (auto it = vars.begin(); it != vars.end(); ++it)
                        varKeys.push_back(it.key());
                }

                const std::vector<std::string> varTypes = {"number", "string"};

                for (size_t vi = 0; vi < varKeys.size(); ++vi)
                {
                    const std::string varKey = varKeys[vi];
                    const std::string idVar = MakeIdSafe(varKey);
                    const auto& varVal = res["size"]["variables"][varKey];
                    const bool isNumber = varVal.is_number();
                    const std::string type = isNumber ? "number" : "string";

                    // Key
                    {
                        auto row = MakeSettingsRow();
                        AddRowLabel(row.get(), "Var Key");
                        auto tf = std::make_unique<TextField>();
                        tf->AddClass("settings-row-field");
                        tf->SetId("rp-res-" + idRes + "-size-var-" + idVar + "-key");
                        tf->SetValue(varKey);
                        tf->SetOnValueChanged([root, pipe, doc, resName, oldKey = varKey](const std::string& newKey) mutable
                                              {
                                                  if (newKey.empty() || newKey == oldKey)
                                                      return;
                                                  if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                      return;
                                                  auto& r = doc["resources"][resName];
                                                  if (!r.contains("size") || !r["size"].is_object())
                                                      r["size"] = nlohmann::json::object();
                                                  if (!r["size"].contains("variables") || !r["size"]["variables"].is_object())
                                                      r["size"]["variables"] = nlohmann::json::object();
                                                  auto& v = r["size"]["variables"];
                                                  if (!v.contains(oldKey))
                                                      return;
                                                  const nlohmann::json oldVal = v[oldKey];
                                                  v.erase(oldKey);
                                                  v[newKey] = oldVal;
                                                  SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                              });
                        row->AddChild(std::move(tf));
                        resFoldoutPtr->AddChild(std::move(row));
                    }

                    // Type
                    {
                        auto row = MakeSettingsRow();
                        AddRowLabel(row.get(), "Var Type");
                        auto dd = std::make_unique<Dropdown>();
                        auto* ddRaw = dd.get();
                        ddRaw->AddClass("settings-row-field");
                        ddRaw->SetId("rp-res-" + idRes + "-size-var-" + idVar + "-type");
                        ddRaw->SetOptions(MakeOptionsFromLabels(varTypes), 0);
                        ddRaw->SetSelectedValue(type);
                        ddRaw->SetOnValueChanged([root, pipe, doc, resName, varKey](const std::string& newType) mutable
                                                 {
                                                     if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                         return;
                                                     auto& r = doc["resources"][resName];
                                                     if (!r.contains("size") || !r["size"].is_object())
                                                         r["size"] = nlohmann::json::object();
                                                     if (!r["size"].contains("variables") || !r["size"]["variables"].is_object())
                                                         r["size"]["variables"] = nlohmann::json::object();
                                                     auto& v = r["size"]["variables"];
                                                     if (ToLowerAscii(newType) == "number")
                                                         v[varKey] = 0.0;
                                                     else
                                                         v[varKey] = "";
                                                     SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                 });
                        row->AddChild(std::move(dd));
                        resFoldoutPtr->AddChild(std::move(row));
                    }

                    // Value (number or string)
                    if (isNumber)
                    {
                        auto row = MakeSettingsRow();
                        AddRowLabel(row.get(), "Var Value");
                        auto ff = std::make_unique<FloatField>();
                        ff->AddClass("settings-row-field");
                        ff->SetId("rp-res-" + idRes + "-size-var-" + idVar + "-number");
                        ff->SetValue(varVal.get<float>());
                        ff->SetOnValueChanged([root, pipe, doc, resName, varKey](const float& v) mutable
                                              {
                                                  if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                      return;
                                                  auto& r = doc["resources"][resName];
                                                  if (!r.contains("size") || !r["size"].is_object())
                                                      r["size"] = nlohmann::json::object();
                                                  if (!r["size"].contains("variables") || !r["size"]["variables"].is_object())
                                                      r["size"]["variables"] = nlohmann::json::object();
                                                  r["size"]["variables"][varKey] = (double)v;
                                                  SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                              });
                        row->AddChild(std::move(ff));
                        resFoldoutPtr->AddChild(std::move(row));
                    }
                    else
                    {
                        auto row = MakeSettingsRow();
                        AddRowLabel(row.get(), "Var Value");
                        auto tf = std::make_unique<TextField>();
                        tf->AddClass("settings-row-field");
                        tf->SetId("rp-res-" + idRes + "-size-var-" + idVar + "-string");
                        tf->SetValue(varVal.is_string() ? varVal.get<std::string>() : std::string());
                        tf->SetOnValueChanged([root, pipe, doc, resName, varKey](const std::string& v) mutable
                                              {
                                                  if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                      return;
                                                  auto& r = doc["resources"][resName];
                                                  if (!r.contains("size") || !r["size"].is_object())
                                                      r["size"] = nlohmann::json::object();
                                                  if (!r["size"].contains("variables") || !r["size"]["variables"].is_object())
                                                      r["size"]["variables"] = nlohmann::json::object();
                                                  r["size"]["variables"][varKey] = v;
                                                  SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                              });
                        row->AddChild(std::move(tf));
                        resFoldoutPtr->AddChild(std::move(row));
                    }

                    // Remove variable
                    {
                        auto row = MakeSettingsRow();
                        AddRowLabel(row.get(), "");
                        auto btn = std::make_unique<Button>();
                        btn->AddClass("settings-row-field");
                        btn->SetId("rp-res-" + idRes + "-size-var-" + idVar + "-remove");
                        btn->SetText("Remove Var");
                        btn->RegisterEventHandler(kEventButtonClick, [root, pipe, doc, resName, varKey](UIEvent&) mutable
                                        {
                                            if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                return;
                                            auto& r = doc["resources"][resName];
                                            if (!r.contains("size") || !r["size"].is_object())
                                                return;
                                            if (r["size"].contains("variables") && r["size"]["variables"].is_object())
                                            {
                                                r["size"]["variables"].erase(varKey);
                                                SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                            }
                                        });
                        row->AddChild(std::move(btn));
                        resFoldoutPtr->AddChild(std::move(row));
                    }
                }

                // Add variable
                {
                    auto row = MakeSettingsRow();
                    AddRowLabel(row.get(), "");
                    auto btn = std::make_unique<Button>();
                    btn->AddClass("settings-row-field");
                    btn->SetId("rp-res-" + idRes + "-size-vars-add");
                    btn->SetText("Add Var");
                    btn->RegisterEventHandler(kEventButtonClick, [root, pipe, doc, resName](UIEvent&) mutable
                                    {
                                        if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                            return;
                                        auto& r = doc["resources"][resName];
                                        if (!r.contains("size") || !r["size"].is_object())
                                            r["size"] = nlohmann::json::object();
                                        if (!r["size"].contains("variables") || !r["size"]["variables"].is_object())
                                            r["size"]["variables"] = nlohmann::json::object();
                                        auto& v = r["size"]["variables"];
                                        const std::string k = MakeUniqueKeyForObject(v, "var");
                                        v[k] = 0.0;
                                        SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                    });
                    row->AddChild(std::move(btn));
                    resFoldoutPtr->AddChild(std::move(row));
                }
            }

            // usage[] list (string list)
            {
                BuildStringListEditor(
                    resFoldoutPtr,
                    "usage[]:",
                    pipe,
                    doc,
                    {"resources", resName, "usage"},
                    "rp-res-" + idRes + "-usage",
                    resourceUsages);
            }

            // memoryUsage (string)
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "memoryUsage");
                auto dd = std::make_unique<Dropdown>();
                auto* ddRaw = dd.get();
                ddRaw->AddClass("settings-row-field");
                ddRaw->SetId("rp-res-" + idRes + "-memoryUsage");
                ddRaw->SetOptions(MakeOptionsFromLabels(memoryUsages), 0);
                ddRaw->SetSelectedValue(res.value("memoryUsage", "DeviceLocal"));
                ddRaw->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                         {
                                             if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                 return;
                                             doc["resources"][resName]["memoryUsage"] = v;
                                             SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                         });
                row->AddChild(std::move(dd));
                resFoldoutPtr->AddChild(std::move(row));
            }

            // flags[] list
            {
                BuildStringListEditor(
                    resFoldoutPtr,
                    "flags[]:",
                    pipe,
                    doc,
                    {"resources", resName, "flags"},
                    "rp-res-" + idRes + "-flags",
                    commonFlags);
            }

            // Texture-specific fields (minimal; still general)
            {
                const std::string kindLower = ToLowerAscii(res.value("kind", "Buffer"));
                if (kindLower == "texture")
                {
                    auto row = MakeSettingsRow();
                    AddRowLabel(row.get(), "format");
                    auto tf = std::make_unique<TextField>();
                    tf->AddClass("settings-row-field");
                    tf->SetId("rp-res-" + idRes + "-format");
                    tf->SetValue(res.value("format", ""));
                    tf->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                          {
                                              if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                  return;
                                              doc["resources"][resName]["format"] = v;
                                              SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                          });
                    row->AddChild(std::move(tf));
                    resFoldoutPtr->AddChild(std::move(row));

                    // extent editor: either {extent:{width,height}} or {extent:{scale:[sx,sy]}}
                    {
                        bool useAbs = false;
                        int w = 1, h = 1;
                        float sx = 1.0f, sy = 1.0f;
                        if (res.contains("extent") && res["extent"].is_object())
                        {
                            const auto& ex = res["extent"];
                            if (ex.contains("width") && ex.contains("height") && ex["width"].is_number_integer() && ex["height"].is_number_integer())
                            {
                                useAbs = true;
                                w = std::max(1, (int)ex["width"].get<int64_t>());
                                h = std::max(1, (int)ex["height"].get<int64_t>());
                            }
                            if (!useAbs && ex.contains("scale") && ex["scale"].is_array() && ex["scale"].size() >= 2)
                            {
                                if (ex["scale"][0].is_number())
                                    sx = ex["scale"][0].get<float>();
                                if (ex["scale"][1].is_number())
                                    sy = ex["scale"][1].get<float>();
                            }
                        }

                        // mode dropdown (UI-only)
                        {
                            auto rowMode = MakeSettingsRow();
                            AddRowLabel(rowMode.get(), "extent.mode");
                            auto dd = std::make_unique<Dropdown>();
                            auto* ddRaw = dd.get();
                            ddRaw->AddClass("settings-row-field");
                            ddRaw->SetId("rp-res-" + idRes + "-extent-mode");
                            ddRaw->SetOptions(MakeOptionsFromLabels({"Scale", "Absolute"}), 0);
                            ddRaw->SetSelectedValue(useAbs ? "Absolute" : "Scale");
                            ddRaw->SetOnValueChanged([root, pipe, doc, resName](const std::string& v) mutable
                                                     {
                                                         if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                             return;
                                                         auto& r = doc["resources"][resName];
                                                         if (!r.contains("extent") || !r["extent"].is_object())
                                                             r["extent"] = nlohmann::json::object();
                                                         auto& ex = r["extent"];
                                                         const std::string m = ToLowerAscii(v);
                                                         if (m == "absolute")
                                                         {
                                                             ex.erase("scale");
                                                             ex["width"] = 1;
                                                             ex["height"] = 1;
                                                         }
                                                         else
                                                         {
                                                             ex.erase("width");
                                                             ex.erase("height");
                                                             ex["scale"] = nlohmann::json::array({1.0, 1.0});
                                                         }
                                                         SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                     });
                            rowMode->AddChild(std::move(dd));
                            resFoldoutPtr->AddChild(std::move(rowMode));
                        }

                        if (useAbs)
                        {
                            auto rowW = MakeSettingsRow();
                            AddRowLabel(rowW.get(), "extent.width");
                            auto iw = std::make_unique<IntField>();
                            iw->AddClass("settings-row-field");
                            iw->SetId("rp-res-" + idRes + "-extent-width");
                            iw->SetValue(w);
                            iw->SetOnValueChanged([root, pipe, doc, resName](const int& v) mutable
                                                  {
                                                      if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                          return;
                                                      auto& r = doc["resources"][resName];
                                                      if (!r.contains("extent") || !r["extent"].is_object())
                                                          r["extent"] = nlohmann::json::object();
                                                      r["extent"]["width"] = std::max(1, v);
                                                      SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                  });
                            rowW->AddChild(std::move(iw));
                            resFoldoutPtr->AddChild(std::move(rowW));

                            auto rowH = MakeSettingsRow();
                            AddRowLabel(rowH.get(), "extent.height");
                            auto ih = std::make_unique<IntField>();
                            ih->AddClass("settings-row-field");
                            ih->SetId("rp-res-" + idRes + "-extent-height");
                            ih->SetValue(h);
                            ih->SetOnValueChanged([root, pipe, doc, resName](const int& v) mutable
                                                  {
                                                      if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                          return;
                                                      auto& r = doc["resources"][resName];
                                                      if (!r.contains("extent") || !r["extent"].is_object())
                                                          r["extent"] = nlohmann::json::object();
                                                      r["extent"]["height"] = std::max(1, v);
                                                      SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                  });
                            rowH->AddChild(std::move(ih));
                            resFoldoutPtr->AddChild(std::move(rowH));
                        }
                        else
                        {
                            // scale is stored as an array [sx,sy]
                            auto rowSX = MakeSettingsRow();
                            AddRowLabel(rowSX.get(), "extent.scaleX");
                            auto fx = std::make_unique<FloatField>();
                            fx->AddClass("settings-row-field");
                            fx->SetId("rp-res-" + idRes + "-extent-scale-x");
                            fx->SetValue(sx);
                            fx->SetOnValueChanged([root, pipe, doc, resName](const float& v) mutable
                                                  {
                                                      if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                          return;
                                                      auto& r = doc["resources"][resName];
                                                      if (!r.contains("extent") || !r["extent"].is_object())
                                                          r["extent"] = nlohmann::json::object();
                                                      auto& ex = r["extent"];
                                                      if (!ex.contains("scale") || !ex["scale"].is_array() || ex["scale"].size() < 2)
                                                          ex["scale"] = nlohmann::json::array({1.0, 1.0});
                                                      ex["scale"][0] = (double)v;
                                                      SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                  });
                            rowSX->AddChild(std::move(fx));
                            resFoldoutPtr->AddChild(std::move(rowSX));

                            auto rowSY = MakeSettingsRow();
                            AddRowLabel(rowSY.get(), "extent.scaleY");
                            auto fy = std::make_unique<FloatField>();
                            fy->AddClass("settings-row-field");
                            fy->SetId("rp-res-" + idRes + "-extent-scale-y");
                            fy->SetValue(sy);
                            fy->SetOnValueChanged([root, pipe, doc, resName](const float& v) mutable
                                                  {
                                                      if (!doc.contains("resources") || !doc["resources"].is_object() || !doc["resources"].contains(resName))
                                                          return;
                                                      auto& r = doc["resources"][resName];
                                                      if (!r.contains("extent") || !r["extent"].is_object())
                                                          r["extent"] = nlohmann::json::object();
                                                      auto& ex = r["extent"];
                                                      if (!ex.contains("scale") || !ex["scale"].is_array() || ex["scale"].size() < 2)
                                                          ex["scale"] = nlohmann::json::array({1.0, 1.0});
                                                      ex["scale"][1] = (double)v;
                                                      SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                                  });
                            rowSY->AddChild(std::move(fy));
                            resFoldoutPtr->AddChild(std::move(rowSY));
                        }
                    }
                }
            }

            // Remove resource
            {
                auto row = MakeSettingsRow();
                AddRowLabel(row.get(), "");
                auto btn = std::make_unique<Button>();
                btn->AddClass("settings-row-field");
                btn->SetId("rp-res-" + idRes + "-remove");
                btn->SetText("Remove Resource");
                btn->RegisterEventHandler(kEventButtonClick, [root, pipe, doc, resName](UIEvent&) mutable
                                {
                                    if (!doc.contains("resources") || !doc["resources"].is_object())
                                        return;
                                    doc["resources"].erase(resName);
                                    SaveReloadRebuildUndoable(root, pipe, doc, s_UndoService, "Render Pipeline Edit");
                                });
                row->AddChild(std::move(btn));
                resFoldoutPtr->AddChild(std::move(row));
            }

            ++idx;
        }
    }

    // ---- Editor: passes ----
    if (!doc.contains("passes") || !doc["passes"].is_array())
    {
        doc["passes"] = nlohmann::json::array();
    }

    std::vector<std::string> knownPassTypes;
    if (auto* rs = GameEngine::EngineCore::GetInstance().GetRenderServices())
    {
        if (auto* reg = rs->Spine().GetPipelineNodeRegistry())
            knownPassTypes = reg->GetRegisteredTypes();
    }
    if (knownPassTypes.empty())
        knownPassTypes = {"WorldRender", "DepthPrepass", "ComputeShader", "FullscreenShader"};

    const std::vector<std::string> rgUsages = {
        "Auto",
        "Sampled",
        "StorageRead",
        "StorageWrite",
        "DepthRead",
        "DepthWrite",
        "CopySrc",
        "CopyDst",
    };

    // Right-click on Passes header: "Add Pass" context menu
    passesFoldoutPtr->RegisterEventHandler(kEventMouseDown,
        [root, pipe, doc, undo, knownPassTypes, passesFoldoutPtr](UIEvent& e) mutable {
            if (e.Button != 1)
                return;
            if (!s_HostWindow)
                return;

            auto menu = CreateContextMenu();
            if (!menu)
                return;

            constexpr uint32_t kCmdAddPass = 1;
            menu->AddItem(0, "Add Pass...", kCmdAddPass);
            menu->SetItemIcon(kCmdAddPass, EditorIcons::kPlus);
            menu->SetCommandHandler([root, pipe, doc, undo, knownPassTypes, passesFoldoutPtr](uint32_t cmd) mutable {
                if (cmd != kCmdAddPass)
                    return;
                UIManager* mgr = passesFoldoutPtr->GetOwnerManager();
                if (!mgr)
                    return;
                UIElement* uiRoot = mgr->GetRootElement();
                if (!uiRoot)
                    return;

                auto provider = std::make_shared<PassTypeSearchProvider>(knownPassTypes);
                auto dialog = std::make_unique<SearchDialog>();
                dialog->SetProvider(provider.get());
                SearchDialog* dialogPtr = dialog.get();

                dialog->SetOnResult([root, pipe, doc, undo, uiRoot, dialogPtr, provider](const SearchResultItem& item) mutable {
                    auto* typeName = std::any_cast<std::string>(&item.UserData);
                    if (!typeName)
                        return;
                    nlohmann::json p = MakeDefaultPassForType(*typeName, doc["passes"]);
                    doc["passes"].push_back(std::move(p));
                    uiRoot->RemoveChild(dialogPtr);
                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Add Pass");
                });
                dialog->SetOnCancel([uiRoot, dialogPtr, provider]() {
                    uiRoot->RemoveChild(dialogPtr);
                });

                float anchorX = passesFoldoutPtr->GetLayoutX();
                float anchorY = passesFoldoutPtr->GetLayoutY() + 24.0f;
                uiRoot->AddChild(std::move(dialog));
                dialogPtr->SetAnchorPosition(anchorX, anchorY);
                dialogPtr->Show();
            });
            menu->Show(s_HostWindow, (int)e.X, (int)e.Y);
            e.Stop();
        });

    // Pass list: drag a pass's header to reorder the passes. Entries are keyed by pass index.
    auto passContainer = std::make_unique<EditorUI::ReorderableSectionList>(
        "RenderPipelinePasses",
        [root, pipe, doc, undo](uint64_t sourceKey, uint64_t targetKey, bool after) mutable {
        if (!doc.contains("passes") || !doc["passes"].is_array())
            return;
        const size_t srcIdx = static_cast<size_t>(sourceKey);
        size_t dstIdx = static_cast<size_t>(targetKey) + (after ? 1u : 0u);
        // The target position counts the dragged pass, which leaves its place first.
        if (srcIdx < dstIdx)
            --dstIdx;
        if (srcIdx == dstIdx)
            return;
        if (srcIdx >= doc["passes"].size() || dstIdx > doc["passes"].size())
            return;
        auto moved = doc["passes"][srcIdx];
        doc["passes"].erase(srcIdx);
        if (dstIdx > doc["passes"].size())
            dstIdx = doc["passes"].size();
        doc["passes"].insert(doc["passes"].begin() + static_cast<ptrdiff_t>(dstIdx), std::move(moved));
        SaveReloadRebuildUndoable(root, pipe, doc, undo, "Reorder Passes");
    });
    EditorUI::ReorderableSectionList* passContainerPtr = passContainer.get();
    passesFoldoutPtr->AddChild(std::move(passContainer));

    for (size_t i = 0; i < doc["passes"].size(); ++i)
    {
        if (!doc["passes"][i].is_object())
            continue;

        const std::string passId = doc["passes"][i].value("id", "<missing>");
        const std::string passType = doc["passes"][i].value("type", "");
        const std::string foldTitle =
            std::string("Pass ") + std::to_string(i) + ": " + passId + (passType.empty() ? "" : (" (" + passType + ")"));

        auto passFoldout = std::make_unique<Foldout>();
        passFoldout->SetTitle(foldTitle);
        passFoldout->SetId("rp-pass-" + std::to_string(i) + "-foldout");
        passFoldout->AddClass("rp-foldout");
        passFoldout->SetExpanded(FoldoutStateCache::Get(pipe, passFoldout->GetId(), false));
        NormalizeRenderPipelineFoldoutContent(passFoldout.get());
        passFoldout->SetOnExpandedChanged([pipe](Foldout& f, bool expanded)
                                          { FoldoutStateCache::Set(pipe, f.GetId(), expanded); });
        UIElement* passUI = passFoldout.get();

        {
        // enabled
        {
            AddInspectorToggleRow(passUI, "Enabled", doc["passes"][i].value("enabled", true),
                "rp-pass-" + std::to_string(i) + "-enabled",
                [root, pipe, doc, undo, i](bool v) mutable
                {
                    if (i < doc["passes"].size() && doc["passes"][i].is_object())
                        doc["passes"][i]["enabled"] = v;
                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Pass Enabled");
                });
        }

        // id
        {
            AddInspectorTextRow(passUI, "Id", doc["passes"][i].value("id", ""),
                "rp-pass-" + std::to_string(i) + "-id",
                [root, pipe, doc, undo, i](const std::string& v) mutable
                {
                    if (i < doc["passes"].size() && doc["passes"][i].is_object())
                        doc["passes"][i]["id"] = v;
                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Pass Id");
                });
        }

        // type
        {
            const std::string currentType = doc["passes"][i].value("type", "");

            std::vector<Dropdown::Option> opts;
            opts.reserve(knownPassTypes.size() + 1);

            int selectedIndex = 0;
            bool found = false;
            for (size_t k = 0; k < knownPassTypes.size(); ++k)
            {
                if (!currentType.empty() && knownPassTypes[k] == currentType)
                {
                    found = true;
                    selectedIndex = static_cast<int>(opts.size());
                }
                opts.push_back(Dropdown::Option{knownPassTypes[k], knownPassTypes[k]});
            }
            if (!found && !currentType.empty())
            {
                // Keep any unknown/custom type selectable so we don't destroy data.
                opts.insert(opts.begin(), Dropdown::Option{currentType, currentType + " (custom)"});
                selectedIndex = 0;
            }

            AddInspectorDropdownRow(passUI, "Type", opts, selectedIndex,
                "rp-pass-" + std::to_string(i) + "-type",
                [root, pipe, doc, undo, i](const std::string& v) mutable
                {
                    if (i < doc["passes"].size() && doc["passes"][i].is_object())
                    {
                        doc["passes"][i]["type"] = v;
                        const nlohmann::json defaults = MakeDefaultPassForType(v, doc["passes"]);
                        for (auto it = defaults.begin(); it != defaults.end(); ++it)
                        {
                            if (it.key() == "id" || it.key() == "type" ||
                                doc["passes"][i].contains(it.key()))
                                continue;
                            doc["passes"][i][it.key()] = it.value();
                        }
                    }
                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Pass Type");
                });
        }

        // shaderPkg (for FullscreenShader / ComputeShader)
        {
            const std::string typeLower = ToLowerAscii(doc["passes"][i].value("type", ""));
            if (typeLower == "fullscreenshader" || typeLower == "computeshader")
            {
                AddInspectorTextRow(passUI, "shaderPkg", doc["passes"][i].value("shaderPkg", ""),
                    "rp-pass-" + std::to_string(i) + "-shaderPkg",
                    [root, pipe, doc, undo, i](const std::string& v) mutable
                    {
                        if (i < doc["passes"].size() && doc["passes"][i].is_object())
                            doc["passes"][i]["shaderPkg"] = v;
                        SaveReloadRebuildUndoable(root, pipe, doc, undo, "Shader Package");
                    });
            }
        }

        // FullscreenShader specific
        {
            const std::string typeLower = ToLowerAscii(doc["passes"][i].value("type", ""));
            if (typeLower == "fullscreenshader")
            {
                // alphaBlend
                {
                    AddInspectorToggleRow(passUI, "alphaBlend", doc["passes"][i].value("alphaBlend", false),
                        "rp-pass-" + std::to_string(i) + "-alphaBlend",
                        [root, pipe, doc, undo, i](bool v) mutable
                        {
                            if (i < doc["passes"].size() && doc["passes"][i].is_object())
                                doc["passes"][i]["alphaBlend"] = v;
                            SaveReloadRebuildUndoable(root, pipe, doc, undo, "Alpha Blend");
                        });
                }

                BuildStringMapEditor(passUI, root, "inputs:", pipe, doc, i, "inputs", "rp-pass-" + std::to_string(i) + "-inputs", /*allowDropRefs=*/true);
                BuildStringMapEditor(passUI, root, "samplers:", pipe, doc, i, "samplers", "rp-pass-" + std::to_string(i) + "-samplers", /*allowDropRefs=*/false);
                BuildStringMapEditor(passUI, root, "buffers:", pipe, doc, i, "buffers", "rp-pass-" + std::to_string(i) + "-buffers", /*allowDropRefs=*/true);
                BuildUsageMapEditor(passUI,
                                    root,
                                    "bufferUsages:",
                                    pipe,
                                    doc,
                                    i,
                                    "bufferUsages",
                                    "rp-pass-" + std::to_string(i) + "-bufferUsages",
                                    rgUsages,
                                    "StorageRead");

                // Push constants: editable float/int fields for each entry.
                if (doc["passes"][i].contains("pushConstants") && doc["passes"][i]["pushConstants"].is_object())
                {
                    AddTextBlock(passUI, "pushConstants:", "inspector-section-subheader");
                    const auto& pcObj = doc["passes"][i]["pushConstants"];
                    for (auto it = pcObj.begin(); it != pcObj.end(); ++it)
                    {
                        const std::string key = it.key();
                        // Skip padding fields (pad, pad0, pad1, etc.)
                        if (key.size() >= 3 && key.substr(0, 3) == "pad" && (key.size() == 3 || std::isdigit(key[3])))
                            continue;
                        if (it.value().is_number_float())
                        {
                            AddInspectorFloatRow(passUI, key, it.value().get<float>(), it.value().get<float>(),
                                -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::infinity(),
                                "rp-pass-" + std::to_string(i) + "-pc-" + key,
                                [root, pipe, doc, undo, i, key](float v) mutable
                                {
                                    if (i < doc["passes"].size() && doc["passes"][i].is_object())
                                        doc["passes"][i]["pushConstants"][key] = v;
                                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Push Constant " + key);
                                });
                        }
                        else if (it.value().is_number_integer())
                        {
                            AddInspectorIntRow(passUI, key, it.value().get<int>(), it.value().get<int>(),
                                "rp-pass-" + std::to_string(i) + "-pc-" + key,
                                [root, pipe, doc, undo, i, key](int v) mutable
                                {
                                    if (i < doc["passes"].size() && doc["passes"][i].is_object())
                                        doc["passes"][i]["pushConstants"][key] = v;
                                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Push Constant " + key);
                                });
                        }
                    }
                }
            }
        }

        // ComputeShader specific
        {
            const std::string typeLower = ToLowerAscii(doc["passes"][i].value("type", ""));
            if (typeLower == "computeshader")
            {
                // targetBuffer/targetTexture
                {
                    AddInspectorTextRow(passUI, "targetBuffer", doc["passes"][i].value("targetBuffer", ""),
                        "rp-pass-" + std::to_string(i) + "-targetBuffer",
                        [root, pipe, doc, undo, i](const std::string& v) mutable
                        {
                            if (i < doc["passes"].size() && doc["passes"][i].is_object())
                                doc["passes"][i]["targetBuffer"] = v;
                            SaveReloadRebuildUndoable(root, pipe, doc, undo, "Target Buffer");
                        });
                }
                {
                    AddInspectorTextRow(passUI, "targetTexture", doc["passes"][i].value("targetTexture", ""),
                        "rp-pass-" + std::to_string(i) + "-targetTexture",
                        [root, pipe, doc, undo, i](const std::string& v) mutable
                        {
                            if (i < doc["passes"].size() && doc["passes"][i].is_object())
                                doc["passes"][i]["targetTexture"] = v;
                            SaveReloadRebuildUndoable(root, pipe, doc, undo, "Target Texture");
                        });
                }

                // dispatch expressions
                {
                    if (!doc["passes"][i].contains("dispatch") || !doc["passes"][i]["dispatch"].is_object())
                        doc["passes"][i]["dispatch"] = nlohmann::json::object();
                    auto& d = doc["passes"][i]["dispatch"];
                    const std::string x = d.contains("x") ? d["x"].dump() : std::string("1");
                    const std::string y = d.contains("y") ? d["y"].dump() : std::string("1");
                    const std::string z = d.contains("z") ? d["z"].dump() : std::string("1");

                    auto makeDispatchField = [&](const char* label, const char* axis, const std::string& initial, const std::string& id)
                    {
                        // Stored JSON might be a number; normalize to string without quotes.
                        std::string v = initial;
                        if (!v.empty() && v.front() == '"' && v.back() == '"')
                            v = v.substr(1, v.size() - 2);
                        AddInspectorTextRow(passUI, label, v, id,
                            [root, pipe, doc, undo, i, axis](const std::string& v2) mutable
                            {
                                if (i >= doc["passes"].size() || !doc["passes"][i].is_object())
                                    return;
                                if (!doc["passes"][i].contains("dispatch") || !doc["passes"][i]["dispatch"].is_object())
                                    doc["passes"][i]["dispatch"] = nlohmann::json::object();
                                doc["passes"][i]["dispatch"][axis] = v2;
                                SaveReloadRebuildUndoable(root, pipe, doc, undo, std::string("Dispatch ") + axis);
                            });
                    };

                    makeDispatchField("dispatch.x", "x", x, "rp-pass-" + std::to_string(i) + "-dispatch-x");
                    makeDispatchField("dispatch.y", "y", y, "rp-pass-" + std::to_string(i) + "-dispatch-y");
                    makeDispatchField("dispatch.z", "z", z, "rp-pass-" + std::to_string(i) + "-dispatch-z");
                }

                BuildStringMapEditor(passUI, root, "inputs:", pipe, doc, i, "inputs", "rp-pass-" + std::to_string(i) + "-inputs", /*allowDropRefs=*/true);
                BuildUsageMapEditor(passUI,
                                    root,
                                    "inputUsages:",
                                    pipe,
                                    doc,
                                    i,
                                    "inputUsages",
                                    "rp-pass-" + std::to_string(i) + "-inputUsages",
                                    rgUsages,
                                    "Sampled");
                BuildStringMapEditor(passUI, root, "samplers:", pipe, doc, i, "samplers", "rp-pass-" + std::to_string(i) + "-samplers", /*allowDropRefs=*/false);
                BuildStringMapEditor(passUI, root, "buffers:", pipe, doc, i, "buffers", "rp-pass-" + std::to_string(i) + "-buffers", /*allowDropRefs=*/true);
                BuildUsageMapEditor(passUI,
                                    root,
                                    "bufferUsages:",
                                    pipe,
                                    doc,
                                    i,
                                    "bufferUsages",
                                    "rp-pass-" + std::to_string(i) + "-bufferUsages",
                                    rgUsages,
                                    "StorageRead");

                // uniformWrites (u32x4) editor
                AddTextBlock(passUI, "uniformWrites (u32x4):");
                if (!doc["passes"][i].contains("uniformWrites") || !doc["passes"][i]["uniformWrites"].is_object())
                    doc["passes"][i]["uniformWrites"] = nlohmann::json::object();
                {
                    size_t uwIdx = 0;
                    const auto& uw = doc["passes"][i]["uniformWrites"];
                    for (auto it = uw.begin(); it != uw.end(); ++it, ++uwIdx)
                    {
                        const std::string binding = it.key();
                        const auto& entry = it.value();
                        std::array<int, 4> v{0, 0, 0, 0};
                        if (entry.is_object() && entry.contains("u32x4") && entry["u32x4"].is_array() && entry["u32x4"].size() == 4)
                        {
                            for (int k = 0; k < 4; ++k)
                            {
                                if (entry["u32x4"][k].is_number_integer())
                                    v[k] = (int)entry["u32x4"][k].get<int64_t>();
                            }
                        }

                        AddTextBlock(passUI, std::string("- ") + binding);

                        // 4 int fields
                        for (int k = 0; k < 4; ++k)
                        {
                            AddInspectorIntRow(passUI, std::string("u32x4[") + std::to_string(k) + "]",
                                v[k], v[k],
                                "rp-pass-" + std::to_string(i) + "-uw-" + std::to_string(uwIdx) + "-" + std::to_string(k),
                                [root, pipe, doc, undo, i, binding, k](int newV) mutable
                                {
                                    if (i >= doc["passes"].size() || !doc["passes"][i].is_object())
                                        return;
                                    if (!doc["passes"][i].contains("uniformWrites") || !doc["passes"][i]["uniformWrites"].is_object())
                                        doc["passes"][i]["uniformWrites"] = nlohmann::json::object();
                                    auto& uw2 = doc["passes"][i]["uniformWrites"];
                                    if (!uw2.contains(binding) || !uw2[binding].is_object())
                                        uw2[binding] = nlohmann::json::object();
                                    auto& e = uw2[binding];
                                    if (!e.contains("u32x4") || !e["u32x4"].is_array() || e["u32x4"].size() != 4)
                                        e["u32x4"] = nlohmann::json::array({0, 0, 0, 0});
                                    e["u32x4"][k] = newV;
                                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Uniform Write");
                                });
                        }

                        // Remove binding
                        {
                            AddInspectorButtonRow(passUI, "", "Remove uniformWrites entry",
                                "rp-pass-" + std::to_string(i) + "-uw-remove-" + std::to_string(uwIdx),
                                [root, pipe, doc, undo, i, binding](UIEvent&) mutable
                                {
                                    if (i >= doc["passes"].size() || !doc["passes"][i].is_object())
                                        return;
                                    if (doc["passes"][i].contains("uniformWrites") && doc["passes"][i]["uniformWrites"].is_object())
                                        doc["passes"][i]["uniformWrites"].erase(binding);
                                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Remove Uniform Write");
                                });
                        }
                    }

                    // Add binding
                    {
                        AddInspectorButtonRow(passUI, "", "Add uniformWrites entry",
                            "rp-pass-" + std::to_string(i) + "-uw-add",
                            [root, pipe, doc, undo, i](UIEvent&) mutable
                            {
                                if (i >= doc["passes"].size() || !doc["passes"][i].is_object())
                                    return;
                                if (!doc["passes"][i].contains("uniformWrites") || !doc["passes"][i]["uniformWrites"].is_object())
                                    doc["passes"][i]["uniformWrites"] = nlohmann::json::object();
                                auto& uw2 = doc["passes"][i]["uniformWrites"];
                                const std::string k = MakeUniqueKeyForObject(uw2, "NewUniform");
                                uw2[k] = nlohmann::json::object({{"u32x4", nlohmann::json::array({0, 0, 0, 0})}});
                                SaveReloadRebuildUndoable(root, pipe, doc, undo, "Add Uniform Write");
                            });
                    }
                }
            }
        }

        // output (for FullscreenShader)
        {
            const std::string typeLower = ToLowerAscii(doc["passes"][i].value("type", ""));
            if (typeLower == "fullscreenshader")
            {
                AddInspectorTextRow(passUI, "output", doc["passes"][i].value("output", "View.Resolve"),
                    "rp-pass-" + std::to_string(i) + "-output",
                    [root, pipe, doc, undo, i](const std::string& v) mutable
                    {
                        if (i < doc["passes"].size() && doc["passes"][i].is_object())
                            doc["passes"][i]["output"] = v;
                        SaveReloadRebuildUndoable(root, pipe, doc, undo, "Pass Output");
                    });
            }
        }

        // Existing diagnostics view (kept; helpful while authoring)
        {
            const std::string typeLower = ToLowerAscii(doc["passes"][i].value("type", ""));
            if (typeLower == "fullscreenshader")
            {
                const std::string shaderPkg = doc["passes"][i].value("shaderPkg", "");
                std::vector<std::string> inputNames;
                if (doc["passes"][i].contains("inputs") && doc["passes"][i]["inputs"].is_object())
                {
                    for (auto it = doc["passes"][i]["inputs"].begin(); it != doc["passes"][i]["inputs"].end(); ++it)
                    {
                        if (it.value().is_string())
                            inputNames.push_back(it.key());
                    }
                }
                if (!shaderPkg.empty())
                {
                    AddTextBlock(passUI, FormatShaderPkgValidation(shaderPkg, inputNames));
                }
            }
        }
        }

        // The pass's options menu on right-click; the pass list owns the drag to reorder.
        {
            Foldout* foldoutPtr = passFoldout.get();

            foldoutPtr->RegisterEventHandler(kEventMouseDown,
                [root, pipe, doc, undo, i, foldoutPtr, knownPassTypes](UIEvent& e) mutable {
                    if (e.Button == 1)
                    {
                        // Right-click: show context menu with Remove Pass.
                        auto menu = CreateContextMenu();
                        if (!menu || !s_HostWindow)
                            return;
                        constexpr uint32_t kCmdMoveUp = 1;
                        constexpr uint32_t kCmdMoveDown = 2;
                        constexpr uint32_t kCmdRemovePass = 3;
                        const size_t passCount = doc.contains("passes") && doc["passes"].is_array() ? doc["passes"].size() : 0;
                        menu->AddItem(0, "Move Up", kCmdMoveUp, i == 0 ? MenuItemFlag_Disabled : MenuItemFlag_None);
                        menu->SetItemIcon(kCmdMoveUp, EditorIcons::kArrowUp);
                        menu->AddItem(0, "Move Down", kCmdMoveDown, (i + 1 >= passCount) ? MenuItemFlag_Disabled : MenuItemFlag_None);
                        menu->SetItemIcon(kCmdMoveDown, EditorIcons::kArrowDown);
                        menu->AddSeparator(0);
                        constexpr uint32_t kCmdAddPass = 4;
                        menu->AddItem(0, "Add Pass...", kCmdAddPass);
                        menu->SetItemIcon(kCmdAddPass, EditorIcons::kPlus);
                        menu->AddSeparator(0);
                        menu->AddItem(0, "Remove Pass", kCmdRemovePass);
                        menu->SetItemIcon(kCmdRemovePass, EditorIcons::kTrash);
                        menu->SetCommandHandler([root, pipe, doc, undo, i, knownPassTypes, foldoutPtr](uint32_t cmd) mutable {
                            if (!doc.contains("passes") || !doc["passes"].is_array())
                                return;
                            if (cmd == kCmdMoveUp && i > 0 && i < doc["passes"].size())
                            {
                                std::swap(doc["passes"][i], doc["passes"][i - 1]);
                                SaveReloadRebuildUndoable(root, pipe, doc, undo, "Move Pass Up");
                            }
                            else if (cmd == kCmdMoveDown && i + 1 < doc["passes"].size())
                            {
                                std::swap(doc["passes"][i], doc["passes"][i + 1]);
                                SaveReloadRebuildUndoable(root, pipe, doc, undo, "Move Pass Down");
                            }
                            else if (cmd == kCmdAddPass)
                            {
                                UIManager* mgr = foldoutPtr->GetOwnerManager();
                                if (!mgr)
                                    return;
                                UIElement* uiRoot = mgr->GetRootElement();
                                if (!uiRoot)
                                    return;
                                auto provider = std::make_shared<PassTypeSearchProvider>(knownPassTypes);
                                auto dialog = std::make_unique<SearchDialog>();
                                dialog->SetProvider(provider.get());
                                SearchDialog* dialogPtr = dialog.get();
                                dialog->SetOnResult([root, pipe, doc, undo, uiRoot, dialogPtr, provider](const SearchResultItem& item) mutable {
                                    auto* typeName = std::any_cast<std::string>(&item.UserData);
                                    if (!typeName)
                                        return;
                                    nlohmann::json p = MakeDefaultPassForType(*typeName, doc["passes"]);
                                    doc["passes"].push_back(std::move(p));
                                    uiRoot->RemoveChild(dialogPtr);
                                    SaveReloadRebuildUndoable(root, pipe, doc, undo, "Add Pass");
                                });
                                dialog->SetOnCancel([uiRoot, dialogPtr, provider]() {
                                    uiRoot->RemoveChild(dialogPtr);
                                });
                                float anchorX = foldoutPtr->GetLayoutX();
                                float anchorY = foldoutPtr->GetLayoutY() + foldoutPtr->GetLayoutHeight();
                                uiRoot->AddChild(std::move(dialog));
                                dialogPtr->SetAnchorPosition(anchorX, anchorY);
                                dialogPtr->Show();
                            }
                            else if (cmd == kCmdRemovePass && i < doc["passes"].size())
                            {
                                doc["passes"].erase(doc["passes"].begin() + (nlohmann::json::difference_type)i);
                                SaveReloadRebuildUndoable(root, pipe, doc, undo, "Remove Pass");
                            }
                        });
                        menu->Show(s_HostWindow, (int)e.X, (int)e.Y);
                        e.Stop();
                    }
                });
        }

        // Commit this pass foldout.
        Foldout* passFoldoutPtr = passFoldout.get();
        passContainerPtr->AddChild(std::move(passFoldout));
        passContainerPtr->AddEntry(i, passFoldoutPtr, "Drag to reorder this pass.");
    }

    NormalizeRenderPipelineInspectorRows(root);

    // Render-pipeline assets still contain a few generic JSON editors whose exact
    // keys are only known at runtime. Make their numeric labels behave like the
    // rest of the Inspector: drag to change, double-click to reset to the value
    // shown when this inspector was built, and commit through the field's normal
    // undoable callback.
    InspectorDrag::AutoSetupDragForInspector(root);
}

} // namespace

void RegisterRenderPipelineInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.Object)
            return;

        auto* base = static_cast<Asset*>(ctx.Object);
        auto* pipe = dynamic_cast<RenderPipelineAsset*>(base);
        if (!pipe)
            return;
        s_HostWindow = ctx.Window;
        s_UndoService = ctx.Undo;
        BuildRenderPipelineEditorUI(ctx.Parent, pipe, ctx.Undo);
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::RenderPipeline, std::move(fn));
}

} // namespace GameEngine
