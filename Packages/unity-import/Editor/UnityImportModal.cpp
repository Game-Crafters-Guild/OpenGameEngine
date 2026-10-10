#include "UnityImportModal.h"

#include "Editor/Assets/UnityImportHostedConverter.h"

#include "Platform/Shell.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <system_error>

namespace GameEngine {

using nlohmann::json;

namespace {

constexpr uint32_t kColBackdrop = 0x99000000;
constexpr uint32_t kColWindow = 0xFF2C2C2C;
constexpr uint32_t kColBar = 0xFF1D1C1D;
constexpr uint32_t kColBorder = 0xFF444444;
constexpr uint32_t kColField = 0xFF1E1E1E;
constexpr uint32_t kColText = 0xFFFFFFFF;
constexpr uint32_t kColMuted = 0xFFB0B0B0;
constexpr uint32_t kColWarn = 0xFFFFB4A8;
constexpr uint32_t kColAccent = 0xFF4C9AFF;
constexpr float kWindowWidthPx = 620.0f;

void StyleLabel(Label* label, uint32_t color, float fontSize) {
    label->Overrides().Set(Style::Color, color).Set(Style::FontSize, StyleLength::Px(fontSize));
}

void StyleField(TextField* field) {
    field->Overrides()
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::BackgroundColor, kColField)
        .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF555555, 0xFF555555, 0xFF555555, 0xFF555555})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4, 4, 4, 4})
        .Set(Style::PaddingTop, StyleLength::Px(6.0f)).Set(Style::PaddingRight, StyleLength::Px(8.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(6.0f)).Set(Style::PaddingLeft, StyleLength::Px(8.0f))
        .Set(Style::Color, kColText);
}

long long JsonInt(const json& obj, const char* key, long long fallback = 0) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return fallback;
    return it->get<long long>();
}

// Windows: absolute, backslash-normalized, \\?\-prefixed. The import stage
// nests <cache>/UnityImport/import-<stamp>/assets/ on top of deep
// pack-relative model paths, which can cross the legacy MAX_PATH limit; the
// converter writes those files fine (.NET is long-path aware), but MSVC's
// std::filesystem issues unprefixed Win32 calls, so plain rename/copy/remove
// fail on them with ERROR_PATH_NOT_FOUND. Applied unconditionally so the
// prefix path is exercised by every import, not only deep installs.
#ifdef _WIN32
std::filesystem::path LongPathSafe(const std::filesystem::path& p) {
    std::filesystem::path abs = std::filesystem::absolute(p);
    abs.make_preferred();
    const std::wstring& native = abs.native();
    if (native.rfind(LR"(\\?\)", 0) == 0 || native.rfind(LR"(\\)", 0) == 0)
        return abs;
    return std::filesystem::path(LR"(\\?\)" + native);
}
#else
const std::filesystem::path& LongPathSafe(const std::filesystem::path& p) {
    return p;
}
#endif

} // namespace

UnityImportModal::UnityImportModal() {
    BuildUI();
}

UnityImportModal::~UnityImportModal() {
    // Ask a still-running hosted conversion to unwind at its next output line
    // so teardown doesn't wait out a long import.
    m_CancelRequested.store(true);
    if (m_Worker.joinable())
        m_Worker.join();
}

void UnityImportModal::SetProjectContext(std::filesystem::path defaultDestination,
                                         std::filesystem::path cacheRoot,
                                         std::filesystem::path assetDbPath) {
    m_DefaultDestination = std::move(defaultDestination);
    m_CacheRoot = std::move(cacheRoot);
    m_AssetDbPath = std::move(assetDbPath);
}

void UnityImportModal::BuildUI() {
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f)).Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f)).Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::ZIndex, 10000)
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::PointerEvents, false);

    auto backdrop = std::make_unique<UIElement>();
    backdrop->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f)).Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f)).Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::BackgroundColor, kColBackdrop)
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, true);

    auto window = std::make_unique<UIElement>();
    window->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Position, PositionType::Relative)
        .Set(Style::BackgroundColor, kColWindow)
        .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
        .Set(Style::BorderColor, BorderColorsTRBL{kColBorder, kColBorder, kColBorder, kColBorder})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{8, 8, 8, 8})
        .Set(Style::BoxShadowProp, BoxShadowValue{0.0f, 8.0f, 24.0f, 0x99000000u})
        .Set(Style::Width, StyleLength::Px(kWindowWidthPx))
        .Set(Style::MaxWidth, StyleLength::Percent(88.0f))
        .Set(Style::OverflowProp, Overflow::Hidden);

    // Header.
    {
        auto header = std::make_unique<UIElement>();
        header->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(14.0f)).Set(Style::PaddingRight, StyleLength::Px(18.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(14.0f)).Set(Style::PaddingLeft, StyleLength::Px(18.0f))
            .Set(Style::BackgroundColor, kColBar)
            .Set(Style::BorderBottomWidth, 1.0f).Set(Style::BorderBottomColor, (uint32_t)0xFF333333);
        auto title = std::make_unique<Label>();
        title->SetText("Import Unity Package");
        title->Overrides().Set(Style::Color, kColText).Set(Style::FontSize, StyleLength::Px(17.0f)).Set(Style::FontWeight, 600);
        header->AddChild(std::move(title));
        window->AddChild(std::move(header));
    }

    auto content = std::make_unique<UIElement>();
    content->Overrides()
        .Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingTop, StyleLength::Px(16.0f)).Set(Style::PaddingRight, StyleLength::Px(18.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(16.0f)).Set(Style::PaddingLeft, StyleLength::Px(18.0f))
        .Set(Style::BackgroundColor, kColWindow);

    auto column = [](float gap) {
        auto e = std::make_unique<UIElement>();
        e->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Column).Set(Style::Gap, StyleLength::Px(gap));
        return e;
    };
    auto row = [](float gap) {
        auto e = std::make_unique<UIElement>();
        e->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::Gap, StyleLength::Px(gap)).Set(Style::AlignItems, AlignItems::Center);
        return e;
    };
    auto captionLabel = [](const std::string& text) {
        auto l = std::make_unique<Label>();
        l->SetText(text);
        StyleLabel(l.get(), kColMuted, 12.5f);
        return l;
    };

    // ---- Config section --------------------------------------------------
    {
        auto section = column(12.0f);
        m_ConfigSection = section.get();

        // Bundle row.
        {
            auto group = column(4.0f);
            group->AddChild(captionLabel("Unity package (.unitypackage) or extracted bundle folder"));
            auto r = row(8.0f);
            auto field = std::make_unique<TextField>();
            m_BundleField = field.get();
            StyleField(field.get());
            field->Overrides().Set(Style::FlexGrow, 1.0f).Set(Style::FlexShrink, 1.0f);
            // Typing or pasting a path + Enter loads it — the "extracted bundle
            // folder" affordance the native file dialog can't offer directly.
            field->SetOnCommit([this]() {
                if (!m_BundleField) return;
                const std::string v = m_BundleField->GetValue();
                if (v.empty()) return;
                m_BundlePath = std::filesystem::path(v);
                StartListScenes(m_BundlePath);
            });
            r->AddChild(std::move(field));
            auto browse = std::make_unique<Button>();
            browse->SetText("Browse…");
            browse->AddClass("secondary");
            browse->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseBundle(); });
            r->AddChild(std::move(browse));
            group->AddChild(std::move(r));
            section->AddChild(std::move(group));
        }

        // Scene list.
        {
            auto group = column(4.0f);
            auto head = std::make_unique<Label>();
            m_SceneHeaderLabel = head.get();
            head->SetText("Scenes in bundle");
            StyleLabel(head.get(), kColText, 13.5f);
            head->Overrides().Set(Style::FontWeight, 600);
            group->AddChild(std::move(head));

            // Scene rows live inside a real ScrollView so big packs (dozens of
            // scenes) scroll instead of clipping at the old MaxHeight cutoff.
            auto scroll = std::make_unique<ScrollView>();
            m_SceneListScroll = scroll.get();
            scroll->Overrides()
                .Set(Style::FlexGrow, 0.0f).Set(Style::FlexShrink, 0.0f)
                .Set(Style::MaxHeight, StyleLength::Px(180.0f))
                .Set(Style::BackgroundColor, (uint32_t)0xFF232323)
                .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4, 4, 4, 4})
                .Set(Style::OverflowProp, Overflow::Hidden);
            auto list = std::make_unique<UIElement>();
            m_SceneListContainer = list.get();
            list->Overrides()
                .Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Column).Set(Style::Gap, StyleLength::Px(2.0f))
                .Set(Style::PaddingTop, StyleLength::Px(6.0f)).Set(Style::PaddingRight, StyleLength::Px(8.0f))
                .Set(Style::PaddingBottom, StyleLength::Px(6.0f)).Set(Style::PaddingLeft, StyleLength::Px(8.0f));
            scroll->AddContent(std::move(list));
            group->AddChild(std::move(scroll));

            auto shared = std::make_unique<Label>();
            m_SharedAssetsLabel = shared.get();
            shared->SetText("");
            StyleLabel(shared.get(), kColMuted, 12.0f);
            group->AddChild(std::move(shared));
            section->AddChild(std::move(group));
        }

        // Destination.
        {
            auto group = column(4.0f);
            group->AddChild(captionLabel("Destination (inside the current project's assets)"));
            auto r = row(8.0f);
            auto field = std::make_unique<TextField>();
            m_DestField = field.get();
            StyleField(field.get());
            field->Overrides().Set(Style::FlexGrow, 1.0f).Set(Style::FlexShrink, 1.0f);
            r->AddChild(std::move(field));
            auto browse = std::make_unique<Button>();
            browse->SetText("Browse…");
            browse->AddClass("secondary");
            browse->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseDestination(); });
            r->AddChild(std::move(browse));
            group->AddChild(std::move(r));
            section->AddChild(std::move(group));
        }

        // Textures status.
        {
            auto l = std::make_unique<Label>();
            m_TexturesLabel = l.get();
            l->SetText("");
            StyleLabel(l.get(), kColMuted, 12.0f);
            section->AddChild(std::move(l));
        }

        // Inline warning (no scenes selected / converter missing).
        {
            auto l = std::make_unique<Label>();
            m_ConfigWarningLabel = l.get();
            l->SetText("");
            StyleLabel(l.get(), kColWarn, 12.5f);
            l->Overrides().Set(Style::Display, DisplayMode::None);
            section->AddChild(std::move(l));
        }

        // Footer buttons.
        {
            auto footer = row(8.0f);
            footer->Overrides().Set(Style::JustifyContent, JustifyContent::FlexEnd).Set(Style::PaddingTop, StyleLength::Px(6.0f));
            auto cancel = std::make_unique<Button>();
            cancel->SetText("Cancel");
            cancel->AddClass("secondary");
            cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancel(); });
            footer->AddChild(std::move(cancel));
            auto import = std::make_unique<Button>();
            import->SetText("Import");
            import->AddClass("primary");
            import->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnImport(); });
            footer->AddChild(std::move(import));
            section->AddChild(std::move(footer));
        }

        content->AddChild(std::move(section));
    }

    // ---- Busy / progress section ----------------------------------------
    {
        auto section = column(10.0f);
        m_BusySection = section.get();
        section->Overrides().Set(Style::Display, DisplayMode::None);

        auto phase = std::make_unique<Label>();
        m_PhaseLabel = phase.get();
        phase->SetText("Preparing…");
        StyleLabel(phase.get(), kColText, 13.5f);
        section->AddChild(std::move(phase));

        auto track = std::make_unique<UIElement>();
        track->Overrides()
            .Set(Style::Width, StyleLength::Percent(100.0f)).Set(Style::Height, StyleLength::Px(14.0f))
            .Set(Style::BackgroundColor, kColField)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{7, 7, 7, 7})
            .Set(Style::OverflowProp, Overflow::Hidden);
        auto fill = std::make_unique<UIElement>();
        m_ProgressFill = fill.get();
        fill->Overrides()
            .Set(Style::Width, StyleLength::Percent(0.0f)).Set(Style::Height, StyleLength::Percent(100.0f))
            .Set(Style::BackgroundColor, kColAccent);
        track->AddChild(std::move(fill));
        section->AddChild(std::move(track));

        auto sub = std::make_unique<Label>();
        m_TextureSubLabel = sub.get();
        sub->SetText("");
        StyleLabel(sub.get(), kColMuted, 12.0f);
        sub->Overrides().Set(Style::Display, DisplayMode::None);
        section->AddChild(std::move(sub));

        content->AddChild(std::move(section));
    }

    // ---- Complete section -----------------------------------------------
    {
        auto section = column(12.0f);
        m_CompleteSection = section.get();
        section->Overrides().Set(Style::Display, DisplayMode::None);

        auto summary = std::make_unique<Label>();
        m_SummaryLabel = summary.get();
        summary->SetText("");
        StyleLabel(summary.get(), kColText, 13.0f);
        section->AddChild(std::move(summary));

        auto openRow = std::make_unique<UIElement>();
        m_OpenSceneRow = openRow.get();
        openRow->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::FlexWrap, true).Set(Style::Gap, StyleLength::Px(8.0f));
        section->AddChild(std::move(openRow));

        auto footer = row(8.0f);
        footer->Overrides().Set(Style::JustifyContent, JustifyContent::FlexEnd).Set(Style::PaddingTop, StyleLength::Px(6.0f));
        auto dismiss = std::make_unique<Button>();
        dismiss->SetText("Dismiss");
        dismiss->AddClass("primary");
        dismiss->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Dismiss(); });
        footer->AddChild(std::move(dismiss));
        section->AddChild(std::move(footer));

        content->AddChild(std::move(section));
    }

    // ---- Error section --------------------------------------------------
    {
        auto section = column(12.0f);
        m_ErrorSection = section.get();
        section->Overrides().Set(Style::Display, DisplayMode::None);

        auto err = std::make_unique<Label>();
        m_ErrorLabel = err.get();
        err->SetText("");
        StyleLabel(err.get(), kColWarn, 12.5f);
        section->AddChild(std::move(err));

        auto footer = row(8.0f);
        footer->Overrides().Set(Style::JustifyContent, JustifyContent::FlexEnd).Set(Style::PaddingTop, StyleLength::Px(6.0f));
        auto dismiss = std::make_unique<Button>();
        dismiss->SetText("Dismiss");
        dismiss->AddClass("secondary");
        dismiss->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Dismiss(); });
        footer->AddChild(std::move(dismiss));
        section->AddChild(std::move(footer));

        content->AddChild(std::move(section));
    }

    window->AddChild(std::move(content));

    backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (!m_Visible) return;
        if (e.Key == Input::kKeyCode_Escape && !m_Busy.load()) {
            e.Handled = true;
            OnCancel();
        }
    });

    backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void UnityImportModal::ShowSection(State state) {
    m_State = state;
    auto set = [](UIElement* e, bool visible) {
        if (e) e->Overrides().Set(Style::Display, visible ? DisplayMode::Flex : DisplayMode::None);
    };
    set(m_ConfigSection, state == State::Config);
    set(m_BusySection, state == State::Busy);
    set(m_CompleteSection, state == State::Complete);
    set(m_ErrorSection, state == State::Error);
}

void UnityImportModal::Show() {
    m_Visible = true;
    Overrides().Set(Style::Display, DisplayMode::Block).Set(Style::PointerEvents, false);

    m_BundlePath.clear();
    if (m_BundleField) m_BundleField->SetValue("");
    if (m_DestField) m_DestField->SetValue(m_DefaultDestination.string());
    if (m_SharedAssetsLabel) m_SharedAssetsLabel->SetText("");
    if (m_TexturesLabel) m_TexturesLabel->SetText("");
    if (m_SceneHeaderLabel) m_SceneHeaderLabel->SetText("Scenes in bundle");
    if (m_SceneListContainer) m_SceneListContainer->RemoveAllChildren();
    if (m_SceneListScroll) m_SceneListScroll->SetScrollY(0.0f);
    m_SceneRows.clear();
    m_RegistrationPending = false;
    if (m_ConfigWarningLabel) m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::None);

    const bool converterReady = !m_ConverterDll.empty() && std::filesystem::exists(m_ConverterDll);
    if (!converterReady && m_ConfigWarningLabel) {
        m_ConfigWarningLabel->SetText(
            "Converter assembly is missing from the unity-import package "
                "(Tools/UnityConverter.dll). Rebuild the Editor to restage its packages.");
        m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::Block);
    }

    ShowSection(State::Config);
}

void UnityImportModal::Hide() {
    m_Visible = false;
    Overrides().Set(Style::Display, DisplayMode::None).Set(Style::PointerEvents, false);
}

void UnityImportModal::OnCancel() {
    if (m_Busy.load()) return;
    Hide();
}

void UnityImportModal::Dismiss() {
    Hide();
}

void UnityImportModal::OnBrowseBundle() {
    const std::filesystem::path picked =
        Platform::SelectFile(m_BundlePath, "Unity Package", "*.unitypackage");
    if (picked.empty()) return;
    m_BundlePath = picked;
    if (m_BundleField) m_BundleField->SetValue(picked.string());
    StartListScenes(picked);
}

void UnityImportModal::OnBrowseDestination() {
    const std::filesystem::path picked = Platform::SelectFolder(DestinationDir());
    if (picked.empty()) return;
    if (m_DestField) m_DestField->SetValue(picked.string());
}

std::filesystem::path UnityImportModal::DestinationDir() const {
    if (m_DestField) {
        const std::string v = m_DestField->GetValue();
        if (!v.empty()) return std::filesystem::path(v);
    }
    return m_DefaultDestination;
}

void UnityImportModal::StartListScenes(const std::filesystem::path& bundle) {
    if (m_ConverterDll.empty() || !std::filesystem::exists(m_ConverterDll)) return;
    if (m_Busy.exchange(true)) return;
    if (m_Worker.joinable()) m_Worker.join();
    m_CancelRequested.store(false);

    if (m_SceneHeaderLabel) m_SceneHeaderLabel->SetText("Reading bundle…");
    if (m_SceneListContainer) m_SceneListContainer->RemoveAllChildren();
    m_SceneRows.clear();

    const std::filesystem::path dll = m_ConverterDll;
    m_Worker = std::thread([this, dll, bundle]() {
        std::string stdoutAll;
        ConverterRunResult res = RunConverterHosted(
            dll, {"--list-scenes", PathArgUtf8(bundle)},
            [&stdoutAll](const std::string& line) { stdoutAll += line; },
            m_CancelRequested);
        std::string tail = res.StderrTail;
        const bool ok = res.Spawned && res.ExitCode == 0;
        this->PostAction([this, ok, stdoutAll, tail]() {
            m_Busy.store(false);
            if (!ok) {
                ShowError("Could not read the bundle.\n\n" + tail);
                return;
            }
            PopulateSceneList(stdoutAll);
        });
    });
}

void UnityImportModal::PopulateSceneList(const std::string& inventoryJson) {
    json inv;
    try {
        inv = json::parse(inventoryJson);
    } catch (const std::exception&) {
        ShowError("The converter returned an unreadable scene inventory.");
        return;
    }

    if (m_SceneListContainer) m_SceneListContainer->RemoveAllChildren();
    if (m_SceneListScroll) m_SceneListScroll->SetScrollY(0.0f);
    m_SceneRows.clear();

    auto scenesIt = inv.find("scenes");
    if (scenesIt == inv.end() || !scenesIt->is_array() || scenesIt->empty()) {
        if (m_SceneHeaderLabel) m_SceneHeaderLabel->SetText("Scenes in bundle");
        if (m_ConfigWarningLabel) {
            m_ConfigWarningLabel->SetText("No .unity scenes were found in this bundle.");
            m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::Block);
        }
        return;
    }
    if (m_ConfigWarningLabel) m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::None);

    for (const auto& s : *scenesIt) {
        SceneRow rowData;
        rowData.Name = s.value("name", std::string());
        rowData.Path = s.value("path", std::string());
        rowData.Guid = s.value("guid", std::string());

        auto box = std::make_unique<Checkbox>();
        box->SetText(rowData.Name.empty() ? rowData.Path : (rowData.Name + "   " + rowData.Path));
        box->SetChecked(true);
        box->Overrides().Set(Style::Color, kColText).Set(Style::FontSize, StyleLength::Px(12.5f));
        box->SetOnValueChanged([this](const bool&) { UpdateSelectedCount(); });
        rowData.Box = box.get();
        if (m_SceneListContainer) m_SceneListContainer->AddChild(std::move(box));
        m_SceneRows.push_back(std::move(rowData));
    }

    UpdateSelectedCount();

    if (m_SharedAssetsLabel) {
        json counts = inv.value("assetCounts", json::object());
        const long long models = JsonInt(counts, "model");
        const long long textures = JsonInt(counts, "texture");
        const long long materials = JsonInt(counts, "material");
        m_SharedAssetsLabel->SetText(
            "Shared assets imported once for any subset: " + std::to_string(models) + " models · " +
            std::to_string(textures) + " textures · " + std::to_string(materials) + " materials");
    }
}

void UnityImportModal::UpdateSelectedCount() {
    int selected = 0;
    for (const auto& r : m_SceneRows)
        if (r.Box && r.Box->IsChecked()) ++selected;
    if (m_SceneHeaderLabel) {
        m_SceneHeaderLabel->SetText("Scenes in bundle  (" + std::to_string(selected) + " of " +
                                    std::to_string(m_SceneRows.size()) + " selected)");
    }
}

void UnityImportModal::OnImport() {
    if (m_Busy.load()) return;
    if (m_ConverterDll.empty() || !std::filesystem::exists(m_ConverterDll)) {
        if (m_ConfigWarningLabel) {
            m_ConfigWarningLabel->SetText(
                "Converter assembly is missing from the unity-import package "
                "(Tools/UnityConverter.dll). Rebuild the Editor to restage its packages.");
            m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::Block);
        }
        return;
    }
    if (m_BundlePath.empty()) {
        if (m_ConfigWarningLabel) {
            m_ConfigWarningLabel->SetText("Pick a Unity package or bundle folder first.");
            m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::Block);
        }
        return;
    }

    std::vector<std::string> sceneArgs;
    for (const auto& r : m_SceneRows) {
        if (r.Box && r.Box->IsChecked() && !r.Path.empty()) {
            sceneArgs.push_back("--scene");
            sceneArgs.push_back(r.Path);
        }
    }
    if (sceneArgs.empty()) {
        if (m_ConfigWarningLabel) {
            m_ConfigWarningLabel->SetText("Select at least one scene to import.");
            m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::Block);
        }
        return;
    }
    if (m_ConfigWarningLabel) m_ConfigWarningLabel->Overrides().Set(Style::Display, DisplayMode::None);

    StartConversion(sceneArgs);
}

void UnityImportModal::StartConversion(const std::vector<std::string>& sceneArgs) {
    // tmp stage under the project cache (excluded from the FileWatcher's scope).
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long long stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    m_StageDir = m_CacheRoot / "UnityImport" / ("import-" + std::to_string(stamp));
    m_StageAssetsDir = m_StageDir / "assets";
    std::error_code ec;
    std::filesystem::create_directories(m_StageDir, ec);
    if (ec) {
        ShowError("Could not create the staging folder:\n" + m_StageDir.string());
        return;
    }

    std::vector<std::string> args;
    args.push_back("--pkg");
    args.push_back(PathArgUtf8(m_BundlePath));
    for (const auto& a : sceneArgs) args.push_back(a);
    args.push_back("--project");
    args.push_back(PathArgUtf8(m_StageDir));
    if (!m_AssetDbPath.empty() && std::filesystem::exists(m_AssetDbPath)) {
        args.push_back("--assetdb");
        args.push_back(PathArgUtf8(m_AssetDbPath));
    }
    // Source textures stage as-is (--png); the engine's native import cooks
    // them (BC7/BC5 + mips, derived cache) once they land in Assets. The
    // pre-compile TextureCompiler path is CLI-only (--texc/GE_TEXC).
    args.push_back("--png");
    args.push_back("--json");

    if (m_TexturesLabel)
        m_TexturesLabel->SetText("Textures: staged as source images — the engine compiles them on import");

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ProgressPhase.clear();
        m_ProgressDetail.clear();
        m_ProgressStep = 0;
        m_ProgressTotal = -1;
        m_TextureCount = 0;
        m_ModelCount = 0;
        m_Outputs.clear();
        m_Scenes.clear();
        m_ConversionOk = false;
        m_ConversionSpawned = false;
        m_ConversionExit = -1;
        m_StderrTail.clear();
        m_MatGenerated = m_TexEncoded = m_TexCopied = m_TexUnresolved = 0;
        m_ModelsSeeded = 0;
    }
    if (m_ProgressFill) m_ProgressFill->Overrides().Set(Style::Width, StyleLength::Percent(0.0f));
    if (m_PhaseLabel) m_PhaseLabel->SetText("Starting conversion…");
    if (m_TextureSubLabel) m_TextureSubLabel->Overrides().Set(Style::Display, DisplayMode::None);
    ShowSection(State::Busy);

    if (m_Busy.exchange(true)) return;
    if (m_Worker.joinable()) m_Worker.join();
    m_CancelRequested.store(false);

    const std::filesystem::path dll = m_ConverterDll;
    m_Worker = std::thread([this, dll, args]() {
        ConverterRunResult res = RunConverterHosted(dll, args, [this](const std::string& line) {
            if (line.empty()) return;
            json obj;
            try {
                obj = json::parse(line);
            } catch (const std::exception&) {
                return; // ignore any stray non-JSON line
            }
            const std::string phase = obj.value("phase", std::string());
            if (phase == "summary") {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_ConversionOk = obj.value("ok", false);
                m_Outputs.clear();
                if (auto it = obj.find("outputs"); it != obj.end() && it->is_array()) {
                    for (const auto& o : *it)
                        m_Outputs.push_back({o.value("path", std::string()), o.value("kind", std::string())});
                }
                m_Scenes.clear();
                if (auto it = obj.find("scenes"); it != obj.end() && it->is_array()) {
                    for (const auto& s : *it) {
                        UnityImportSceneResult sr;
                        sr.Name = s.value("scene", std::string());
                        sr.Output = s.value("output", std::string());
                        sr.Entities = JsonInt(s, "entities");
                        sr.ResolvedMeshes = JsonInt(s, "resolvedMeshes");
                        sr.UnresolvedMeshes = JsonInt(s, "unresolvedMeshes");
                        sr.SeededMeshes = JsonInt(s, "seededMeshes");
                        if (auto dit = s.find("dropped"); dit != s.end() && dit->is_array()) {
                            for (const auto& d : *dit) {
                                const std::string kind = d.value("kind", std::string());
                                const std::string detail = d.value("detail", std::string());
                                sr.Dropped.push_back(detail.empty() ? kind : (kind + ": " + detail));
                            }
                        }
                        m_Scenes.push_back(std::move(sr));
                    }
                }
                if (auto it = obj.find("materials"); it != obj.end() && it->is_object()) {
                    m_MatGenerated = JsonInt(*it, "generated");
                    m_TexEncoded = JsonInt(*it, "texEncoded");
                    m_TexCopied = JsonInt(*it, "texCopied");
                    m_TexUnresolved = JsonInt(*it, "texUnresolved");
                }
                if (auto it = obj.find("models"); it != obj.end() && it->is_object())
                    m_ModelsSeeded = JsonInt(*it, "seeded");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_ProgressPhase = phase;
                m_ProgressDetail = obj.value("detail", std::string());
                m_ProgressStep = JsonInt(obj, "step");
                auto tit = obj.find("total");
                m_ProgressTotal = (tit != obj.end() && tit->is_number()) ? tit->get<long long>() : -1;
                // Unbounded item streams count per phase (converter counters
                // are per-phase too): textures staged vs pack models extracted.
                if (m_ProgressTotal < 0) {
                    if (phase == "models") m_ModelCount = m_ProgressStep;
                    else m_TextureCount = m_ProgressStep;
                }
            }
            this->PostAction([this]() { ApplyProgressToUI(); });
        }, m_CancelRequested);

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_ConversionSpawned = res.Spawned;
            m_ConversionExit = res.ExitCode;
            m_StderrTail = res.StderrTail;
        }
        this->PostAction([this]() { OnConversionFinished(); });
    });
}

void UnityImportModal::ApplyProgressToUI() {
    std::string phase, detail;
    long long step = 0, total = -1, texCount = 0, modelCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        phase = m_ProgressPhase;
        detail = m_ProgressDetail;
        step = m_ProgressStep;
        total = m_ProgressTotal;
        texCount = m_TextureCount;
        modelCount = m_ModelCount;
    }

    if (total > 0) {
        const float pct = std::clamp(static_cast<float>(step) / static_cast<float>(total) * 100.0f, 0.0f, 100.0f);
        if (m_ProgressFill) m_ProgressFill->Overrides().Set(Style::Width, StyleLength::Percent(pct));
        if (m_PhaseLabel) {
            std::string text = phase;
            if (!detail.empty()) text += " · " + detail;
            text += "   " + std::to_string(step) + " / " + std::to_string(total);
            m_PhaseLabel->SetText(text);
        }
    }

    if (total < 0 && m_TextureSubLabel) {
        std::string text = "textures: " + std::to_string(texCount) + " staged";
        if (modelCount > 0)
            text += " · models: " + std::to_string(modelCount) + " extracted from pack";
        m_TextureSubLabel->SetText(text + "…");
        m_TextureSubLabel->Overrides().Set(Style::Display, DisplayMode::Block);
    }
}

void UnityImportModal::OnConversionFinished() {
    bool spawned, ok;
    int exitCode;
    std::string tail;
    bool haveSummary;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        spawned = m_ConversionSpawned;
        exitCode = m_ConversionExit;
        ok = m_ConversionOk;
        tail = m_StderrTail;
        haveSummary = !m_Scenes.empty() || ok;
    }
    if (!spawned) {
        m_Busy.store(false);
        CleanupStage();
        ShowError("Could not start the hosted converter.\n\n" + tail);
        return;
    }
    if (exitCode != 0 || !ok || !haveSummary) {
        m_Busy.store(false);
        CleanupStage();
        ShowError("Conversion failed (exit code " + std::to_string(exitCode) + ").\n\n" + tail);
        return;
    }
    // m_Busy stays true through the finalize chain (scan/move/cleanup/register
    // on the worker); the terminal PostActions clear it.
    CollisionCheckAndMove();
}

// Purely lexical relativization: the converter emits absolute paths under
// <stage>/assets, and we want the same layout under the destination. Avoid
// std::filesystem::relative here — its weakly_canonical step can rewrite case
// or resolve symlinks on the live filesystem and break the prefix match.
std::filesystem::path UnityImportModal::RemapStagePath(const std::string& absPath,
                                                       const std::filesystem::path& stageAssets,
                                                       const std::filesystem::path& dest) {
    const std::filesystem::path p(absPath);
    std::filesystem::path rel = p.lexically_relative(stageAssets);
    if (rel.empty() || rel.begin() == rel.end() || *rel.begin() == "..")
        rel = p.filename();
    return dest / rel;
}

void UnityImportModal::CollisionCheckAndMove() {
    // UI thread: snapshot what the worker chain needs (DestinationDir reads a
    // TextField; the stage members are stable while Busy), then hand every
    // filesystem phase to the worker. These phases are unbounded IO — run on
    // the UI thread they starve the message pump for the import's whole tail
    // (Windows AppHangTransient class).
    const std::filesystem::path dest = DestinationDir();
    const std::filesystem::path stageAssets = m_StageAssetsDir;
    std::vector<UnityImportOutput> outputs;
    std::vector<UnityImportSceneResult> scenes;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        outputs = m_Outputs;
        scenes = m_Scenes;
    }

    if (m_PhaseLabel) m_PhaseLabel->SetText("Checking destination for existing files…");

    if (m_Worker.joinable()) m_Worker.join();
    m_Worker = std::thread([this, dest, stageAssets, outputs = std::move(outputs),
                            scenes = std::move(scenes)]() mutable {
        // Overwrite-collision check against the destination (worker thread).
        const auto tScan0 = std::chrono::steady_clock::now();
        std::vector<std::filesystem::path> collisions;
        for (const auto& o : outputs) {
            if (o.Path.empty()) continue;
            std::filesystem::path finalPath = RemapStagePath(o.Path, stageAssets, dest);
            if (std::filesystem::exists(LongPathSafe(finalPath)))
                collisions.push_back(std::move(finalPath));
        }
        Logger::Log::Info("UnityImport: collision scan {} output(s) in {:.0f}ms (worker thread)",
                          outputs.size(),
                          std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tScan0)
                              .count());
        this->PostAction([this, dest, stageAssets, outputs = std::move(outputs),
                          scenes = std::move(scenes), collisions = std::move(collisions)]() mutable {
            OnCollisionScanDone(std::move(dest), std::move(stageAssets), std::move(outputs),
                                std::move(scenes), std::move(collisions));
        });
    });
}

void UnityImportModal::OnCollisionScanDone(std::filesystem::path dest,
                                           std::filesystem::path stageAssets,
                                           std::vector<UnityImportOutput> outputs,
                                           std::vector<UnityImportSceneResult> scenes,
                                           std::vector<std::filesystem::path> collisions) {
    // UI thread. The overwrite prompt is a native modal — it blocks this
    // action but pumps its own message loop, so the window stays responsive.
    if (!collisions.empty()) {
        std::string msg = std::to_string(collisions.size()) +
                          " file(s) already exist at the destination and will be replaced:\n\n";
        const size_t shown = std::min<size_t>(collisions.size(), 12);
        for (size_t i = 0; i < shown; ++i)
            msg += "  " + collisions[i].filename().string() + "\n";
        if (collisions.size() > shown)
            msg += "  … and " + std::to_string(collisions.size() - shown) + " more\n";

        const int choice = Platform::ShowNativeChoiceDialog(
            "Overwrite existing files?", msg, {"Overwrite", "Cancel"}, 1);
        if (choice != 0) {
            // Discard the staged conversion off-thread too — the stage can be
            // ~1 GB of extracted package.
            if (m_Worker.joinable()) m_Worker.join();
            m_Worker = std::thread([this]() {
                CleanupStage();
                this->PostAction([this]() {
                    m_Busy.store(false);
                    ShowSection(State::Config);
                });
            });
            return;
        }
    }

    if (m_PhaseLabel) m_PhaseLabel->SetText("Moving files into the project…");

    if (m_Worker.joinable()) m_Worker.join();
    m_Worker = std::thread([this, dest = std::move(dest), stageAssets = std::move(stageAssets),
                            outputs = std::move(outputs), scenes = std::move(scenes)]() mutable {
        FinalizeMoveAndRegister(std::move(dest), std::move(stageAssets), std::move(outputs),
                                std::move(scenes));
    });
}

void UnityImportModal::FinalizeMoveAndRegister(std::filesystem::path dest,
                                               std::filesystem::path stageAssets,
                                               std::vector<UnityImportOutput> outputs,
                                               std::vector<UnityImportSceneResult> scenes) {
    // WORKER THREAD.
    //
    // One move pass: stage -> destination, preserving the relative layout so
    // the scenes' relative material/texture references still resolve. All
    // filesystem calls go through LongPathSafe (deep stage paths cross
    // MAX_PATH). Rename failures get bounded retry sweeps with a short
    // backoff — transient share locks from AV/indexer scans of the freshly
    // written stage, which the subprocess converter amortized behind its
    // process teardown — then a copy fallback. Anything that still won't move
    // is reported loudly: a dropped output is a broken scene reference at
    // open, not a cosmetic loss.
    const auto tMove0 = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> movedFiles;
    movedFiles.reserve(outputs.size());
    struct PendingMove {
        const UnityImportOutput* Output;
        std::filesystem::path FinalPath;
    };
    std::vector<PendingMove> pending;
    for (const auto& o : outputs) {
        if (o.Path.empty()) continue;
        std::filesystem::path finalPath = RemapStagePath(o.Path, stageAssets, dest);
        std::error_code ec;
        std::filesystem::create_directories(LongPathSafe(finalPath.parent_path()), ec);
        std::filesystem::remove(LongPathSafe(finalPath), ec);
        ec.clear();
        std::filesystem::rename(LongPathSafe(o.Path), LongPathSafe(finalPath), ec);
        if (ec) pending.push_back({&o, std::move(finalPath)});
        else movedFiles.push_back(std::move(finalPath));
    }
    std::error_code lastMoveError;
    for (int sweep = 0; sweep < 2 && !pending.empty(); ++sweep) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sweep == 0 ? 150 : 400));
        std::vector<PendingMove> still;
        for (auto& p : pending) {
            std::error_code ec;
            std::filesystem::rename(LongPathSafe(p.Output->Path), LongPathSafe(p.FinalPath), ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy_file(LongPathSafe(p.Output->Path), LongPathSafe(p.FinalPath),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::error_code rmEc;
                    std::filesystem::remove(LongPathSafe(p.Output->Path), rmEc);
                }
            }
            if (ec) {
                lastMoveError = ec;
                still.push_back(std::move(p));
            } else {
                movedFiles.push_back(p.FinalPath);
            }
        }
        pending.swap(still);
    }
    const size_t unmovedOutputs = pending.size();
    for (const auto& p : pending)
        Logger::Log::Warning("UnityImport: could not move '{}' into the project after retries ({})",
                             p.Output->Path, lastMoveError.message());

    // Remap scene output paths for the "Open Scene" buttons.
    for (auto& s : scenes)
        if (!s.Output.empty()) s.RemappedOutput = RemapStagePath(s.Output, stageAssets, dest).string();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Scenes = scenes;
    }

    const auto tMove1 = std::chrono::steady_clock::now();
    CleanupStage();
    const auto tClean1 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    Logger::Log::Info("UnityImport: moved {} output file(s) into {}", movedFiles.size(), dest.string());
    Logger::Log::Info("UnityImport: finalize timings (worker thread) — movePass={:.0f}ms "
                      "cleanupStage={:.0f}ms ({} outputs, {} unmoved)",
                      ms(tMove0, tMove1), ms(tMove1, tClean1), outputs.size(), unmovedOutputs);

    // Import-quiescence gate: register the moved files with the AssetRegistry
    // still on this worker thread and hold the "Open scene" buttons back until
    // it completes, so the scenes' path-form references (seeded Models_Unity
    // FBX, staged source textures) resolve deterministically at open instead
    // of racing the FileWatcher debounce. Honest scope: this waits for
    // REGISTRATION (path -> GUID identity), not for the async cooks (BC7
    // texture compile, ufbx model import) — those stream in after open by
    // design. ShowComplete lands first (with the "Registering…" note) so the
    // summary is visible while registration runs.
    const bool doRegister =
        m_OnAssetsChanged && !movedFiles.empty() && !m_CancelRequested.load();
    this->PostAction([this, unmovedOutputs, doRegister]() {
        m_RegistrationPending = doRegister;
        ShowComplete(unmovedOutputs);
    });

    if (doRegister) {
        // Registration stats files after the move; a TOCTOU vanish or disk
        // error throws, and an uncaught exception on a raw thread is
        // std::terminate. The gate must always clear.
        const auto tReg0 = std::chrono::steady_clock::now();
        try {
            m_OnAssetsChanged(movedFiles);
            Logger::Log::Info(
                "UnityImport: registered {} file(s) on worker thread in {:.0f}ms",
                movedFiles.size(),
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tReg0)
                    .count());
        } catch (const std::exception& e) {
            Logger::Log::Warning("[UnityImport] asset registration failed: {}", e.what());
        } catch (...) {
            Logger::Log::Warning("[UnityImport] asset registration failed (unknown exception)");
        }
    } else if (m_OnAssetsChanged && !movedFiles.empty()) {
        Logger::Log::Info("UnityImport: registration skipped (shutdown requested); "
                          "the moved files resolve on the next scan");
    }

    this->PostAction([this]() {
        m_RegistrationPending = false;
        m_Busy.store(false);
        if (m_State == State::Complete)
            RefreshOpenSceneRow();
    });
}

void UnityImportModal::ShowComplete(size_t unmovedOutputs) {
    std::vector<UnityImportSceneResult> scenes;
    long long matGenerated, texEncoded, texCopied, texUnresolved, modelsSeeded;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        scenes = m_Scenes;
        matGenerated = m_MatGenerated;
        texEncoded = m_TexEncoded;
        texCopied = m_TexCopied;
        texUnresolved = m_TexUnresolved;
        modelsSeeded = m_ModelsSeeded;
    }

    long long entities = 0, unresolved = 0, seeded = 0;
    std::set<std::string> dropped;
    for (const auto& s : scenes) {
        entities += s.Entities;
        unresolved += s.UnresolvedMeshes;
        seeded += s.SeededMeshes;
        for (const auto& d : s.Dropped) dropped.insert(d);
    }

    std::string text = "Import complete — " + std::to_string(scenes.size()) + " scene(s) · " +
                       std::to_string(entities) + " entities · " + std::to_string(matGenerated) + " materials";
    if (modelsSeeded > 0) {
        text += "\nModels: " + std::to_string(modelsSeeded) + " extracted from the pack (" +
                std::to_string(seeded) + " mesh reference(s) seeded)";
    }
    if (texEncoded || texCopied || texUnresolved) {
        text += "\nTextures: " + std::to_string(texEncoded + texCopied) +
                " staged for engine import";
        if (texUnresolved) text += ", " + std::to_string(texUnresolved) + " unresolved";
    }
    if (unresolved > 0)
        text += "\n" + std::to_string(unresolved) + " mesh reference(s) did not resolve against the project.";
    if (unmovedOutputs > 0) {
        text += "\n" + std::to_string(unmovedOutputs) +
                " output file(s) could not be moved into the project — see the log; re-import to repair.";
    }
    if (!dropped.empty()) {
        text += "\nDidn't carry over: ";
        int n = 0;
        for (const auto& d : dropped) {
            if (n++) text += ", ";
            if (n > 6) { text += "…"; break; }
            text += d;
        }
    }
    if (m_SummaryLabel) m_SummaryLabel->SetText(text);

    RefreshOpenSceneRow();
    ShowSection(State::Complete);
}

void UnityImportModal::RefreshOpenSceneRow() {
    if (!m_OpenSceneRow) return;
    m_OpenSceneRow->RemoveAllChildren();

    if (m_RegistrationPending) {
        auto note = std::make_unique<Label>();
        note->SetText("Registering imported assets with the project…");
        StyleLabel(note.get(), kColMuted, 12.5f);
        m_OpenSceneRow->AddChild(std::move(note));
        return;
    }

    std::vector<UnityImportSceneResult> scenes;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        scenes = m_Scenes;
    }
    for (const auto& s : scenes) {
        if (s.RemappedOutput.empty()) continue;
        auto btn = std::make_unique<Button>();
        btn->SetText("Open " + (s.Name.empty() ? std::string("Scene") : s.Name));
        btn->AddClass("secondary");
        const std::filesystem::path scenePath = s.RemappedOutput;
        btn->RegisterEventHandler(kEventButtonClick, [this, scenePath](UIEvent&) {
            if (m_OnOpenScene) m_OnOpenScene(scenePath);
            Hide();
        });
        m_OpenSceneRow->AddChild(std::move(btn));
    }
}

void UnityImportModal::ShowError(const std::string& message) {
    if (m_ErrorLabel) m_ErrorLabel->SetText(message);
    ShowSection(State::Error);
}

void UnityImportModal::CleanupStage() {
    if (m_StageDir.empty()) return;
    std::error_code ec;
    // LongPathSafe root so the recursive delete reaches children past
    // MAX_PATH (they inherit the \\?\ prefix during iteration).
    std::filesystem::remove_all(LongPathSafe(m_StageDir), ec);
    m_StageDir.clear();
    m_StageAssetsDir.clear();
}

} // namespace GameEngine
