#include "Panels/ProjectFolderPickerModal.h"
#include "Platform/Capabilities.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/TextField.h"
#include "UI/PanelSearchBar.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"
#include "Platform/Shell.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/TreeRemoval.h"
#include "Logger/Logger.h"
#include "Core/Application.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/EditorPaths.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "Core/Engine.h"
#include "Input/KeyCodes.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace GameEngine {

namespace {
constexpr float kLogoHueDegreesPerSecond = 360.0f / 12.0f;
// The library window fits the four-column recents grid with even 24px side
// margins; create/import opens as a narrower stacked modal sized to the
// three-column template grid, matching the reference prototype's layered
// modals.
// Stacked modal: three-column grid (868) + 16px side margins inside the
// scroll viewport - enough for card outlines/shadows AND the overlay
// scrollbar to sit clear of the last column - plus the content padding.
constexpr float kProjectPickerStackedModalWidthPx = 940.0f;
constexpr float kProjectPickerStackedModalHeightPx = 700.0f;
constexpr float kProjectPickerModalHeightPx = 768.0f;
constexpr float kProjectPickerModalHeaderHorizontalPaddingPx = 24.0f;
constexpr float kProjectPickerCubeLogoSizePx = 128.0f;
constexpr float kProjectPickerCubeLogoHalfPx = kProjectPickerCubeLogoSizePx * 0.5f;
constexpr float kProjectPickerCardWidthPx = 280.0f;
constexpr float kProjectPickerCardGapPx = 14.0f;
constexpr float kProjectPickerGridWidthPx = kProjectPickerCardWidthPx * 3.0f + kProjectPickerCardGapPx * 2.0f;
constexpr float kProjectPickerLibraryGridWidthPx =
    kProjectPickerCardWidthPx * 4.0f + kProjectPickerCardGapPx * 3.0f;
// Width budget for the library window. The card grid is a fixed-width,
// left-aligned block; the window is sized so the grid, the gap, the scrollbar
// lane and the paddings all fit without the viewport ever having to clip a card
// column. The vertical scrollbar takes real layout width from the viewport (it
// is not an overlay), so its thickness must be part of the budget whether or not
// it is currently shown:
//
//   window = (4 x card + 3 x gap)          <- kProjectPickerLibraryGridWidthPx
//          + content padding left/right
//          + (scrollbar gap + scrollbar thickness + scrollbar edge gap)
//          + 2 x window outline padding
//
// These stay in C++ rather than the stylesheet because every one of them also
// feeds a computed value the stylesheet cannot express: the window's centering
// margin is -width/2, the full-bleed header row cancels the content padding with
// a negative margin, and the scrollbar lane is applied as margins on the bar
// itself, whose thickness comes from Scrollbar::kDefaultThicknessPx.
constexpr float kProjectPickerContentPaddingLeftPx = 20.0f;
constexpr float kProjectPickerContentPaddingRightPx = 0.0f;
constexpr float kProjectPickerScrollbarGapPx = 10.0f;
constexpr float kProjectPickerScrollbarEdgeGapPx = 8.0f;
constexpr float kProjectPickerScrollbarLanePx =
    kProjectPickerScrollbarGapPx + Scrollbar::kDefaultThicknessPx + kProjectPickerScrollbarEdgeGapPx;
// Children fill the window's border box, so the window keeps one pixel of
// padding per side to stop its header/footer bands painting over the outline.
constexpr float kProjectPickerWindowOutlinePaddingPx = 1.0f;
constexpr float kProjectPickerLibraryWidthPx =
    kProjectPickerLibraryGridWidthPx + kProjectPickerContentPaddingLeftPx +
    kProjectPickerScrollbarLanePx + kProjectPickerContentPaddingRightPx +
    2.0f * kProjectPickerWindowOutlinePaddingPx;
/// Slightly longer than the ProjectPicker.css opacity transition so WKWebView stays off-screen until display:none.
constexpr float kNativeWebViewSuppressionAfterHideSeconds = 0.11f;

// Hit-testing compares z-index flat across stacking contexts, so any
// interactive element inside the modal window that sets its own z-index must
// sit ABOVE the window's value — a small local value (e.g. the 10/20 the
// panel-search CSS uses) loses the hit test to every plain container in the
// modal and becomes unclickable.
constexpr int kProjectPickerModalWindowZIndex = 10001;
constexpr int kProjectPickerSearchClearZIndex = kProjectPickerModalWindowZIndex + 2;
// Above the stacked scrim too, so the create/import grid's bar stays grabbable
// there as well: `.scrollbar { z-index: 50 }` from the theme would otherwise
// lose the flat hit test to every plain container in the modal.
constexpr int kProjectPickerScrollbarZIndex = kProjectPickerModalWindowZIndex + 4;

// Lane between the card grid and the window edge: the bar is a layout sibling
// of the clip viewport, so both gaps have to be margins on the bar itself
// (the theme's `.scrollbar.vertical` margins are overridden here).
void ApplyScrollbarLane(ScrollView& scrollView)
{
    UIElement* verticalBar = scrollView.GetVerticalScrollbar();
    if (!verticalBar)
        return;
    verticalBar->Overrides()
        .Set(Style::ZIndex, kProjectPickerScrollbarZIndex)
        .Set(Style::MarginLeft, StyleLength::Px(kProjectPickerScrollbarGapPx))
        .Set(Style::MarginRight, StyleLength::Px(kProjectPickerScrollbarEdgeGapPx));
}

// --ui_color_accent / the neutral field border from the editor theme. The modal
// styles inline (overrides beat CSS), so the search bar's focus outline has to
// be driven in code to match .panel-search-bar's accent focus elsewhere.
constexpr uint32_t kProjectPickerAccentColor = 0xFF3A8FFFu;

constexpr float kProjectPickerWindowDragThresholdPx = 4.0f;

// Turns `handle` into a title bar for `window`: press-drag moves the window in
// its parent, clamped so no edge can leave the parent. The window's percentage
// anchor and centering margins only describe where it opens; the first drag
// switches it to plain pixel offsets.
void EnableWindowDrag(UIElement& handle, UIElement& window, bool directTargetOnly = false)
{
    struct DragState
    {
        bool Pending = false;
        bool Active = false;
        float StartMouseX = 0.0f;
        float StartMouseY = 0.0f;
        float StartLeft = 0.0f;
        float StartTop = 0.0f;
    };

    auto state = std::make_shared<DragState>();
    UIElement* handlePtr = &handle;
    UIElement* windowPtr = &window;

    handle.AddClass("project-picker-drag-handle");

    handle.RegisterEventHandler(kEventMouseDown, [state, handlePtr, windowPtr, directTargetOnly](UIEvent& e) {
        UIElement* parent = windowPtr->GetParent();
        if (e.Button != 0 || !parent)
            return;
        // A row that also holds controls only drags from its own background:
        // capturing a press meant for a button would swallow that button's
        // mouse-up and with it the click.
        if (directTargetOnly && e.Target != handlePtr)
            return;
        e.Capture(handlePtr); // so the move/up stream keeps arriving outside the handle
        state->Pending = true;
        state->Active = false;
        state->StartMouseX = e.X;
        state->StartMouseY = e.Y;
        state->StartLeft = windowPtr->GetLayoutX() - parent->GetLayoutX();
        state->StartTop = windowPtr->GetLayoutY() - parent->GetLayoutY();
    });

    handle.RegisterEventHandler(kEventMouseMove, [state, windowPtr](UIEvent& e) {
        if (!state->Pending && !state->Active)
            return;
        UIElement* parent = windowPtr->GetParent();
        if (!parent)
            return;

        const float dx = e.X - state->StartMouseX;
        const float dy = e.Y - state->StartMouseY;
        if (!state->Active)
        {
            if (dx * dx + dy * dy <=
                kProjectPickerWindowDragThresholdPx * kProjectPickerWindowDragThresholdPx)
                return;
            state->Pending = false;
            state->Active = true;
        }

        const float maxLeft = std::max(0.0f, parent->GetLayoutWidth() - windowPtr->GetLayoutWidth());
        const float maxTop = std::max(0.0f, parent->GetLayoutHeight() - windowPtr->GetLayoutHeight());
        const float left = std::clamp(state->StartLeft + dx, 0.0f, maxLeft);
        const float top = std::clamp(state->StartTop + dy, 0.0f, maxTop);

        windowPtr->Overrides()
            .Set(Style::PositionLeft, StyleLength::Px(std::round(left)))
            .Set(Style::PositionTop, StyleLength::Px(std::round(top)))
            .Set(Style::MarginLeft, StyleLength::Px(0.0f))
            .Set(Style::MarginTop, StyleLength::Px(0.0f));
        windowPtr->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        parent->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        windowPtr->RequestRelayout();
    });

    handle.RegisterEventHandler(kEventMouseUp, [state](UIEvent& e) {
        if (e.Button != 0)
            return;
        state->Pending = false;
        state->Active = false;
    });
}

// The skeleton's ids are the contract between ProjectPicker.uxml and this file.
// A lookup that fails means the two drifted apart: say so and let the caller
// abandon the build rather than render half a modal.
UIElement* RequireLayoutElement(UIElement& root, const char* elementId, bool& layoutComplete)
{
    UIElement* element = root.FindById(elementId);
    if (!element)
    {
        Logger::Log::Error("ProjectPicker: ProjectPicker.uxml has no element with id '{}'", elementId);
        layoutComplete = false;
    }
    return element;
}

template <typename T>
T* RequireLayoutElementAs(UIElement& root, const char* elementId, bool& layoutComplete)
{
    UIElement* element = RequireLayoutElement(root, elementId, layoutComplete);
    if (!element)
        return nullptr;
    T* typed = dynamic_cast<T*>(element);
    if (!typed)
    {
        Logger::Log::Error("ProjectPicker: element '{}' in ProjectPicker.uxml has the wrong type", elementId);
        layoutComplete = false;
    }
    return typed;
}

enum class ModalButtonRole
{
    Primary,
    Secondary,
    FilledSecondary
};

// Role classes only: every visual (border, radius, font, cursor, idle and
// :hover colors) lives in .project-picker-button* in ProjectPicker.css.
static void ApplyModalButtonStyle(Button& button, ModalButtonRole role)
{
    button.AddClass("project-picker-button");
    if (role == ModalButtonRole::Primary)
        button.AddClass("project-picker-button-primary");
    else if (role == ModalButtonRole::FilledSecondary)
        button.AddClass("project-picker-button-filled-secondary");
    else
        button.AddClass("project-picker-button-secondary");
    button.AddClass(role == ModalButtonRole::Primary ? "primary" : "secondary");
}

// Shared compact height/padding come from .project-picker-button-compact; the
// per-site widths are .project-picker-button-w130 / -w140.
static void ApplyCompactModalButtonLayout(Button& button, const char* widthClass = nullptr)
{
    button.AddClass("project-picker-button-compact");
    if (widthClass)
        button.AddClass(widthClass);
}

// The Button is a stationary hover/hit target; this wrapper carries the
// card's visuals and is what the :hover lift moves (see .project-card-lift
// in ProjectPicker.css — all styling lives there). Moving the hover target itself
// would shift the hit area under the pointer and oscillate when the cursor
// rests on a card edge.
static UIElement& AddProjectCardLift(Button& card)
{
    auto lift = std::make_unique<UIElement>();
    UIElement* liftPtr = lift.get();
    lift->AddClass("project-card-lift");
    card.AddChild(std::move(lift));
    return *liftPtr;
}

static std::string TrimProjectText(std::string value)
{
    const auto isWhitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
        return !isWhitespace(static_cast<unsigned char>(c));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
        return !isWhitespace(static_cast<unsigned char>(c));
    }).base(), value.end());
    return value;
}

static std::string SanitizeProjectName(std::string value)
{
    value = TrimProjectText(std::move(value));
    for (char& c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != ' ')
            c = '_';
    }
    if (value.empty() || value == "." || value == "..")
        value = "Untitled Project";
    return value;
}

static std::string FormatLastOpenedDate(int64_t unixSeconds)
{
    const std::time_t timestamp = static_cast<std::time_t>(unixSeconds);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &timestamp);
#else
    localtime_r(&timestamp, &localTime);
#endif
    std::ostringstream out;
    out << std::put_time(&localTime, "%b %d, %Y");
    return out.str();
}

static int64_t CurrentUnixSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string FormatEditedTime(int64_t unixSeconds)
{
    if (unixSeconds <= 0)
        return "edited recently";

    const int64_t elapsed = std::max<int64_t>(0, CurrentUnixSeconds() - unixSeconds);
    if (elapsed < 60)
        return "edited just now";
    if (elapsed < 3600)
    {
        const int64_t minutes = elapsed / 60;
        return "edited " + std::to_string(minutes) + (minutes == 1 ? " minute ago" : " minutes ago");
    }
    if (elapsed < 86400)
    {
        const int64_t hours = elapsed / 3600;
        return "edited " + std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }
    if (elapsed < 604800)
    {
        const int64_t days = elapsed / 86400;
        return "edited " + std::to_string(days) + (days == 1 ? " day ago" : " days ago");
    }
    return "edited " + FormatLastOpenedDate(unixSeconds);
}

static uintmax_t CalculateProjectSizeBytes(const std::filesystem::path& projectRoot)
{
    // This runs from a UI callback. Where the platform cannot walk a tree from
    // that thread (see Platform::SupportsSynchronousDirectoryWalk) the card
    // shows no size rather than deadlock the frame loop.
    if (!Platform::SupportsSynchronousDirectoryWalk())
        return 0;
    const auto tSizeStart = std::chrono::steady_clock::now();
    uint32_t walkedEntries = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(projectRoot, ec))
        return 0;

    uintmax_t totalBytes = 0;
    std::filesystem::recursive_directory_iterator it(
        projectRoot, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        ++walkedEntries;
        std::error_code fileEc;
        if (it->is_regular_file(fileEc))
        {
            const uintmax_t fileBytes = it->file_size(fileEc);
            if (!fileEc)
                totalBytes += fileBytes;
        }
    }
    (void)walkedEntries;
    (void)tSizeStart;
    return totalBytes;
}

static std::string FormatProjectSize(uintmax_t bytes)
{
    constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits))
    {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    if (unit == 0)
        out << bytes << ' ' << kUnits[unit];
    else
        out << std::fixed << std::setprecision(value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2))
            << value << ' ' << kUnits[unit];
    return out.str();
}

constexpr uint32_t kCardPreviewColors[] = {
    0xFF182B3Bu, 0xFF24203Bu, 0xFF18372Eu, 0xFF3A202Eu, 0xFF202A42u};

// Catalog-entry ids name staging dirs and cache files; imports synthesize
// their entry, so derive a conforming slug from the user-facing name.
static std::string MakeCatalogSlug(const std::string& name)
{
    std::string slug;
    for (const char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            slug += c;
        else if (c >= 'A' && c <= 'Z')
            slug += static_cast<char>(c - 'A' + 'a');
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.size() > 64)
        slug.resize(64);
    return slug.empty() ? "import" : slug;
}

static bool IsHttpUrl(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

static std::string DeriveGitProjectName(const std::string& url)
{
    std::string name = url;
    while (!name.empty() && (name.back() == '/' || name.back() == '\\'))
        name.pop_back();
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    constexpr const char* kGitSuffix = ".git";
    if (name.size() > 4 && name.compare(name.size() - 4, 4, kGitSuffix) == 0)
        name.resize(name.size() - 4);
    return name;
}

static std::string FormatStars(int64_t stars)
{
    if (stars < 1000)
        return std::to_string(stars);
    std::ostringstream out;
    out << std::fixed << std::setprecision(stars % 1000 >= 100 ? 1 : 0)
        << static_cast<double>(stars) / 1000.0 << 'k';
    return out.str();
}

// Display name from the project's settings; the folder name is the fallback.
static std::string ReadProjectDisplayName(const std::filesystem::path& projectRoot)
{
    Editor::SettingsStore settings = Editor::OpenProjectSettings(projectRoot);
    std::string err;
    (void)settings.Load(&err);
    std::string displayName;
    if (settings.TryGetString("project.displayName", displayName) && !displayName.empty())
        return displayName;
    return projectRoot.filename().empty() ? projectRoot.string()
                                          : projectRoot.filename().string();
}

static std::string FindProjectSceneThumbnail(const std::filesystem::path& projectRoot)
{
    const std::filesystem::path thumbnailRoot = projectRoot / ".Editor" / "Thumbnails";
    std::error_code ec;
    if (!std::filesystem::is_directory(thumbnailRoot, ec))
        return {};

    // A user-assigned cover always wins over generated scene thumbnails.
    const std::filesystem::path cover = thumbnailRoot / "cover.png";
    if (std::filesystem::exists(cover, ec))
        return cover.generic_string();

    // Fixed-name card thumbnail written by SceneThumbnailCapture on every scene
    // load/save. A single exists() (no directory walk), so it is web-safe and is
    // the newest capture — preferred over walking for a per-scene scenethumb.
    const std::filesystem::path card = thumbnailRoot / "card.png";
    if (std::filesystem::exists(card, ec))
        return card.generic_string();

    // This runs from a UI callback (RefreshRecentProjectsList, including right
    // after a delete). Where the platform cannot walk a tree from that thread
    // the card falls back to the placeholder when there is no cover/card.png.
    if (!Platform::SupportsSynchronousDirectoryWalk())
        return {};
    std::filesystem::path newestPath;
    std::filesystem::file_time_type newestTime = std::filesystem::file_time_type::min();
    for (std::filesystem::directory_iterator it(thumbnailRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string fileName = it->path().filename().string();
        if (fileName.rfind("scenethumb_", 0) != 0 || it->path().extension() != ".png")
            continue;
        const auto modified = it->last_write_time(ec);
        if (!ec && (newestPath.empty() || modified > newestTime))
        {
            newestTime = modified;
            newestPath = it->path();
        }
    }
    return newestPath.empty() ? std::string{} : newestPath.generic_string();
}

} // namespace

ProjectFolderPickerModal::ProjectFolderPickerModal()
    : m_CatalogService(EngineCore::GetInstance().GetJobSystem()),
      m_Acquisition(EngineCore::GetInstance().GetJobSystem())
{
    RequestSubtreeStyleAssetPath("UI/controls/ProjectPicker.css", "editor");
    AddClass("project-picker-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    SetFocusable(true);

    // Geometry and the closed resting state come from .project-picker-modal;
    // Hide() installs the overrides that Show() later animates.
    Hide();
}

void ProjectFolderPickerModal::OnOwnerManagerChanged(UIManager* owner)
{
    if (!owner || m_TreeBuilt || m_BuildScheduled)
        return;

    // Off the startup path: the picker is hidden until the toolbar or a
    // project-less launch asks for it, so its tree is built on the first UI
    // tick after attach rather than while the editor is still coming up.
    m_BuildScheduled = true;
    PostSafeAction([this]() { EnsureTreeBuilt(); });
}

void ProjectFolderPickerModal::EnsureTreeBuilt()
{
    m_BuildScheduled = false;
    if (m_TreeBuilt)
        return;
    // The layout asset binds through the owning manager, so an unattached
    // picker waits for OnOwnerManagerChanged instead.
    if (!GetOwnerManager())
        return;
    m_TreeBuilt = true;
    BuildTree();
}

void ProjectFolderPickerModal::BuildTree()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        Logger::Log::Error("ProjectPicker: built without a UIManager; the picker will be empty.");
        return;
    }

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path layoutAssetPath =
        std::filesystem::path("UI") / "controls" / "ProjectPicker.uxml";
    const GUID layoutGuid = assetManager.ResolveAssetGuid(layoutAssetPath, kAssetSourceAliasEditor);
    if (layoutGuid.IsNull())
    {
        Logger::Log::Error("ProjectPicker: UI/controls/ProjectPicker.uxml is not in the asset registry.");
        return;
    }
    // Waits on purpose: the editor's own .uxml, small and never cooked, read once while the editor builds.
    auto layoutAsset = assetManager.LoadAssetAsync(layoutGuid, AssetLoadPriority::High).get();
    if (!layoutAsset || layoutAsset->GetType() != AssetType::UILayout ||
        !ui->BindLayoutToSubtreeChildrenFromAsset(
            this, *static_cast<UILayoutAsset*>(layoutAsset.get())))
    {
        Logger::Log::Error("ProjectPicker: failed to bind UI/controls/ProjectPicker.uxml.");
        return;
    }

    // The skeleton (backdrop, both windows, the hero and footer bands, the
    // library content column) comes from the layout asset; this function keeps
    // the computed styling, the wiring, and everything dynamic.
    bool layoutComplete = true;
    UIElement* windowPtr = RequireLayoutElement(*this, "modal-window", layoutComplete);
    UIElement* header = RequireLayoutElement(*this, "modal-header", layoutComplete);
    UIElement* engineRegion = RequireLayoutElement(*this, "header-engine-region", layoutComplete);
    UIElement* versionRegion = RequireLayoutElement(*this, "header-version-region", layoutComplete);
    UIElement* cubeIcon = RequireLayoutElement(*this, "cube-icon", layoutComplete);
    UIElement* content = RequireLayoutElement(*this, "modal-content", layoutComplete);
    UIElement* projectHeader = RequireLayoutElement(*this, "projects-header", layoutComplete);
    UIElement* title = RequireLayoutElement(*this, "modal-title", layoutComplete);
    ScrollView* scrollView =
        RequireLayoutElementAs<ScrollView>(*this, "recent-projects-scroll", layoutComplete);
    UIElement* bottomPanel = RequireLayoutElement(*this, "bottom-panel", layoutComplete);
    Button* browseButton = RequireLayoutElementAs<Button>(*this, "browse-button", layoutComplete);
    UIElement* projectLogo = RequireLayoutElement(*this, "project-logo", layoutComplete);
    UIElement* buttonContainer = RequireLayoutElement(*this, "button-container", layoutComplete);
    Button* openButton = RequireLayoutElementAs<Button>(*this, "open-button", layoutComplete);
    UIElement* loadingLabel = RequireLayoutElement(*this, "loading-label", layoutComplete);
    UIElement* footer = RequireLayoutElement(*this, "modal-footer", layoutComplete);
    Button* cancelButton = RequireLayoutElementAs<Button>(*this, "cancel-button", layoutComplete);
    UIElement* stackedScrim =
        RequireLayoutElement(*this, "project-picker-stacked-scrim", layoutComplete);
    UIElement* stackedWindowPtr =
        RequireLayoutElement(*this, "project-picker-stacked-window", layoutComplete);
    UIElement* stackedContentPtr =
        RequireLayoutElement(*this, "project-picker-stacked-content", layoutComplete);
    UIElement* stackedFooter =
        RequireLayoutElement(*this, "project-picker-stacked-footer", layoutComplete);
    // Styled entirely from ProjectPicker.css and never touched from here, but
    // still part of the id contract: a rename in the layout asset must be
    // reported rather than silently leaving the element unstyled.
    for (const char* styledOnlyId :
         {"modal-backdrop", "engine-label", "version-label", "cube-container",
          "projects-title-row", "projects-header-spacer", "path-container", "path-input"})
    {
        RequireLayoutElement(*this, styledOnlyId, layoutComplete);
    }
    if (!layoutComplete)
        return;

    // The window's fixed size and its centering margins are one arithmetic unit
    // (margin = -size/2), so both stay here rather than splitting across the
    // stylesheet. Everything else about the window is in ProjectPicker.css.
    windowPtr->Overrides()
        .Set(Style::MarginLeft, StyleLength::Px(-kProjectPickerLibraryWidthPx * 0.5f))
        .Set(Style::MarginTop, StyleLength::Px(-kProjectPickerModalHeightPx * 0.5f))
        .Set(Style::Width, StyleLength::Px(kProjectPickerLibraryWidthPx))
        .Set(Style::Height, StyleLength::Px(kProjectPickerModalHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kProjectPickerModalHeightPx))
        .Set(Style::MaxHeight, StyleLength::Px(kProjectPickerModalHeightPx));

    // The hero band doubles as the window's title bar.
    EnableWindowDrag(*header, *windowPtr);

    // Side bands: inner width from header padding to cube edge (symmetric).
    const float headerInnerWidthPx =
        kProjectPickerLibraryWidthPx - 2.0f * kProjectPickerModalHeaderHorizontalPaddingPx;
    const float headerInnerHalfPx = headerInnerWidthPx * 0.5f;
    const float sideTitleBandWidthPx = headerInnerHalfPx - kProjectPickerCubeLogoHalfPx;
    engineRegion->Overrides().Set(Style::Width, StyleLength::Px(sideTitleBandWidthPx));
    versionRegion->Overrides().Set(Style::Width, StyleLength::Px(sideTitleBandWidthPx));

    UI::Layout::SetBackgroundPath(*cubeIcon, "Icons/logo.svg");

    // The content padding is part of the window width budget above, and the
    // projects header cancels it with equal negative margins, so the pair stays
    // in C++ where that arithmetic lives.
    content->Overrides()
        .Set(Style::PaddingTop, StyleLength::Px(0.0f))
        .Set(Style::PaddingRight, StyleLength::Px(kProjectPickerContentPaddingRightPx))
        // No bottom padding: the scroll viewport reaches the footer's rule, so a
        // partially scrolled card row is cut by that line instead of ending in
        // mid-air above it.
        .Set(Style::PaddingBottom, StyleLength::Px(0.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(kProjectPickerContentPaddingLeftPx));

    m_ProjectsHeader = projectHeader;
    // Full-bleed band: negative margins cancel the content padding so the rule
    // runs edge to edge, while the row's own padding keeps the title on the
    // first card column and the search field's right edge on the last one (the
    // scrollbar lane sits inside the right padding).
    projectHeader->Overrides()
        .Set(Style::MarginLeft, StyleLength::Px(-kProjectPickerContentPaddingLeftPx))
        .Set(Style::MarginRight, StyleLength::Px(-kProjectPickerContentPaddingRightPx))
        .Set(Style::PaddingLeft, StyleLength::Px(kProjectPickerContentPaddingLeftPx))
        .Set(Style::PaddingRight, StyleLength::Px(kProjectPickerScrollbarLanePx));

    m_Title = title;

    auto onProjectSearchChanged = [this](const std::string& value) {
            m_SearchQuery = value;
            if (m_SearchPlaceholder)
            {
                m_SearchPlaceholder->Overrides().Set(
                    Style::Display, value.empty() ? DisplayMode::Flex : DisplayMode::None);
                m_SearchPlaceholder->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            }
            // RefreshRecentProjectsList() replaces every card in the grid. When this
            // callback fires from the search bar's own clear-button click, that
            // rebuild runs synchronously inside UIManager's mouse-up dispatch and
            // trips the tree-mutated-during-events guard, which skips the post-click
            // focus/visual bookkeeping for the gesture (the field's own SetValue("")
            // already ran, but nothing forces the frame that would show it). Defer
            // the rebuild the same way ListView/TreeView/Dropdown do for structural
            // changes triggered from inside an event handler.
            PostSafeAction([this]() { RefreshRecentProjectsList(); });
        };
    auto search = BuildPanelSearchBar(
        "project-picker-search-field",
        []() { return true; },
        onProjectSearchChanged,
        onProjectSearchChanged);
    search.Root->SetId("project-search");
    // Accent focus outline, as every other editor search bar gets from
    // .panel-search-bar's focus rules — those can't reach here because
    // #project-search's border out-specifies them.
    UIElement* searchRoot = search.RootPtr;
    search.FieldPtr->RegisterEventHandler(kEventFocusIn, [searchRoot](UIEvent&) {
        searchRoot->Overrides().Set(Style::BorderColor, BorderColorsTRBL{
            kProjectPickerAccentColor, kProjectPickerAccentColor,
            kProjectPickerAccentColor, kProjectPickerAccentColor});
        searchRoot->MarkDirty(UIElement::VisualDirty);
    });
    search.FieldPtr->RegisterEventHandler(kEventFocusOut, [searchRoot](UIEvent&) {
        // Drop the override so the idle border falls back to #project-search.
        searchRoot->Overrides().Reset(Style::BorderColor);
        searchRoot->MarkDirty(UIElement::VisualDirty);
    });
    search.FieldPtr->AddClass("project-picker-small-text");
    m_SearchField = search.FieldPtr;
    if (search.ClearButtonPtr)
    {
        Button* clearButton = search.ClearButtonPtr;
        // Keep the shared search-bar clear handler. Routing focus through the
        // field prevents mouse-up from focusing the clear button after it runs.
        clearButton->SetFocusProxy(m_SearchField);
        // Lift the clear button above the modal-window z context. The theme's
        // z-index of 20 both loses the flat hit test inside the modal and lets
        // the search bar's own background paint over the glyph. Kept in code:
        // the shared control owns the element, so nothing in this modal's
        // stylesheet reliably selects it.
        clearButton->Overrides().Set(Style::ZIndex, kProjectPickerSearchClearZIndex);
        clearButton->RegisterEventHandler(kEventMouseEnter, [clearButton](UIEvent&) {
            clearButton->Overrides().Set(Style::Opacity, 1.0f);
            clearButton->MarkDirty(UIElement::VisualDirty);
        });
        clearButton->RegisterEventHandler(kEventMouseLeave, [clearButton](UIEvent&) {
            clearButton->Overrides().Set(Style::Opacity, 0.7f);
            clearButton->MarkDirty(UIElement::VisualDirty);
        });
    }
    search.Root->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if ((e.Button == 0 || e.Button == 1) && m_SearchField)
        {
            if (UIManager* manager = GetOwnerManager())
                manager->FocusElement(m_SearchField);
        }
    });

    auto searchPlaceholder = std::make_unique<Label>();
    searchPlaceholder->SetText("Search projects");
    searchPlaceholder->AddClass("project-picker-small-text");
    searchPlaceholder->AddClass("project-picker-search-placeholder");
    m_SearchPlaceholder = searchPlaceholder.get();
    search.Root->AddChild(std::move(searchPlaceholder));

    // Appended after the title row and the spacer declared in the layout asset.
    projectHeader->AddChild(std::move(search.Root));

    // New-project view. This mirrors the reference flow: choose a template, enter a
    // project name and parent folder, then create the project from one clear action.
    auto newProjectView = std::make_unique<UIElement>();
    newProjectView->SetId("new-project-view");
    m_NewProjectView = newProjectView.get();

    auto newProjectTop = std::make_unique<UIElement>();
    UIElement* newProjectTopPtr = newProjectTop.get();
    newProjectTop->SetId("new-project-top");

    auto backButton = std::make_unique<Button>();
    backButton->SetId("new-project-back");
    backButton->SetText("‹");
    backButton->SetTooltip("Back to projects");
    ApplyModalButtonStyle(*backButton, ModalButtonRole::Secondary);
    backButton->AddClass("project-picker-back-button");
    backButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowLibraryView(); });

    auto newProjectTitle = std::make_unique<Label>();
    UIElement* newProjectTitlePtr = newProjectTitle.get();
    newProjectTitle->SetText("New project");
    newProjectTitle->AddClass("project-picker-page-title");
    newProjectTop->AddChild(std::move(backButton));
    newProjectTop->AddChild(std::move(newProjectTitle));

    auto newProjectTabs = std::make_unique<UIElement>();
    newProjectTabs->SetId("new-project-tabs");

    auto templatesTab = std::make_unique<Button>();
    m_TemplatesTabButton = templatesTab.get();
    templatesTab->SetId("project-templates-tab");
    templatesTab->AddClass("project-picker-new-tab");
    // Templates is the tab the create page opens on; UpdateNewProjectTabView
    // moves the class from here on. Colors and the accent underline come from
    // .project-picker-new-tab / .active / :hover in ProjectPicker.css.
    templatesTab->AddClass("active");
    templatesTab->SetText("Templates");
    templatesTab->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        m_ShowingCommunityProjects = false;
        UpdateNewProjectTabView();
    });

    auto communityTab = std::make_unique<Button>();
    m_CommunityTabButton = communityTab.get();
    communityTab->SetId("project-community-tab");
    communityTab->AddClass("project-picker-new-tab");
    communityTab->SetText("Community");
    communityTab->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        m_ShowingCommunityProjects = true;
        UpdateNewProjectTabView();
    });

    newProjectTabs->AddChild(std::move(templatesTab));
    newProjectTabs->AddChild(std::move(communityTab));
    newProjectTop->AddChild(std::move(newProjectTabs));
    newProjectView->AddChild(std::move(newProjectTop));

    // The template/community grids scroll once the catalog outgrows the
    // stacked modal (the recents grid gets the same treatment in the library).
    auto newProjectGridScroll = std::make_unique<ScrollView>();
    newProjectGridScroll->SetId("new-project-grid-scroll");
    ApplyScrollbarLane(*newProjectGridScroll);
    auto newProjectGridContent = std::make_unique<UIElement>();
    UIElement* newProjectGridContentPtr = newProjectGridContent.get();
    newProjectGridContent->AddClass("project-picker-grid-content");

    auto templateRow = std::make_unique<UIElement>();
    templateRow->SetId("project-templates");
    m_TemplateProjectsView = templateRow.get();
    // Grid width and gutter are derived from the card metrics, so they stay
    // beside those constants; flow and alignment are in #project-templates.
    templateRow->Overrides()
        .Set(Style::Gap, StyleLength::Px(kProjectPickerCardGapPx))
        .Set(Style::Width, StyleLength::Px(kProjectPickerGridWidthPx));

    newProjectGridContentPtr->AddChild(std::move(templateRow));

    std::string templateCatalogError;
    if (!Editor::ProjectCatalogService::LoadTemplateCatalog(m_TemplateCatalog, m_TemplateCatalogRoot,
                                                            &templateCatalogError))
    {
        Logger::Log::Warning("Project templates unavailable: {}", templateCatalogError);
    }
    BuildTemplateCards();

    auto communityView = std::make_unique<UIElement>();
    m_CommunityProjectsView = communityView.get();
    communityView->SetId("project-community");

    auto communityStatus = std::make_unique<UIElement>();
    m_CommunityStatusView = communityStatus.get();
    communityStatus->AddClass("project-community-status");

    auto communityIcon = std::make_unique<UIElement>();
    communityIcon->AddClass("project-community-icon");
    UI::Layout::SetBackgroundPath(*communityIcon, "Icons/cloud.png");

    auto communityTitle = std::make_unique<Label>();
    m_CommunityStatusTitle = communityTitle.get();
    communityTitle->SetText("Community projects");
    communityTitle->AddClass("project-community-title");

    auto communityMessage = std::make_unique<Label>();
    m_CommunityStatusMessage = communityMessage.get();
    communityMessage->SetText("");
    communityMessage->AddClass("project-community-message");

    auto communityRetry = std::make_unique<Button>();
    m_CommunityRetryButton = communityRetry.get();
    communityRetry->SetId("project-community-retry");
    communityRetry->SetText("Retry");
    communityRetry->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { m_CatalogService.RequestCommunityFetch(); });
    ApplyModalButtonStyle(*communityRetry, ModalButtonRole::Secondary);

    communityStatus->AddChild(std::move(communityIcon));
    communityStatus->AddChild(std::move(communityTitle));
    communityStatus->AddChild(std::move(communityMessage));
    communityStatus->AddChild(std::move(communityRetry));
    communityView->AddChild(std::move(communityStatus));

    // Offline banner: shown above the grid when the catalog is a cached copy
    // after a failed refresh, with an explicit way to retry the live source.
    auto offlineBanner = std::make_unique<UIElement>();
    m_CommunityOfflineBanner = offlineBanner.get();
    offlineBanner->SetId("project-community-offline");

    auto offlineLabel = std::make_unique<Label>();
    m_CommunityOfflineLabel = offlineLabel.get();
    offlineLabel->SetText("");
    offlineLabel->AddClass("project-community-offline-label");

    auto offlineRefresh = std::make_unique<Button>();
    offlineRefresh->SetId("project-community-refresh");
    offlineRefresh->SetText("Refresh");
    offlineRefresh->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { m_CatalogService.RequestCommunityFetch(); });
    ApplyModalButtonStyle(*offlineRefresh, ModalButtonRole::Secondary);

    offlineBanner->AddChild(std::move(offlineLabel));
    offlineBanner->AddChild(std::move(offlineRefresh));
    communityView->AddChild(std::move(offlineBanner));

    // Card grid: shown once a catalog is available. Wraps past three entries.
    auto communityGrid = std::make_unique<UIElement>();
    m_CommunityGrid = communityGrid.get();
    communityGrid->SetId("project-community-grid");
    communityGrid->Overrides()
        .Set(Style::Gap, StyleLength::Px(kProjectPickerCardGapPx))
        .Set(Style::Width, StyleLength::Px(kProjectPickerGridWidthPx));
    communityView->AddChild(std::move(communityGrid));

    newProjectGridContentPtr->AddChild(std::move(communityView));
    newProjectGridScroll->AddContent(std::move(newProjectGridContent));
    newProjectView->AddChild(std::move(newProjectGridScroll));

    // Form strip above the footer: the head of the bottom band, full-bleed to
    // the window edges (negative margins cancel the content padding) and
    // sharing the footer's background so name/location and the action buttons
    // read as one block. No top margin — the rule is the scroll viewport's
    // bottom edge, so a partially scrolled card row is cut by that line.
    auto newProjectForm = std::make_unique<UIElement>();
    UIElement* newProjectFormPtr = newProjectForm.get();
    newProjectForm->SetId("new-project-form");
    newProjectForm->AddClass("project-picker-full-bleed");

    auto addField = [](UIElement& form, const char* labelText, TextField*& fieldOut) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("project-picker-field-row");
        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("project-picker-field-label");
        auto field = std::make_unique<TextField>();
        fieldOut = field.get();
        field->AddClass("project-picker-text-field");
        field->AddClass("project-picker-text-field-fill");
        row->AddChild(std::move(label));
        row->AddChild(std::move(field));
        form.AddChild(std::move(row));
    };
    addField(*newProjectForm, "PROJECT NAME", m_NewProjectNameField);
    // Ids let the debug-server automation (set_text_field / click_element)
    // target the creation form.
    m_NewProjectNameField->SetId("new-project-name-field");

    auto locationRow = std::make_unique<UIElement>();
    locationRow->AddClass("project-picker-field-row");
    auto locationLabel = std::make_unique<Label>();
    locationLabel->SetText("LOCATION");
    locationLabel->AddClass("project-picker-field-label");
    auto locationControls = std::make_unique<UIElement>();
    locationControls->AddClass("project-picker-field-controls");
    auto locationField = std::make_unique<TextField>();
    m_NewProjectLocationField = locationField.get();
    locationField->SetId("new-project-location-field");
    locationField->AddClass("project-picker-text-field");
    locationField->AddClass("project-picker-text-field-grow");
    locationControls->AddChild(std::move(locationField));
    // The location field stays editable everywhere; the Browse affordance only
    // exists where a folder picker does (see SupportsFolderPicker).
    if (Platform::SupportsFolderPicker())
    {
        auto locationBrowse = std::make_unique<Button>();
        locationBrowse->SetText("Browse");
        locationBrowse->SetOnClick([this](UIEvent&) { OnNewProjectBrowseClicked(); });
        ApplyModalButtonStyle(*locationBrowse, ModalButtonRole::Primary);
        ApplyCompactModalButtonLayout(*locationBrowse, "project-picker-button-w140");
        locationBrowse->AddClass("project-picker-button-noshrink");
        locationControls->AddChild(std::move(locationBrowse));
    }
    locationRow->AddChild(std::move(locationLabel));
    locationRow->AddChild(std::move(locationControls));
    newProjectForm->AddChild(std::move(locationRow));
    newProjectView->AddChild(std::move(newProjectForm));

    auto newProjectError = std::make_unique<Label>();
    m_NewProjectErrorLabel = newProjectError.get();
    newProjectError->SetText("");
    newProjectError->SetId("new-project-error");
    newProjectError->AddClass("project-picker-error");
    newProjectView->AddChild(std::move(newProjectError));

    auto importProjectView = std::make_unique<UIElement>();
    m_ImportProjectView = importProjectView.get();
    importProjectView->SetId("import-project-view");

    auto importHeader = std::make_unique<UIElement>();
    UIElement* importHeaderPtr = importHeader.get();
    importHeader->AddClass("project-import-header");

    auto importBack = std::make_unique<Button>();
    importBack->SetText("‹");
    importBack->SetTooltip("Back to projects");
    ApplyModalButtonStyle(*importBack, ModalButtonRole::Secondary);
    importBack->AddClass("project-picker-back-button");
    importBack->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowLibraryView(); });

    auto importHeading = std::make_unique<UIElement>();
    importHeading->AddClass("project-import-heading");
    auto importTitle = std::make_unique<Label>();
    UIElement* importTitlePtr = importTitle.get();
    importTitle->SetText("Import project");
    importTitle->AddClass("project-picker-page-title");
    auto importSubtitle = std::make_unique<Label>();
    importSubtitle->SetText("Bring in an existing project from a folder, a .zip archive, or a Git URL.");
    importSubtitle->AddClass("project-import-subtitle");
    importHeading->AddChild(std::move(importTitle));
    importHeader->AddChild(std::move(importBack));
    importHeader->AddChild(std::move(importHeading));
    importHeader->AddChild(std::move(importSubtitle));
    importProjectView->AddChild(std::move(importHeader));

    auto importTabs = std::make_unique<UIElement>();
    importTabs->AddClass("project-import-tabs");

    auto makeImportTab = [this](const char* text, const char* id, ImportSourceMode mode,
                                Button*& out) {
        auto tab = std::make_unique<Button>();
        out = tab.get();
        tab->SetId(id);
        tab->AddClass("project-import-tab");
        tab->SetText(text);
        tab->RegisterEventHandler(kEventButtonClick, [this, mode](UIEvent&) {
            m_ImportSourceMode = mode;
            UpdateImportProjectMode();
        });
        // Layout, border, radius, font, cursor and every idle/hover/active
        // color come from .project-import-tab in ProjectPicker.css.
        return tab;
    };
    importTabs->AddChild(
        makeImportTab("Folder", "import-folder-tab", ImportSourceMode::Folder, m_ImportFolderTabButton));
    importTabs->AddChild(
        makeImportTab("Archive", "import-archive-tab", ImportSourceMode::Archive, m_ImportArchiveTabButton));
    importTabs->AddChild(
        makeImportTab("Git URL", "import-git-tab", ImportSourceMode::GitUrl, m_ImportGitTabButton));
    importProjectView->AddChild(std::move(importTabs));

    auto makeImportPanel = [](UIElement*& out) {
        auto panel = std::make_unique<UIElement>();
        out = panel.get();
        panel->AddClass("project-import-panel");
        return panel;
    };

    // Without a folder picker (web on Firefox/Safari, or Brave with the File
    // System Access flag off) the dropzone carries the explanation instead of
    // a dead control — SelectFolder cannot say "impossible" after the click.
    const bool folderPickerAvailable = Platform::SupportsFolderPicker();

    auto folderPanel = makeImportPanel(m_ImportFolderView);
    folderPanel->AddClass("project-import-panel-folder");
    auto folderDrop = std::make_unique<Button>();
    folderDrop->AddClass("project-import-dropzone");
    folderDrop->AddClass("project-import-dropzone-folder");
    if (folderPickerAvailable)
    {
        folderDrop->SetText("Choose a project folder");
        folderDrop->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });

        // Explicit "Browse…" affordance in the middle of the dropzone, in addition
        // to the whole zone being clickable. Nested inside the dropzone button;
        // Button::OnEvent captures+stops mouse-down/up so a click on this inner
        // button doesn't also re-trigger the outer dropzone's handler.
        auto folderBrowseButton = std::make_unique<Button>();
        folderBrowseButton->SetText("Browse");
        folderBrowseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });
        ApplyModalButtonStyle(*folderBrowseButton, ModalButtonRole::Primary);
        ApplyCompactModalButtonLayout(*folderBrowseButton, "project-picker-button-w140");
        folderBrowseButton->AddClass("project-picker-button-center");
        folderDrop->AddChild(std::move(folderBrowseButton));
    }
    else
    {
        folderDrop->SetText("This browser can't browse local folders. Use Chrome or Edge — or in "
                            "Brave, enable brave://flags/#file-system-access-api.");
        folderDrop->AddClass("disabled");
    }

    // Layout, colors, hover and the selected check mark all come from
    // .project-import-option* in ProjectPicker.css; `disabled` also turns off
    // the pointer there.
    auto makeFolderImportOption = [](const char* text, bool selected, bool enabled) {
        auto option = std::make_unique<Button>();
        option->AddClass("project-import-option");
        if (selected)
            option->AddClass("selected");
        if (!enabled)
            option->AddClass("disabled");

        auto indicator = std::make_unique<UIElement>();
        indicator->AddClass("project-import-option-indicator");

        auto label = std::make_unique<Label>();
        label->AddClass("project-import-option-label");
        label->SetText(text);
        option->AddChild(std::move(indicator));
        option->AddChild(std::move(label));
        return option;
    };

    auto referenceOption =
        makeFolderImportOption("Reference in place - keep the project where it is",
                               folderPickerAvailable, folderPickerAvailable);
    if (folderPickerAvailable)
        referenceOption->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });
    auto copyOption =
        makeFolderImportOption("Copy into workspace - unavailable in this build", false, false);
    folderPanel->AddChild(std::move(folderDrop));
    folderPanel->AddChild(std::move(referenceOption));
    folderPanel->AddChild(std::move(copyOption));

    auto archivePanel = makeImportPanel(m_ImportArchiveView);
    auto archiveDrop = std::make_unique<Button>();
    m_ImportArchiveDrop = archiveDrop.get();
    archiveDrop->SetId("import-archive-drop");
    archiveDrop->AddClass("project-import-dropzone");
    archiveDrop->SetText("Choose a .zip archive");
    archiveDrop->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnImportArchiveBrowseClicked(); });

    auto archiveBrowseButton = std::make_unique<Button>();
    archiveBrowseButton->SetText("Browse");
    archiveBrowseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnImportArchiveBrowseClicked(); });
    ApplyModalButtonStyle(*archiveBrowseButton, ModalButtonRole::Primary);
    ApplyCompactModalButtonLayout(*archiveBrowseButton, "project-picker-button-w140");
    archiveBrowseButton->AddClass("project-picker-button-center");
    archiveDrop->AddChild(std::move(archiveBrowseButton));
    archivePanel->AddChild(std::move(archiveDrop));

    auto gitPanel = makeImportPanel(m_ImportGitView);
    gitPanel->AddClass("project-import-panel-git");
    auto gitUrlLabel = std::make_unique<Label>();
    gitUrlLabel->SetText("REPOSITORY URL");
    gitUrlLabel->AddClass("project-picker-field-label");
    auto gitUrlField = std::make_unique<TextField>();
    m_ImportGitUrlField = gitUrlField.get();
    gitUrlField->SetId("import-git-url-field");
    gitUrlField->AddClass("project-picker-text-field");
    gitUrlField->AddClass("project-picker-text-field-fill");
    gitUrlField->SetOnValueChanged([this](const std::string&) { UpdateImportProjectEnabled(); });
    gitUrlField->RegisterEventHandler(
        kEventKeyUp, [this](UIEvent&) { UpdateImportProjectEnabled(); });
    auto gitHint = std::make_unique<Label>();
    m_ImportGitHint = gitHint.get();
    gitHint->SetText("");
    gitHint->AddClass("project-import-git-hint");
    gitPanel->AddChild(std::move(gitUrlLabel));
    gitPanel->AddChild(std::move(gitUrlField));
    gitPanel->AddChild(std::move(gitHint));

    importProjectView->AddChild(std::move(folderPanel));
    importProjectView->AddChild(std::move(archivePanel));
    importProjectView->AddChild(std::move(gitPanel));

    // Shared destination for the archive/git flows: the project folder is
    // created inside this directory, named after the archive/repository.
    auto importDestinationRow = std::make_unique<UIElement>();
    m_ImportDestinationRow = importDestinationRow.get();
    importDestinationRow->SetId("import-destination-row");
    auto importDestinationLabel = std::make_unique<Label>();
    importDestinationLabel->SetText("CREATE IN");
    importDestinationLabel->AddClass("project-picker-field-label");
    importDestinationLabel->AddClass("project-picker-field-label-nudged");
    auto importDestinationControls = std::make_unique<UIElement>();
    importDestinationControls->AddClass("project-picker-field-controls");
    auto importDestinationField = std::make_unique<TextField>();
    m_ImportDestinationField = importDestinationField.get();
    importDestinationField->SetId("import-destination-field");
    importDestinationField->AddClass("project-picker-text-field");
    importDestinationField->AddClass("project-picker-text-field-grow");
    auto importDestinationBrowse = std::make_unique<Button>();
    importDestinationBrowse->SetText("Browse");
    importDestinationBrowse->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        const std::filesystem::path selected = Platform::SelectFolder(ResolveHomePath());
        if (!selected.empty() && m_ImportDestinationField)
        {
            m_ImportDestinationField->SetValue(NormalizePath(selected).generic_string());
            MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    });
    ApplyModalButtonStyle(*importDestinationBrowse, ModalButtonRole::Primary);
    ApplyCompactModalButtonLayout(*importDestinationBrowse, "project-picker-button-w140");
    importDestinationBrowse->AddClass("project-picker-button-noshrink");
    importDestinationControls->AddChild(std::move(importDestinationField));
    importDestinationControls->AddChild(std::move(importDestinationBrowse));
    importDestinationRow->AddChild(std::move(importDestinationLabel));
    importDestinationRow->AddChild(std::move(importDestinationControls));
    importProjectView->AddChild(std::move(importDestinationRow));

    auto importError = std::make_unique<Label>();
    m_ImportErrorLabel = importError.get();
    importError->SetText("");
    importError->SetId("import-error");
    importError->AddClass("project-picker-error");
    importProjectView->AddChild(std::move(importError));

    auto newProjectActions = std::make_unique<UIElement>();
    m_NewProjectActions = newProjectActions.get();
    newProjectActions->SetId("new-project-actions");
    auto newProjectCancel = std::make_unique<Button>();
    newProjectCancel->AddClass("new-project-cancel-button");
    newProjectCancel->SetText("Cancel");
    newProjectCancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowLibraryView(); });
    ApplyModalButtonStyle(*newProjectCancel, ModalButtonRole::FilledSecondary);
    ApplyCompactModalButtonLayout(*newProjectCancel, "project-picker-button-w130");
    auto createProject = std::make_unique<Button>();
    m_CreateProjectButton = createProject.get();
    createProject->SetId("create-project-button");
    createProject->SetText("Create project");
    createProject->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCreateProjectClicked(); });
    ApplyModalButtonStyle(*createProject, ModalButtonRole::Primary);
    ApplyCompactModalButtonLayout(*createProject);
    newProjectActions->AddChild(std::move(newProjectCancel));
    newProjectActions->AddChild(std::move(createProject));

    auto importProjectActions = std::make_unique<UIElement>();
    m_ImportProjectActions = importProjectActions.get();
    importProjectActions->SetId("import-project-actions");
    auto importCancel = std::make_unique<Button>();
    importCancel->SetText("Cancel");
    importCancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowLibraryView(); });
    ApplyModalButtonStyle(*importCancel, ModalButtonRole::FilledSecondary);
    ApplyCompactModalButtonLayout(*importCancel, "project-picker-button-w140");
    auto importProjectButton = std::make_unique<Button>();
    m_ImportProjectButton = importProjectButton.get();
    importProjectButton->SetId("import-project-button");
    importProjectButton->SetText("Confirm");
    importProjectButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnImportProjectClicked(); });
    ApplyModalButtonStyle(*importProjectButton, ModalButtonRole::Primary);
    ApplyCompactModalButtonLayout(*importProjectButton, "project-picker-button-w140");
    // Hidden until a valid archive/URL exists; UpdateImportProjectEnabled owns
    // this state from here on.
    importProjectButton->Overrides()
        .Set(Style::Display, DisplayMode::None);
    importProjectActions->AddChild(std::move(importCancel));
    importProjectActions->AddChild(std::move(importProjectButton));

    m_BottomPanel = bottomPanel;

    ApplyScrollbarLane(*scrollView);
    m_RecentProjectsScroll = scrollView;

    // Container for recent projects items. Installed as the ScrollView's
    // content, which is a call the layout asset cannot express.
    auto recentProjectsContainer = std::make_unique<UIElement>();
    recentProjectsContainer->SetId("recent-projects-container");
    // Grid width and gutter are derived from the card metrics and feed the
    // window width budget, so they stay beside those constants.
    recentProjectsContainer->Overrides()
        .Set(Style::Gap, StyleLength::Px(kProjectPickerCardGapPx))
        .Set(Style::Width, StyleLength::Px(kProjectPickerLibraryGridWidthPx))
        .Set(Style::MinWidth, StyleLength::Px(kProjectPickerLibraryGridWidthPx))
        .Set(Style::MaxWidth, StyleLength::Px(kProjectPickerLibraryGridWidthPx));
    m_RecentProjectsContainer = recentProjectsContainer.get();

    scrollView->AddContent(std::move(recentProjectsContainer));

    ApplyModalButtonStyle(*browseButton, ModalButtonRole::Primary);
    ApplyCompactModalButtonLayout(*browseButton, "project-picker-button-w140");
    if (Platform::SupportsFolderPicker())
        browseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });
    else
        browseButton->Overrides().Set(Style::Display, DisplayMode::None);

    UI::Layout::SetBackgroundPath(*projectLogo, "Icons/logo.svg");
    m_ProjectLogo = projectLogo;

    ApplyModalButtonStyle(*openButton, ModalButtonRole::Primary);
    openButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSelectClicked(); });

    m_ButtonContainer = buttonContainer;

    m_Footer = footer;
    // The footer band is a second drag handle, from its own background only so
    // the Cancel button keeps its click.
    EnableWindowDrag(*footer, *windowPtr, /*directTargetOnly=*/true);

    m_CancelButton = cancelButton;
    ApplyModalButtonStyle(*cancelButton, ModalButtonRole::FilledSecondary);
    ApplyCompactModalButtonLayout(*cancelButton, "project-picker-button-w130");
    cancelButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

    m_LoadingLabel = loadingLabel;

    // Defer LoadRecentProjects to first Show() so constructor does not block on filesystem (avoids stall on Windows/Linux)

    // Stacked create/import modal: the library stays beneath a dim scrim, as
    // in the reference prototype where New project opens a second modal on
    // top of the projects window.
    m_StackedScrim = stackedScrim;

    // Fixed size plus its centering margins (margin = -size/2) are one
    // arithmetic unit; the rest of the window is in ProjectPicker.css.
    stackedWindowPtr->Overrides()
        .Set(Style::MarginLeft, StyleLength::Px(-kProjectPickerStackedModalWidthPx * 0.5f))
        .Set(Style::MarginTop, StyleLength::Px(-kProjectPickerStackedModalHeightPx * 0.5f))
        .Set(Style::Width, StyleLength::Px(kProjectPickerStackedModalWidthPx))
        .Set(Style::Height, StyleLength::Px(kProjectPickerStackedModalHeightPx));

    // The create/import views' title rows are this window's title bar. The rows
    // also hold the back button and the tab strip, so they only drag from their
    // own background; the headings drag unconditionally.
    EnableWindowDrag(*newProjectTopPtr, *stackedWindowPtr, /*directTargetOnly=*/true);
    EnableWindowDrag(*newProjectTitlePtr, *stackedWindowPtr);
    EnableWindowDrag(*importHeaderPtr, *stackedWindowPtr, /*directTargetOnly=*/true);
    EnableWindowDrag(*importTitlePtr, *stackedWindowPtr);
    // The action rows fill the footer band, so the band's drag handle has to be
    // the rows themselves — a press never reaches the footer element behind them.
    if (m_NewProjectActions)
        EnableWindowDrag(*m_NewProjectActions, *stackedWindowPtr, /*directTargetOnly=*/true);
    if (m_ImportProjectActions)
        EnableWindowDrag(*m_ImportProjectActions, *stackedWindowPtr, /*directTargetOnly=*/true);
    // The name/location strip is the head of the same bottom band; its fields
    // and labels are children, so only its background drags.
    EnableWindowDrag(*newProjectFormPtr, *stackedWindowPtr, /*directTargetOnly=*/true);

    stackedContentPtr->AddChild(std::move(newProjectView));
    stackedContentPtr->AddChild(std::move(importProjectView));
    // The padded strip above the title row belongs to these backgrounds, so the
    // drag area reaches the window's top edge rather than starting at the row.
    EnableWindowDrag(*stackedContentPtr, *stackedWindowPtr, /*directTargetOnly=*/true);
    if (m_NewProjectView)
        EnableWindowDrag(*m_NewProjectView, *stackedWindowPtr, /*directTargetOnly=*/true);
    if (m_ImportProjectView)
        EnableWindowDrag(*m_ImportProjectView, *stackedWindowPtr, /*directTargetOnly=*/true);

    EnableWindowDrag(*stackedFooter, *stackedWindowPtr, /*directTargetOnly=*/true);
    stackedFooter->AddChild(std::move(newProjectActions));
    stackedFooter->AddChild(std::move(importProjectActions));

    RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            if (m_ShowingNewProject || m_ShowingImportProject)
                ShowLibraryView();
            else
                OnCancelClicked();
        }
        else if (m_ShowingNewProject &&
                 (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter))
        {
            e.Handled = true;
            OnCreateProjectClicked();
        }
    });

    // State that arrived before the tree existed: those setters recorded their
    // values but had no widgets to drive.
    SetShowButtons(m_ShowCancelButton);
    UpdateLayoutForRecentProjects();
}

ProjectFolderPickerModal::~ProjectFolderPickerModal() = default;

void ProjectFolderPickerModal::Show()
{
    // A Show() that beats the deferred build gets the tree here instead.
    EnsureTreeBuilt();

    // A successful project switch closes the modal while its loading page is
    // still visible. Reset that stale state only while the modal is hidden,
    // before presenting a fresh library page on the next open.
    if (m_Loading)
        HideLoading();

    m_Visible = true;
    m_ShowingNewProject = false;
    m_ShowingImportProject = false;
    m_SuppressNativeWebViewAfterHide = false;

    // Make the root structurally visible at opacity 0; the CSS transition on
    // .project-picker-modal will animate it to 1 on the next frame when
    // Update() sets opacity to 1. effectiveOpacity propagates through the
    // tree, so the entire modal fades as a unit.
    Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, true)
        .Set(Style::Opacity, 0.0f);

    m_FadeInPending = true;

    std::filesystem::path currentWorkspace;
    if (EngineCore::GetInstance().IsInitialized())
        currentWorkspace = EngineCore::GetInstance().GetWorkspaceRoot();
    if (currentWorkspace.empty() && !m_SelectedPath.empty())
        currentWorkspace = m_SelectedPath;

    UpdateButtonsAndLogo(currentWorkspace);
    ShowLibraryView();
    UpdateLayoutForRecentProjects();

    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    if (auto* manager = GetOwnerManager())
    {
        manager->PostToUI([manager, this]() { manager->FocusElement(this); });
    }
}

void ProjectFolderPickerModal::Hide()
{
    m_Visible = false;
    if (auto* manager = GetOwnerManager())
    {
        const std::string& focusId = manager->GetFocusedElementId();
        if (!focusId.empty())
        {
            if (UIElement* focused = FindById(focusId))
            {
                for (UIElement* p = focused; p; p = p->GetParent())
                {
                    if (p == this)
                    {
                        manager->ClearFocus();
                        break;
                    }
                }
            }
        }
        m_HideTimeSecondsForNativeWebView = manager->GetTimeSeconds();
        m_SuppressNativeWebViewAfterHide = true;
    }
    else
    {
        m_SuppressNativeWebViewAfterHide = false;
    }
    // Set opacity to 0 and display to none. The CSS transition on
    // .project-picker-modal animates opacity over 0.1s and delays the
    // display:none switch by the same duration, so the element stays
    // visible during the fade-out.
    Overrides()
        .Set(Style::PointerEvents, false)
        .Set(Style::Opacity, 0.0f)
        .Set(Style::Display, DisplayMode::None);
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

std::string ProjectFolderPickerModal::DefaultNewProjectName() const
{
    // Where the platform keeps every project under one flat root, a taken
    // default dead-ends the create at "already exists" — count up to the first
    // free name instead. Elsewhere the location is user-chosen, so probing any
    // one directory would be a guess and the plain default stands.
    const std::filesystem::path library = FileSystem::ProjectLibraryRoot();
    std::string name = "Untitled Project";
    if (library.empty())
        return name;
    std::error_code ec;
    for (int i = 2; std::filesystem::exists(library / name, ec); ++i)
        name = "Untitled Project " + std::to_string(i);
    return name;
}

std::filesystem::path ProjectFolderPickerModal::ResolveHomePath() const
{
    // A platform that confines projects to one root (web: the persistent mount,
    // the only tree that survives a reload) gets that root; $HOME there would
    // be a per-session filesystem where a project silently dies with the tab.
    const std::filesystem::path library = FileSystem::ProjectLibraryRoot();
    if (!library.empty())
        return library;
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0])
        home = std::getenv("USERPROFILE");
    if ((!home || !home[0]))
    {
        const char* drive = std::getenv("HOMEDRIVE");
        const char* path = std::getenv("HOMEPATH");
        if (drive && path)
        {
            return std::filesystem::path(std::string(drive) + std::string(path));
        }
    }
#endif
    if (home && home[0])
        return std::filesystem::path(home);
    return {};
}

std::filesystem::path ProjectFolderPickerModal::NormalizePath(const std::filesystem::path& path) const
{
    if (path.empty())
        return {};

    std::error_code ec;
    std::filesystem::path normalized = path;
    if (!normalized.is_absolute())
        normalized = std::filesystem::absolute(normalized, ec);
    normalized = normalized.lexically_normal();

    std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec && !canonical.empty())
        normalized = canonical.lexically_normal();

    return normalized;
}

std::string ProjectFolderPickerModal::NormalizePathKey(const std::filesystem::path& path) const
{
    std::string key = NormalizePath(path).string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

bool ProjectFolderPickerModal::IsDefaultProjectPath(const std::filesystem::path& path) const
{
    const auto defaultRoot = GameEngine::Editor::GetEditorGlobalPaths().defaultProjectRoot;
    const std::string defaultKey = NormalizePathKey(defaultRoot);
    if (defaultKey.empty())
        return false;

    return NormalizePathKey(path) == defaultKey;
}

void ProjectFolderPickerModal::UpdateButtonsAndLogo(const std::filesystem::path& workspacePath)
{
    const bool isDefaultProject = IsDefaultProjectPath(workspacePath);
    // Cancel must never leave the editor without a project. Recent projects alone
    // are not enough; there must be an active, non-default project to return to.
    const bool shouldShowCancel = !isDefaultProject && !workspacePath.empty();
    // The Open button row is not needed - Browse goes directly to loading,
    // and recent projects each have their own Open button
    const bool shouldShowOpen = false;
    SetShowButtons(shouldShowCancel, shouldShowOpen);
    // #project-logo stays hidden: the browse bar it belonged to is not shown.
}

void ProjectFolderPickerModal::UpdateLayoutForRecentProjects()
{
    if (m_Loading)
        return;

    if (m_ProjectCountLabel)
    {
        if (m_SearchQuery.empty())
        {
            m_ProjectCountLabel->SetText(std::to_string(m_RecentProjects.size()) +
                                         (m_RecentProjects.size() == 1 ? " project" : " projects"));
        }
        else
        {
            m_ProjectCountLabel->SetText(std::to_string(m_FilteredProjectCount) +
                                         (m_FilteredProjectCount == 1 ? " match" : " matches"));
        }
        m_ProjectCountLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_Footer)
    {
        // The library footer only hosts the picker-dismiss Cancel; the
        // create/import actions live in the stacked modal's own footer.
        const bool showLibraryCancel = m_ShowCancelButton;
        m_Footer->Overrides().Set(Style::Display,
                                  showLibraryCancel ? DisplayMode::Flex : DisplayMode::None);
        m_Footer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);

        if (m_CancelButton)
        {
            m_CancelButton->Overrides().Set(
                Style::Display, showLibraryCancel ? DisplayMode::Block : DisplayMode::None);
            m_CancelButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    }

}

void ProjectFolderPickerModal::SetRecentProjectsVisible(bool visible, bool aggressiveHide)
{
    // ProjectPicker.css owns the resting layout of #recent-projects-scroll and
    // #recent-projects-container; only visibility and the off-screen parking
    // used while a project loads are driven from here. Showing again drops the
    // parking overrides so the stylesheet's values come back.
    if (m_RecentProjectsScroll)
    {
        StyleOverrides& scroll = m_RecentProjectsScroll->Overrides();
        if (visible)
        {
            scroll.Set(Style::Display, DisplayMode::Flex)
                .Set(Style::Visibility, true)
                .Set(Style::Position, PositionType::Relative)
                .Set(Style::MarginTop, StyleLength::Px(0.0f));
            scroll.Reset(Style::PositionLeft);
            scroll.Reset(Style::PositionTop);
            scroll.Reset(Style::Width);
            scroll.Reset(Style::Height);
        }
        else
        {
            scroll.Set(Style::Display, DisplayMode::None)
                .Set(Style::Visibility, false)
                .Set(Style::MarginTop, StyleLength::Px(16.0f));
            if (aggressiveHide)
            {
                scroll.Set(Style::Position, PositionType::Absolute)
                    .Set(Style::PositionLeft, StyleLength::Px(-9999.0f))
                    .Set(Style::PositionTop, StyleLength::Px(-9999.0f))
                    .Set(Style::Width, StyleLength::Px(0.0f))
                    .Set(Style::Height, StyleLength::Px(0.0f));
            }
        }
        m_RecentProjectsScroll->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_RecentProjectsContainer)
    {
        StyleOverrides& grid = m_RecentProjectsContainer->Overrides();
        if (visible)
        {
            // The grid width is code-owned (derived from the card metrics), so
            // it is re-set rather than reset.
            grid.Set(Style::Display, DisplayMode::Flex)
                .Set(Style::Visibility, true)
                .Set(Style::Position, PositionType::Relative)
                .Set(Style::Gap, StyleLength::Px(kProjectPickerCardGapPx))
                .Set(Style::Width, StyleLength::Px(kProjectPickerLibraryGridWidthPx))
                .Set(Style::MinWidth, StyleLength::Px(kProjectPickerLibraryGridWidthPx))
                .Set(Style::MaxWidth, StyleLength::Px(kProjectPickerLibraryGridWidthPx));
            grid.Reset(Style::PositionLeft);
            grid.Reset(Style::PositionTop);
            grid.Reset(Style::Height);
            grid.Reset(Style::PaddingTop);
            grid.Reset(Style::PaddingRight);
            grid.Reset(Style::PaddingBottom);
            grid.Reset(Style::PaddingLeft);
        }
        else
        {
            grid.Set(Style::Display, DisplayMode::None)
                .Set(Style::Visibility, false)
                .Set(Style::PaddingTop, StyleLength::Px(0.0f))
                .Set(Style::PaddingRight, StyleLength::Px(0.0f))
                .Set(Style::PaddingBottom, StyleLength::Px(0.0f))
                .Set(Style::PaddingLeft, StyleLength::Px(0.0f));
            if (aggressiveHide)
            {
                grid.Set(Style::Position, PositionType::Absolute)
                    .Set(Style::PositionLeft, StyleLength::Px(-9999.0f))
                    .Set(Style::PositionTop, StyleLength::Px(-9999.0f))
                    .Set(Style::Width, StyleLength::Px(0.0f))
                    .Set(Style::Height, StyleLength::Px(0.0f))
                    .Set(Style::OverflowProp, Overflow::Hidden);
            }
        }
        m_RecentProjectsContainer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void ProjectFolderPickerModal::ShowLoading()
{
    m_Loading = true;

    // CRITICAL: Hide recent projects scroll view FIRST and most aggressively.
    SetRecentProjectsVisible(false, true);

    if (m_ProjectsHeader)
    {
        m_ProjectsHeader->Overrides().Set(Style::Display, DisplayMode::None);
        m_ProjectsHeader->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    // The stacked create/import modal comes down while the loading page shows;
    // HideLoading re-enters the active view, which re-raises it.
    if (m_StackedScrim)
    {
        m_StackedScrim->Overrides().Set(Style::Display, DisplayMode::None);
        m_StackedScrim->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    // Hide everything: title, browse bar (path container), bottom panel, and cancel button.
    if (m_Title)
    {
        m_Title->Overrides().Set(Style::Display, DisplayMode::None);
        m_Title->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_BottomPanel)
    {
        m_BottomPanel->Overrides().Set(Style::Display, DisplayMode::None);
        m_BottomPanel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_CancelButton)
    {
        m_CancelButton->Overrides().Set(Style::Display, DisplayMode::None);
        m_CancelButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_Footer)
    {
        m_Footer->Overrides().Set(Style::Display, DisplayMode::None);
        m_Footer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    // Show loading label - center it vertically in the full content area.
    if (m_LoadingLabel)
    {
        // Update loading text with project folder name (not full path)
        std::string loadingText = "Loading...";
        const std::filesystem::path& pathForName = !m_PendingPath.empty() ? m_PendingPath : m_SelectedPath;
        if (!pathForName.empty())
        {
            std::string folderName = pathForName.filename().string();
            if (!folderName.empty())
                loadingText = "Loading " + folderName;
        }

        if (Label* label = dynamic_cast<Label*>(m_LoadingLabel))
            label->SetText(loadingText);

        m_LoadingLabel->Overrides().Set(Style::Display, DisplayMode::Flex);
        m_LoadingLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::HideLoading()
{
    m_Loading = false;

    std::filesystem::path currentWorkspace;
    if (EngineCore::GetInstance().IsInitialized())
        currentWorkspace = EngineCore::GetInstance().GetWorkspaceRoot();
    if (currentWorkspace.empty() && !m_SelectedPath.empty())
        currentWorkspace = m_SelectedPath;
    UpdateButtonsAndLogo(currentWorkspace);

    if (m_ShowingNewProject)
        ShowNewProjectView();
    else if (m_ShowingImportProject)
        ShowImportProjectView();
    else
        ShowLibraryView();

    if (m_LoadingLabel)
    {
        m_LoadingLabel->Overrides().Set(Style::Display, DisplayMode::None);
        m_LoadingLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::Update()
{
    float now = 0.0f;
    if (auto* manager = GetOwnerManager())
        now = manager->GetTimeSeconds();

    if (!m_Visible && m_SuppressNativeWebViewAfterHide)
    {
        if (auto* mgr = GetOwnerManager())
        {
            if (now - m_HideTimeSecondsForNativeWebView >= kNativeWebViewSuppressionAfterHideSeconds)
                m_SuppressNativeWebViewAfterHide = false;
        }
        else
        {
            m_SuppressNativeWebViewAfterHide = false;
        }
    }

    if (m_FadeInPending)
    {
        // The previous frame established opacity:0. Now set opacity:1
        // so the CSS transition can interpolate the change.
        Overrides().Set(Style::Opacity, 1.0f);
        m_FadeInPending = false;
    }

    if (m_Visible && !m_RecentsLoaded)
    {
        m_RecentsLoaded = true;
        LoadRecentProjects();
        RefreshRecentProjectsList();
        std::filesystem::path currentWorkspace;
        if (EngineCore::GetInstance().IsInitialized())
            currentWorkspace = EngineCore::GetInstance().GetWorkspaceRoot();
        if (currentWorkspace.empty() && !m_SelectedPath.empty())
            currentWorkspace = m_SelectedPath;
        UpdateButtonsAndLogo(currentWorkspace);
        UpdateLayoutForRecentProjects();
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_CatalogService.PollCommunity(m_Community))
    {
        // A stale-cache result re-arms the fetch so the next tab visit
        // retries the live source.
        if (m_Community.FromCache)
            m_CommunityFetchRequested = false;
        if (m_SelectedCommunity >= static_cast<int>(m_Community.Catalog.Entries.size()))
            m_SelectedCommunity = -1;
        RebuildCommunityCards();
        UpdateCommunityStatusView();
        UpdateCreateProjectEnabled();
    }

    Editor::ProjectAcquireStatus acquireStatus;
    if (m_Acquisition.Poll(acquireStatus) &&
        acquireStatus.State != Editor::ProjectAcquireState::Running)
    {
        m_AcquireInFlight = false;
        if (acquireStatus.State == Editor::ProjectAcquireState::Succeeded)
        {
            m_PendingFolderLoad = true;
            m_PendingPath = acquireStatus.Destination;
        }
        else
        {
            HideLoading();
            UpdateCreateProjectEnabled();
            UpdateImportProjectEnabled();
            // After HideLoading: the restored view's Update*Mode clears its
            // error label, so surface the failure last, on whichever flow
            // started the acquisition.
            Label* errorLabel =
                m_ShowingImportProject ? m_ImportErrorLabel : m_NewProjectErrorLabel;
            if (errorLabel)
            {
                errorLabel->SetText(acquireStatus.Error);
                errorLabel->Overrides().Set(Style::Display, DisplayMode::Block);
                errorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            }
        }
    }

    if (m_PendingFolderLoad && m_OnFolderSelected)
    {
        m_PendingFolderLoad = false;
        std::filesystem::path pathToLoad = m_PendingPath;
        m_PendingPath.clear();

        // Now call the callback which will trigger SetProjectFolder
        m_OnFolderSelected(pathToLoad);
        // Note: Hide() will be called after loading completes in SetProjectFolder
    }
    
    if (m_Visible)
    {
        if (m_LastLogoUpdateTime < 0.0f)
            m_LastLogoUpdateTime = now;
        const float dt = std::max(0.0f, now - m_LastLogoUpdateTime);
        m_LastLogoUpdateTime = now;
        m_LogoHueAnimationTime = std::fmod(
            m_LogoHueAnimationTime + dt * kLogoHueDegreesPerSecond, 360.0f);

        if (UIElement* cubeIcon = FindById("cube-icon"))
        {
            const float h = m_LogoHueAnimationTime;
            constexpr float s = 0.5f;
            constexpr float v = 1.0f;
            const float c = v * s;
            const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
            const float m = v - c;
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (h < 60.0f)       { r = c; g = x; }
            else if (h < 120.0f) { r = x; g = c; }
            else if (h < 180.0f) { g = c; b = x; }
            else if (h < 240.0f) { g = x; b = c; }
            else if (h < 300.0f) { r = x; b = c; }
            else                 { r = c; b = x; }

            const uint32_t tint = 0xFF000000u |
                (static_cast<uint32_t>((r + m) * 255.0f) << 16) |
                (static_cast<uint32_t>((g + m) * 255.0f) << 8) |
                static_cast<uint32_t>((b + m) * 255.0f);
            cubeIcon->Styles().SetBackgroundTint(tint);
        }
    }
    else
    {
        m_LastLogoUpdateTime = -1.0f;
    }
}

void ProjectFolderPickerModal::UpdateTemplateSelection()
{
    // Selection is a class toggle; the hover/selected colors live in ProjectPicker.css
    // so state changes animate through the CSS transitions.
    for (size_t index = 0; index < m_TemplateButtons.size(); ++index)
    {
        Button* button = m_TemplateButtons[index];
        if (!button)
            continue;
        const bool selected = static_cast<int>(index) == m_SelectedTemplate;
        if (selected)
            button->AddClass("selected");
        else
            button->RemoveClass("selected");
        button->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    }
}

Button* ProjectFolderPickerModal::AddCatalogCard(UIElement& row, const std::string& elementId,
                                                 const Editor::ProjectCatalogEntry& entry,
                                                 const std::string& subtitle, uint32_t fallbackTint,
                                                 UIElement::EventHandler onClick)
{
    auto card = std::make_unique<Button>();
    Button* cardPtr = card.get();
    card->SetId(elementId);
    card->AddClass("project-template-card");
    card->AddClass("project-card-animated");
    card->RegisterEventHandler(kEventButtonClick, std::move(onClick));
    UIElement& lift = AddProjectCardLift(*card);

    auto preview = std::make_unique<UIElement>();
    preview->AddClass("project-card-preview");
    // Per-entry tint behind the thumbnail.
    preview->Overrides().Set(Style::BackgroundColor,
                             entry.AccentColor != 0 ? entry.AccentColor : fallbackTint);

    auto previewImage = std::make_unique<UIElement>();
    previewImage->AddClass("project-card-preview-image");
    UI::Layout::SetBackgroundPath(*previewImage, entry.ThumbnailRef.empty()
                                                     ? "Icons/project-scene-placeholder.svg"
                                                     : entry.ThumbnailRef);
    previewImage->Overrides().Set(
        Style::BackgroundSize,
        BackgroundSizeValue{entry.ThumbnailContain ? BackgroundSizeMode::Contain
                                                   : BackgroundSizeMode::Cover});
    // Cards rebuilt mid-session (community fetch) reference disk paths the
    // texture registry has never seen; resolve + upload explicitly like the
    // PolyHaven thumbnails do. Constructor-time cards have no manager yet and
    // are handled by the startup upload pass.
    if (!entry.ThumbnailRef.empty())
    {
        if (UIManager* manager = GetOwnerManager())
        {
            const GUID thumbnailGuid = manager->ResolveBackgroundImagePath(entry.ThumbnailRef);
            if (!thumbnailGuid.IsNull())
                manager->EnsureBackgroundTextureUploaded(thumbnailGuid);
        }
    }
    preview->AddChild(std::move(previewImage));

    auto body = std::make_unique<UIElement>();
    body->AddClass("project-card-body");
    auto name = std::make_unique<Label>();
    name->SetText(entry.Name);
    name->AddClass("project-card-title");
    auto subtitleLabel = std::make_unique<Label>();
    subtitleLabel->SetText(subtitle);
    subtitleLabel->AddClass("project-card-meta");
    body->AddChild(std::move(name));
    body->AddChild(std::move(subtitleLabel));
    lift.AddChild(std::move(preview));
    lift.AddChild(std::move(body));

    auto outlineOverlay = std::make_unique<UIElement>();
    outlineOverlay->AddClass("project-card-outline");
    lift.AddChild(std::move(outlineOverlay));

    row.AddChild(std::move(card));
    return cardPtr;
}

void ProjectFolderPickerModal::BuildTemplateCards()
{
    if (!m_TemplateProjectsView)
        return;
    m_TemplateProjectsView->RemoveAllChildren();
    m_TemplateButtons.clear();

    if (m_TemplateCatalog.Entries.empty())
    {
        auto emptyLabel = std::make_unique<Label>();
        emptyLabel->SetText("No templates available — the project is created empty.");
        emptyLabel->AddClass("project-templates-empty");
        m_TemplateProjectsView->AddChild(std::move(emptyLabel));
        return;
    }

    m_SelectedTemplate =
        std::clamp(m_SelectedTemplate, 0, static_cast<int>(m_TemplateCatalog.Entries.size()) - 1);
    for (size_t index = 0; index < m_TemplateCatalog.Entries.size(); ++index)
    {
        const Editor::ProjectCatalogEntry& entry = m_TemplateCatalog.Entries[index];
        const int templateIndex = static_cast<int>(index);
        Button* card = AddCatalogCard(
            *m_TemplateProjectsView, "project-template-" + std::to_string(index), entry,
            entry.Description, kCardPreviewColors[index % 5], [this, templateIndex](UIEvent&) {
                m_SelectedTemplate = templateIndex;
                UpdateTemplateSelection();
            });
        m_TemplateButtons.push_back(card);
    }
    UpdateTemplateSelection();
}

void ProjectFolderPickerModal::RebuildCommunityCards()
{
    if (!m_CommunityGrid)
        return;
    m_CommunityGrid->RemoveAllChildren();
    m_CommunityButtons.clear();

    for (size_t index = 0; index < m_Community.Catalog.Entries.size(); ++index)
    {
        const Editor::ProjectCatalogEntry& entry = m_Community.Catalog.Entries[index];
        std::string subtitle = entry.Author.empty() ? entry.Description : "by " + entry.Author;
        // Plain text, not a star glyph — the editor UI font has no U+2605.
        if (entry.Stars >= 0)
            subtitle += (subtitle.empty() ? "" : " · ") + FormatStars(entry.Stars) + " stars";
        const int communityIndex = static_cast<int>(index);
        Button* card = AddCatalogCard(
            *m_CommunityGrid, "project-community-" + std::to_string(index), entry, subtitle,
            kCardPreviewColors[index % 5],
            [this, communityIndex](UIEvent&) { OnCommunityCardClicked(communityIndex); });
        if (communityIndex == m_SelectedCommunity)
            card->AddClass("selected");
        m_CommunityButtons.push_back(card);
    }
    m_CommunityGrid->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::UpdateCommunityStatusView()
{
    if (!m_CommunityStatusView || !m_CommunityGrid)
        return;

    std::string title;
    std::string message;
    bool showRetry = false;
    bool showGrid = false;
    switch (m_Community.State)
    {
    case Editor::CommunityCatalogState::Idle:
    case Editor::CommunityCatalogState::Fetching:
        title = "Fetching community projects…";
        message = Editor::ProjectCatalogService::GetCommunityManifestSource();
        break;
    case Editor::CommunityCatalogState::Failed:
        title = "Couldn't reach the community catalog";
        message = m_Community.Error;
        showRetry = true;
        break;
    case Editor::CommunityCatalogState::Ready:
        if (m_Community.Catalog.Entries.empty())
        {
            title = "No community projects yet";
            message = "The catalog lists no projects.";
            showRetry = true;
        }
        else
        {
            showGrid = true;
        }
        break;
    }

    m_CommunityStatusView->Overrides().Set(Style::Display,
                                           showGrid ? DisplayMode::None : DisplayMode::Flex);
    m_CommunityGrid->Overrides().Set(Style::Display,
                                     showGrid ? DisplayMode::Flex : DisplayMode::None);
    if (m_CommunityStatusTitle)
        m_CommunityStatusTitle->SetText(title);
    if (m_CommunityStatusMessage)
        m_CommunityStatusMessage->SetText(message);
    if (m_CommunityRetryButton)
        m_CommunityRetryButton->Overrides().Set(Style::Display,
                                                showRetry ? DisplayMode::Flex : DisplayMode::None);
    const bool showOffline = showGrid && m_Community.FromCache;
    if (m_CommunityOfflineBanner)
        m_CommunityOfflineBanner->Overrides().Set(
            Style::Display, showOffline ? DisplayMode::Flex : DisplayMode::None);
    if (m_CommunityOfflineLabel && showOffline)
        m_CommunityOfflineLabel->SetText(
            m_Community.Error.empty()
                ? "Couldn't reach the catalog — showing the last fetched copy."
                : "Couldn't reach the catalog (" + m_Community.Error +
                      ") — showing the last fetched copy.");
    if (m_CommunityProjectsView)
        m_CommunityProjectsView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::OnCommunityCardClicked(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Community.Catalog.Entries.size()))
        return;
    m_SelectedCommunity = index;
    for (size_t i = 0; i < m_CommunityButtons.size(); ++i)
    {
        Button* button = m_CommunityButtons[i];
        if (!button)
            continue;
        if (static_cast<int>(i) == index)
            button->AddClass("selected");
        else
            button->RemoveClass("selected");
        button->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    }

    // Auto-fill the project name unless the user already typed their own.
    if (m_NewProjectNameField)
    {
        const std::string current = TrimProjectText(m_NewProjectNameField->GetValue());
        if (current.empty() || current == "Untitled Project" || current == m_LastAutoFilledName)
        {
            const std::string& entryName = m_Community.Catalog.Entries[index].Name;
            m_NewProjectNameField->SetValue(entryName);
            m_LastAutoFilledName = entryName;
        }
    }
    UpdateCreateProjectEnabled();
}

void ProjectFolderPickerModal::UpdateCreateProjectEnabled()
{
    if (!m_CreateProjectButton)
        return;
    bool enabled = !m_AcquireInFlight;
    if (m_ShowingCommunityProjects)
        enabled = enabled && m_SelectedCommunity >= 0;
    m_CreateProjectButton->Overrides()
        .Set(Style::PointerEvents, enabled)
        .Set(Style::Opacity, enabled ? 1.0f : 0.45f);
    m_CreateProjectButton->MarkDirty(UIElement::VisualDirty);
}

void ProjectFolderPickerModal::UpdateNewProjectTabView()
{
    const bool showCommunity = m_ShowingCommunityProjects;
    if (showCommunity && !m_CommunityFetchRequested)
    {
        m_CommunityFetchRequested = true;
        m_CatalogService.RequestCommunityFetch();
    }
    if (m_TemplateProjectsView)
    {
        m_TemplateProjectsView->Overrides().Set(
            Style::Display, showCommunity ? DisplayMode::None : DisplayMode::Flex);
        m_TemplateProjectsView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_CommunityProjectsView)
    {
        m_CommunityProjectsView->Overrides().Set(
            Style::Display, showCommunity ? DisplayMode::Flex : DisplayMode::None);
        m_CommunityProjectsView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    // Class toggle only: .project-picker-new-tab and its .active / :hover rules
    // in ProjectPicker.css carry the colors and the accent underline.
    auto updateTab = [](Button* tab, bool active) {
        if (!tab)
            return;
        if (active)
            tab->AddClass("active");
        else
            tab->RemoveClass("active");
        tab->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    };
    updateTab(m_TemplatesTabButton, !showCommunity);
    updateTab(m_CommunityTabButton, showCommunity);

    UpdateCommunityStatusView();
    UpdateCreateProjectEnabled();

    if (m_NewProjectErrorLabel)
    {
        m_NewProjectErrorLabel->SetText("");
        m_NewProjectErrorLabel->Overrides().Set(Style::Display, DisplayMode::None);
        m_NewProjectErrorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::UpdateImportProjectMode()
{
    const bool folder = m_ImportSourceMode == ImportSourceMode::Folder;
    const bool archive = m_ImportSourceMode == ImportSourceMode::Archive;
    const bool git = m_ImportSourceMode == ImportSourceMode::GitUrl;
    auto showView = [](UIElement* view, bool visible) {
        if (!view)
            return;
        view->Overrides().Set(Style::Display, visible ? DisplayMode::Flex : DisplayMode::None);
        view->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    };
    showView(m_ImportFolderView, folder);
    showView(m_ImportArchiveView, archive);
    showView(m_ImportGitView, git);

    // Class toggle only: .project-import-tab and its .active / :hover rules in
    // ProjectPicker.css carry the colors.
    auto updateTab = [](Button* tab, bool active) {
        if (!tab)
            return;
        if (active)
            tab->AddClass("active");
        else
            tab->RemoveClass("active");
        tab->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    };
    updateTab(m_ImportFolderTabButton, folder);
    updateTab(m_ImportArchiveTabButton, archive);
    updateTab(m_ImportGitTabButton, git);

    // The archive/git flows create the project inside a chosen directory; the
    // folder flow picks the project directly.
    if (m_ImportDestinationRow)
    {
        m_ImportDestinationRow->Overrides().Set(Style::Display,
                                                folder ? DisplayMode::None : DisplayMode::Flex);
        m_ImportDestinationRow->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_ImportGitHint)
    {
        m_ImportGitHint->SetText(Editor::ProjectAcquisition::IsGitAvailable()
            ? "Public http(s) repository; cloned shallowly."
            : "Requires Git — install it from git-scm.com.");
        m_ImportGitHint->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    if (m_ImportErrorLabel)
    {
        m_ImportErrorLabel->SetText("");
        m_ImportErrorLabel->Overrides().Set(Style::Display, DisplayMode::None);
        m_ImportErrorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    UpdateImportProjectEnabled();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::ShowLibraryView()
{
    if (m_Loading)
        return;

    m_ShowingNewProject = false;
    m_ShowingImportProject = false;
    if (m_StackedScrim)
        m_StackedScrim->Overrides().Set(Style::Display, DisplayMode::None);
    if (m_ProjectsHeader)
        m_ProjectsHeader->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_Title)
        m_Title->Overrides().Set(Style::Display, DisplayMode::Block);
    SetRecentProjectsVisible(true, false);
    UpdateLayoutForRecentProjects();

    if (m_NewProjectErrorLabel)
    {
        m_NewProjectErrorLabel->SetText("");
        m_NewProjectErrorLabel->Overrides().Set(Style::Display, DisplayMode::None);
        m_NewProjectErrorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::ShowNewProjectView()
{
    if (m_Loading)
        return;

    const bool entering = !m_ShowingNewProject;
    m_ShowingNewProject = true;
    m_ShowingImportProject = false;
    if (m_StackedScrim)
        m_StackedScrim->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_NewProjectView)
        m_NewProjectView->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_ImportProjectView)
        m_ImportProjectView->Overrides().Set(Style::Display, DisplayMode::None);
    if (m_NewProjectActions)
        m_NewProjectActions->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_ImportProjectActions)
        m_ImportProjectActions->Overrides().Set(Style::Display, DisplayMode::None);

    if (entering)
    {
        m_ShowingCommunityProjects = false;
        m_SelectedTemplate = 1;
        if (m_NewProjectNameField)
            m_NewProjectNameField->SetValue(DefaultNewProjectName());

        std::filesystem::path initialLocation;
        Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        std::string lastBrowsedDirectory;
        if (prefs.TryGetString("lastBrowsedDirectory", lastBrowsedDirectory) &&
            !lastBrowsedDirectory.empty())
        {
            std::error_code ec;
            const std::filesystem::path candidate(lastBrowsedDirectory);
            if (std::filesystem::is_directory(candidate, ec))
                initialLocation = candidate;
        }
        if (initialLocation.empty())
            initialLocation = ResolveHomePath();
        if (m_NewProjectLocationField)
            m_NewProjectLocationField->SetValue(initialLocation.generic_string());
    }

    UpdateNewProjectTabView();
    UpdateTemplateSelection();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::ShowImportProjectView()
{
    if (m_Loading)
        return;

    m_ShowingNewProject = false;
    m_ShowingImportProject = true;
    m_ImportSourceMode = ImportSourceMode::Folder;

    if (m_ImportDestinationField && TrimProjectText(m_ImportDestinationField->GetValue()).empty())
    {
        std::filesystem::path initialDestination;
        Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        std::string lastBrowsedDirectory;
        if (prefs.TryGetString("lastBrowsedDirectory", lastBrowsedDirectory) &&
            !lastBrowsedDirectory.empty())
        {
            std::error_code ec;
            if (std::filesystem::is_directory(lastBrowsedDirectory, ec))
                initialDestination = lastBrowsedDirectory;
        }
        if (initialDestination.empty())
            initialDestination = ResolveHomePath();
        m_ImportDestinationField->SetValue(initialDestination.generic_string());
    }
    if (m_StackedScrim)
        m_StackedScrim->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_NewProjectView)
        m_NewProjectView->Overrides().Set(Style::Display, DisplayMode::None);
    if (m_ImportProjectView)
        m_ImportProjectView->Overrides().Set(Style::Display, DisplayMode::Flex);
    if (m_NewProjectActions)
        m_NewProjectActions->Overrides().Set(Style::Display, DisplayMode::None);
    if (m_ImportProjectActions)
        m_ImportProjectActions->Overrides().Set(Style::Display, DisplayMode::Flex);

    UpdateImportProjectMode();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::OnNewProjectClicked()
{
    ShowNewProjectView();
}

void ProjectFolderPickerModal::OnNewProjectBrowseClicked()
{
    if (!Platform::SupportsFolderPicker())
        return;
    std::filesystem::path initialPath;
    if (m_NewProjectLocationField)
    {
        const std::string typedPath = TrimProjectText(m_NewProjectLocationField->GetValue());
        if (!typedPath.empty())
        {
            std::error_code ec;
            const std::filesystem::path candidate = NormalizePath(std::filesystem::path(typedPath));
            if (std::filesystem::is_directory(candidate, ec))
                initialPath = candidate;
        }
    }
    if (initialPath.empty())
        initialPath = ResolveHomePath();

    const std::filesystem::path selected = Platform::SelectFolder(initialPath);
    if (!selected.empty() && m_NewProjectLocationField)
    {
        m_NewProjectLocationField->SetValue(NormalizePath(selected).generic_string());
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void ProjectFolderPickerModal::OnCreateProjectClicked()
{
    auto setError = [this](const std::string& message) {
        if (m_NewProjectErrorLabel)
        {
            m_NewProjectErrorLabel->SetText(message);
            m_NewProjectErrorLabel->Overrides().Set(Style::Display, DisplayMode::Block);
            m_NewProjectErrorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    };

    if (m_AcquireInFlight)
        return;

    const Editor::ProjectCatalogEntry* entry = nullptr;
    if (m_ShowingCommunityProjects)
    {
        if (m_SelectedCommunity < 0 ||
            m_SelectedCommunity >= static_cast<int>(m_Community.Catalog.Entries.size()))
        {
            setError("Select a community project first.");
            return;
        }
        entry = &m_Community.Catalog.Entries[m_SelectedCommunity];
        if (entry->Source.Type == Editor::ProjectSourceType::Unsupported)
        {
            setError("This project's source type is not supported by this build.");
            return;
        }
        if (entry->Source.Type == Editor::ProjectSourceType::Git &&
            !Editor::ProjectAcquisition::IsGitAvailable())
        {
            setError("Cloning community projects requires Git. Install it from git-scm.com.");
            return;
        }
    }
    else if (m_SelectedTemplate >= 0 &&
             m_SelectedTemplate < static_cast<int>(m_TemplateCatalog.Entries.size()))
    {
        entry = &m_TemplateCatalog.Entries[m_SelectedTemplate];
    }

    const std::string projectName = SanitizeProjectName(
        m_NewProjectNameField ? m_NewProjectNameField->GetValue() : std::string{});
    const std::string locationText = m_NewProjectLocationField
        ? TrimProjectText(m_NewProjectLocationField->GetValue())
        : std::string{};
    const std::filesystem::path parent = NormalizePath(
        locationText.empty() ? ResolveHomePath() : std::filesystem::path(locationText));
    if (parent.empty())
    {
        setError("Choose a valid location for the project.");
        return;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec))
    {
        setError("The project location must be an existing folder.");
        return;
    }

    const std::filesystem::path projectPath = (parent / projectName).lexically_normal();
    if (IsDefaultProjectPath(projectPath))
    {
        setError("The default project folder cannot be used for a new project.");
        return;
    }
    if (std::filesystem::exists(projectPath, ec))
    {
        setError("A project with that name already exists at this location.");
        return;
    }

    if (m_NewProjectNameField)
        m_NewProjectNameField->SetValue(projectName);
    if (m_NewProjectLocationField)
        m_NewProjectLocationField->SetValue(parent.generic_string());

    // Both tabs materialize through the acquisition worker (template payload
    // copy or git clone); Update() polls it and opens the project on success.
    Editor::ProjectAcquireRequest request;
    if (entry)
        request.Entry = *entry;
    request.CatalogRoot = m_TemplateCatalogRoot;
    request.Destination = projectPath;
    request.DisplayName = projectName;

    m_SelectedPath = projectPath;
    if (!m_Acquisition.Start(std::move(request)))
    {
        setError("Another project is still being created.");
        return;
    }
    m_AcquireInFlight = true;
    UpdateCreateProjectEnabled();
    ShowLoading();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::OnImportArchiveBrowseClicked()
{
    const std::filesystem::path selected =
        Platform::SelectFile(ResolveHomePath(), "Zip Archives", "*.zip");
    if (selected.empty())
        return;
    m_ImportArchivePath = selected;
    if (m_ImportArchiveDrop)
    {
        m_ImportArchiveDrop->SetText(selected.filename().string());
        m_ImportArchiveDrop->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
    UpdateImportProjectEnabled();
}

void ProjectFolderPickerModal::UpdateImportProjectEnabled()
{
    if (!m_ImportProjectButton)
        return;
    bool hasValidSource = false;
    if (m_ImportSourceMode == ImportSourceMode::Archive)
    {
        std::error_code ec;
        hasValidSource = !m_ImportArchivePath.empty() &&
                         std::filesystem::is_regular_file(m_ImportArchivePath, ec);
    }
    else if (m_ImportSourceMode == ImportSourceMode::GitUrl)
    {
        const std::string url =
            m_ImportGitUrlField ? TrimProjectText(m_ImportGitUrlField->GetValue()) : std::string{};
        hasValidSource = IsHttpUrl(url) && Editor::ProjectAcquisition::IsGitAvailable();
    }

    // Folder selection imports immediately through Browse. Archive and Git
    // require a valid source before there is anything for Confirm to submit.
    const bool enabled = !m_AcquireInFlight && hasValidSource;
    m_ImportProjectButton->Overrides()
        .Set(Style::Display, enabled ? DisplayMode::Flex : DisplayMode::None);
    m_ImportProjectButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::OnImportProjectClicked()
{
    auto setError = [this](const std::string& message) {
        if (m_ImportErrorLabel)
        {
            m_ImportErrorLabel->SetText(message);
            m_ImportErrorLabel->Overrides().Set(Style::Display, DisplayMode::Block);
            m_ImportErrorLabel->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    };

    if (m_AcquireInFlight || m_ImportSourceMode == ImportSourceMode::Folder)
        return;

    Editor::ProjectCatalogEntry entry;
    std::filesystem::path localArchive;
    if (m_ImportSourceMode == ImportSourceMode::Archive)
    {
        std::error_code ec;
        if (m_ImportArchivePath.empty() || !std::filesystem::is_regular_file(m_ImportArchivePath, ec))
        {
            setError("Choose a .zip archive first.");
            return;
        }
        entry.Name = SanitizeProjectName(m_ImportArchivePath.stem().string());
        entry.Source.Type = Editor::ProjectSourceType::Zip;
        localArchive = m_ImportArchivePath;
    }
    else
    {
        const std::string url =
            m_ImportGitUrlField ? TrimProjectText(m_ImportGitUrlField->GetValue()) : std::string{};
        if (!IsHttpUrl(url))
        {
            setError("Enter an http(s) repository URL.");
            return;
        }
        if (!Editor::ProjectAcquisition::IsGitAvailable())
        {
            setError("Cloning requires Git. Install it from git-scm.com.");
            return;
        }
        entry.Name = SanitizeProjectName(DeriveGitProjectName(url));
        entry.Source.Type = Editor::ProjectSourceType::Git;
        entry.Source.Url = url;
    }
    entry.Id = MakeCatalogSlug(entry.Name);

    const std::string destinationText = m_ImportDestinationField
        ? TrimProjectText(m_ImportDestinationField->GetValue())
        : std::string{};
    const std::filesystem::path parent = NormalizePath(
        destinationText.empty() ? ResolveHomePath() : std::filesystem::path(destinationText));
    std::error_code ec;
    if (parent.empty() || !std::filesystem::is_directory(parent, ec))
    {
        setError("Choose an existing folder to create the project in.");
        return;
    }
    const std::filesystem::path projectPath = (parent / entry.Name).lexically_normal();
    if (IsDefaultProjectPath(projectPath))
    {
        setError("The default project folder cannot be used.");
        return;
    }
    if (std::filesystem::exists(projectPath, ec))
    {
        setError("A folder named '" + entry.Name + "' already exists at this location.");
        return;
    }

    Editor::ProjectAcquireRequest request;
    request.Entry = entry;
    request.Destination = projectPath;
    request.DisplayName = entry.Name;
    request.LocalArchivePath = std::move(localArchive);

    m_SelectedPath = projectPath;
    if (!m_Acquisition.Start(std::move(request)))
    {
        setError("Another project is still being imported.");
        return;
    }
    m_AcquireInFlight = true;
    UpdateImportProjectEnabled();
    ShowLoading();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void ProjectFolderPickerModal::OnBrowseClicked()
{
    if (!Platform::SupportsFolderPicker())
        return;
    // Determine initial path for folder picker
    std::filesystem::path initialPath;
    
    // Check if current workspace is the default project
    std::filesystem::path currentWorkspace;
    if (EngineCore::GetInstance().IsInitialized())
    {
        currentWorkspace = EngineCore::GetInstance().GetWorkspaceRoot();
    }
    
    const bool isCurrentlyDefault = IsDefaultProjectPath(currentWorkspace);
    
    if (isCurrentlyDefault)
    {
        // If default project is open, start at user home directory
        initialPath = ResolveHomePath();
    }
    else
    {
        // If not default project, try to use last browsed directory from preferences
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        
        std::string lastBrowsedDir;
        if (prefs.TryGetString("lastBrowsedDirectory", lastBrowsedDir) && !lastBrowsedDir.empty())
        {
            std::filesystem::path lastDir = std::filesystem::path(lastBrowsedDir);
            std::error_code ec;
            if (std::filesystem::exists(lastDir, ec) && std::filesystem::is_directory(lastDir, ec))
            {
                initialPath = lastDir;
            }
        }
        
        // Fallback to user home if no last browsed directory
        if (initialPath.empty())
            initialPath = ResolveHomePath();
    }

    std::filesystem::path selected = Platform::SelectFolder(initialPath);
    if (!selected.empty())
    {
        // Normalize the selected path
        std::filesystem::path normalizedSelected = NormalizePath(selected);
        
        // Prevent selecting the default project
        if (IsDefaultProjectPath(normalizedSelected))
        {
            Logger::Log::Warning("Cannot select the default project folder. Please choose a different folder.");
            return;
        }
        
        // Save the browsed directory for next time
        GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        prefs.SetString("lastBrowsedDirectory", normalizedSelected.string());
        (void)prefs.Save(&err);
        
        m_SelectedPath = normalizedSelected;
        
        // Path is no longer displayed in the browse bar
        
        // Hide recent projects list immediately before showing loading
        SetRecentProjectsVisible(false, true);
        
        // Show loading and defer the folder loading to allow UI to update first
        // Note: AddToRecentProjects will be called in SetProjectFolder after successful load
        ShowLoading();
        m_PendingFolderLoad = true;
        m_PendingPath = m_SelectedPath;
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        // The actual folder loading will happen in Update() to allow UI to render first
    }
}

void ProjectFolderPickerModal::OnSelectClicked()
{
    if (m_SelectedPath.empty())
    {
        // Show error or prompt to select a folder
        Logger::Log::Warning("Please select a project folder");
        return;
    }

    // CRITICAL: Hide recent projects list IMMEDIATELY before any other operations.
    SetRecentProjectsVisible(false, true);
    // Force immediate UI update before proceeding
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    
    // Show loading and defer the folder loading to allow UI to update first
    ShowLoading();
    m_PendingFolderLoad = true;
    m_PendingPath = m_SelectedPath;
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    // The actual folder loading will happen in Update() to allow UI to render first
}

void ProjectFolderPickerModal::OnCancelClicked()
{
    if (m_OnCancelled)
    {
        m_OnCancelled();
    }
    
    Hide();
}

void ProjectFolderPickerModal::SetPath(const std::filesystem::path& path)
{
    m_SelectedPath = NormalizePath(path);
    
    // Path label in browse bar is always empty - never display path text

    UpdateButtonsAndLogo(m_SelectedPath);
    UpdateLayoutForRecentProjects();
}

void ProjectFolderPickerModal::SetShowButtons(bool showCancel, bool showOpen)
{
    // Always set the complete style to avoid corruption from partial style modifications
    m_ShowCancelButton = showCancel;
    if (m_Footer)
    {
        m_Footer->Overrides().Set(
            Style::Display, showCancel ? DisplayMode::Flex : DisplayMode::None);
        m_Footer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_CancelButton)
    {
        m_CancelButton->Overrides().Set(Style::Display, showCancel ? DisplayMode::Block : DisplayMode::None);
        m_CancelButton->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }

    if (m_ButtonContainer)
    {
        m_ButtonContainer->Overrides().Set(Style::Display, showOpen ? DisplayMode::Flex : DisplayMode::None);
        m_ButtonContainer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    }
}

void ProjectFolderPickerModal::AddToRecentProjects(const std::filesystem::path& path)
{
    const std::filesystem::path normalized = NormalizePath(path);
    if (normalized.empty())
        return;
    
    const std::string key = NormalizePathKey(normalized);
    m_RecentProjectLastOpened[key] = CurrentUnixSeconds();
    m_RecentProjects.erase(
        std::remove_if(m_RecentProjects.begin(), m_RecentProjects.end(),
                       [this, &key](const std::filesystem::path& existing) {
                           return NormalizePathKey(existing) == key;
                       }),
        m_RecentProjects.end());
    
    // Add to front
    m_RecentProjects.insert(m_RecentProjects.begin(), normalized);

    PersistRecentProjects();
    RefreshRecentProjectsList();
}

void ProjectFolderPickerModal::PersistRecentProjects()
{
    GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    nlohmann::json recentArray = nlohmann::json::array();
    for (const auto& p : m_RecentProjects)
    {
        const std::string projectKey = NormalizePathKey(p);
        recentArray.push_back({
            {"path", p.string()},
            {"lastOpened", m_RecentProjectLastOpened[projectKey]}
        });
    }
    prefs.SetJson("recentProjects", recentArray);
    (void)prefs.Save(&err);
}

void ProjectFolderPickerModal::DeleteRecentProject(const std::filesystem::path& path)
{
    const std::filesystem::path normalized = NormalizePath(path);
    if (normalized.empty())
        return;

    // Never delete the project the editor currently has open: the async delete
    // would pull files out from under the live session. The card omits the
    // delete item for the open project, so this only fires if a caller bypasses it.
    if (EngineCore::GetInstance().IsInitialized())
    {
        const std::string openKey =
            NormalizePathKey(NormalizePath(EngineCore::GetInstance().GetWorkspaceRoot()));
        if (!openKey.empty() && NormalizePathKey(normalized) == openKey)
        {
            Logger::Log::Warning(
                "ProjectFolderPickerModal: refused to delete the open project '{}'",
                normalized.string());
            return;
        }
    }

    // Forget the project first: removing it from recents and refreshing the list
    // do not walk the project tree, so the UI updates immediately.
    const std::string key = NormalizePathKey(normalized);
    m_RecentProjectLastOpened.erase(key);
    m_ProjectThumbnailPathCache.erase(key);
    m_RecentProjects.erase(
        std::remove_if(m_RecentProjects.begin(), m_RecentProjects.end(),
                       [this, &key](const std::filesystem::path& existing) {
                           return NormalizePathKey(existing) == key;
                       }),
        m_RecentProjects.end());
    PersistRecentProjects();
    RefreshRecentProjectsList();

    // Files are deleted only for projects the editor owns: everything in the
    // platform's project library where one exists, otherwise the managed
    // projects root. An externally opened folder is forgotten, never removed.
    const std::filesystem::path library = FileSystem::ProjectLibraryRoot();
    const std::filesystem::path ownedRoot =
        (library.empty() ? GameEngine::Editor::GetEditorGlobalPaths().projectsRoot : library)
            .lexically_normal();
    const std::filesystem::path rel = normalized.lexically_normal().lexically_relative(ownedRoot);
    const bool underOwnedRoot = !rel.empty() && rel.begin()->string() != "..";
    if (!underOwnedRoot)
    {
        Logger::Log::Info("ProjectFolderPickerModal: forgot external project '{}' (files kept)",
                          normalized.string());
        return;
    }

    // Off the UI thread: the tree may be large, or live behind a storage proxy
    // that a synchronous walk from this thread would deadlock on.
    if (!m_TreeRemoval)
        m_TreeRemoval = std::make_unique<FileSystem::TreeRemoval>(EngineCore::GetInstance().GetJobSystem());
    m_TreeRemoval->Remove(normalized);
}

void ProjectFolderPickerModal::LoadRecentProjects()
{
    m_RecentProjects.clear();
    m_RecentProjectLastOpened.clear();
    m_ProjectThumbnailPathCache.clear();

    GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    
    const auto& root = prefs.Json();
    const auto it = root.find("recentProjects");
    if (it != root.end() && it->is_array())
    {
        std::unordered_set<std::string> seen;
        for (const auto& item : *it)
        {
            std::string storedPath;
            int64_t lastOpened = 0;
            if (item.is_string())
                storedPath = item.get<std::string>();
            else if (item.is_object())
            {
                storedPath = item.value("path", std::string{});
                lastOpened = item.value("lastOpened", int64_t{0});
            }

            if (!storedPath.empty())
            {
                std::filesystem::path projectPath = std::filesystem::path(storedPath);
                // Verify the path still exists
                std::error_code ec;
                if (std::filesystem::exists(projectPath, ec) && std::filesystem::is_directory(projectPath, ec))
                {
                    const std::filesystem::path normalized = NormalizePath(projectPath);
                    const std::string key = NormalizePathKey(normalized);
                    if (!normalized.empty() && seen.insert(key).second)
                    {
                        m_RecentProjects.push_back(normalized);
                        if (lastOpened > 0)
                            m_RecentProjectLastOpened[key] = lastOpened;
                    }
                }
            }
        }
    }

    // startup with --project writes lastProjectPath, while recentProjects is only
    // updated after picker-based selection. Always prefer lastProjectPath as the
    // first entry when it still points to a valid directory so the picker opens
    // to the most recently used project deterministically.
    std::string lastProjectPath;
    if (prefs.TryGetString("lastProjectPath", lastProjectPath) && !lastProjectPath.empty())
    {
        std::filesystem::path projectPath = std::filesystem::path(lastProjectPath);
        std::error_code ec;
        if (std::filesystem::exists(projectPath, ec) && std::filesystem::is_directory(projectPath, ec))
        {
            const std::filesystem::path normalized = NormalizePath(projectPath);
            if (!normalized.empty())
            {
                const std::string key = NormalizePathKey(normalized);
                m_RecentProjects.erase(
                    std::remove_if(m_RecentProjects.begin(),
                                   m_RecentProjects.end(),
                                   [this, &key](const std::filesystem::path& existing)
                                   {
                                       return NormalizePathKey(existing) == key;
                                   }),
                    m_RecentProjects.end());

                m_RecentProjects.insert(m_RecentProjects.begin(), normalized);
                m_RecentProjectLastOpened[key] = CurrentUnixSeconds();
            }
        }
    }

    // Where the platform keeps every project under one root, that root IS the
    // project library, and preferences only know the ones the picker itself
    // opened. Folders that got there any other way — created before a failed
    // open, imported by the folder picker, planted by the seed — would otherwise
    // be invisible and undeletable while still blocking their name. Dot-prefixed
    // entries (.user-data, .cache) are infrastructure, not projects.
    const std::filesystem::path library = FileSystem::ProjectLibraryRoot();
    if (library.empty())
        return;
    std::unordered_set<std::string> listed;
    for (const auto& p : m_RecentProjects)
        listed.insert(NormalizePathKey(p));
    size_t found = 0;
    for (const std::filesystem::path& dir : FileSystem::ListDirectories(library))
    {
        const std::string name = dir.filename().string();
        if (name.empty() || name.front() == '.')
            continue;
        ++found;
        const std::filesystem::path normalized = NormalizePath(dir);
        if (!normalized.empty() && listed.insert(NormalizePathKey(normalized)).second)
            m_RecentProjects.push_back(normalized);
    }
    LOG_INFO("Picker: {} project(s) in the project library", found);
}

void ProjectFolderPickerModal::RefreshRecentProjectsList()
{
    if (UIElement::IsInEventDispatch())
    {
        if (m_RefreshRecentsPosted)
            return;
        m_RefreshRecentsPosted = true;
        PostAction([this]() {
            m_RefreshRecentsPosted = false;
            RefreshRecentProjectsList();
        });
        return;
    }

    m_RefreshRecentsPosted = false;

    if (!m_RecentProjectsContainer)
        return;
    
    // Clear existing items
    auto& children = m_RecentProjectsContainer->GetChildren();
    while (!children.empty())
    {
        m_RecentProjectsContainer->RemoveChild(children.back().get());
    }
    
    std::string normalizedQuery = m_SearchQuery;
    std::transform(normalizedQuery.begin(), normalizedQuery.end(), normalizedQuery.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    m_FilteredProjectCount = 0;

    auto addCardOutline = [](UIElement& card) {
        auto outline = std::make_unique<UIElement>();
        outline->AddClass("project-card-outline");
        card.AddChild(std::move(outline));
    };

    // The first tile is the native equivalent of the HTML reference's dashed
    // "New project" card and opens the dedicated creation flow above.
    auto newCard = std::make_unique<Button>();
    newCard->SetId("recent-project-card-new");
    newCard->AddClass("project-card");
    newCard->AddClass("project-card-animated");
    newCard->AddClass("project-card-new");
    newCard->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnNewProjectClicked(); });
    UIElement& newLift = AddProjectCardLift(*newCard);

    auto newPreview = std::make_unique<UIElement>();
    newPreview->AddClass("project-card-empty-preview");
    newPreview->AddClass("project-card-preview");

    auto plusIcon = std::make_unique<UIElement>();
    plusIcon->AddClass("project-card-plus-icon");
    plusIcon->AddClass("project-card-action-icon");
    UI::Layout::SetBackgroundPath(*plusIcon, "Icons/plus.png");
    newPreview->AddChild(std::move(plusIcon));

    auto newBody = std::make_unique<UIElement>();
    newBody->AddClass("project-card-body");
    auto newName = std::make_unique<Label>();
    newName->SetText("New project");
    newName->AddClass("project-card-title");
    auto newMeta = std::make_unique<Label>();
    newMeta->SetText("Blank · 3D · Open World");
    newMeta->AddClass("project-card-meta");
    newBody->AddChild(std::move(newName));
    newBody->AddChild(std::move(newMeta));
    newLift.AddChild(std::move(newPreview));
    newLift.AddChild(std::move(newBody));
    addCardOutline(newLift);
    m_RecentProjectsContainer->AddChild(std::move(newCard));

    // Keep opening an existing project as an explicit first-class action when the
    // recent-project list is empty (and alongside it when recents are available).
    auto openCard = std::make_unique<Button>();
    openCard->SetId("recent-project-card-open");
    openCard->AddClass("project-card");
    openCard->AddClass("project-card-animated");
    openCard->AddClass("project-card-open");
    openCard->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ShowImportProjectView(); });
    UIElement& openLift = AddProjectCardLift(*openCard);

    auto openPreview = std::make_unique<UIElement>();
    openPreview->AddClass("project-card-empty-preview");
    openPreview->AddClass("project-card-preview");
    auto folderIcon = std::make_unique<UIElement>();
    folderIcon->AddClass("project-card-folder-icon");
    folderIcon->AddClass("project-card-action-icon");
    UI::Layout::SetBackgroundPath(*folderIcon, "Icons/Folder@64px.png");
    openPreview->AddChild(std::move(folderIcon));

    auto openBody = std::make_unique<UIElement>();
    openBody->AddClass("project-card-body");
    auto openName = std::make_unique<Label>();
    openName->SetText("Open project");
    openName->AddClass("project-card-title");
    auto openMeta = std::make_unique<Label>();
    openMeta->SetText("Choose an existing folder");
    openMeta->AddClass("project-card-meta");
    openBody->AddChild(std::move(openName));
    openBody->AddChild(std::move(openMeta));
    openLift.AddChild(std::move(openPreview));
    openLift.AddChild(std::move(openBody));
    addCardOutline(openLift);
    m_RecentProjectsContainer->AddChild(std::move(openCard));

    // The currently-open project is exempt from deletion below: on web the async
    // OPFS removeEntry would tear its tree out from under the live editor while it
    // still streams and saves from those files. Close it first to delete it.
    std::string openWorkspaceKey;
    if (EngineCore::GetInstance().IsInitialized())
        openWorkspaceKey =
            NormalizePathKey(NormalizePath(EngineCore::GetInstance().GetWorkspaceRoot()));

    size_t projectIndex = 0;
    for (const auto& projectPath : m_RecentProjects)
    {
        const std::string fullPath = projectPath.string();
        const std::string projectName = ReadProjectDisplayName(projectPath);
        std::string searchable = projectName + " " + fullPath;
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!normalizedQuery.empty() && searchable.find(normalizedQuery) == std::string::npos)
            continue;

        ++m_FilteredProjectCount;

        auto card = std::make_unique<Button>();
        card->SetId("recent-project-card-" + std::to_string(projectIndex));
        card->AddClass("project-card");
        card->AddClass("project-card-animated");
        std::filesystem::path pathToOpen = projectPath;
        card->RegisterEventHandler(kEventButtonClick, [this, pathToOpen](UIEvent&) { OnRecentProjectClicked(pathToOpen); });

        // Right-click a project card to delete it. A submenu confirm guards the
        // irreversible file delete. The open project has no delete item — deleting
        // it while the editor streams from its files is unsafe; close it first.
        if (openWorkspaceKey.empty() || NormalizePathKey(pathToOpen) != openWorkspaceKey)
        {
            card->AddManipulator(ContextMenuManipulator::Create(
                std::vector<ContextMenuManipulator::Item>{
                    {.Path = "Delete project/Confirm delete",
                     .OnActivate = [this, pathToOpen]() { DeleteRecentProject(pathToOpen); }}}));
        }

        UIElement& lift = AddProjectCardLift(*card);

        auto preview = std::make_unique<UIElement>();
        preview->SetId("recent-preview-" + std::to_string(projectIndex));
        preview->AddClass("project-card-preview");
        preview->Overrides().Set(Style::BackgroundColor, kCardPreviewColors[projectIndex % 5]);

        auto previewImage = std::make_unique<UIElement>();
        previewImage->AddClass("project-card-preview-image");
        // Resolve the card thumbnail once per picker session and cache it: on web
        // FindProjectSceneThumbnail does per-card OPFS stats, and re-running them on
        // every refresh (search keystrokes, the post-delete refresh) stacks
        // main-thread proxy round-trips behind the async project delete and stalls
        // the frame loop. The cache makes every refresh after the first free.
        const std::string savedThumbnail = [&]() -> std::string {
            const std::string thumbKey = NormalizePathKey(projectPath);
            auto cached = m_ProjectThumbnailPathCache.find(thumbKey);
            if (cached != m_ProjectThumbnailPathCache.end())
                return cached->second;
            std::string resolved = FindProjectSceneThumbnail(projectPath);
            m_ProjectThumbnailPathCache.emplace(thumbKey, resolved);
            return resolved;
        }();
        UI::Layout::SetBackgroundPath(
            *previewImage, savedThumbnail.empty() ? "Icons/project-scene-placeholder.svg" : savedThumbnail);
        previewImage->Overrides().Set(
            Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Cover});
        preview->AddChild(std::move(previewImage));

        auto cardBody = std::make_unique<UIElement>();
        cardBody->AddClass("project-card-body");

        auto nameLabel = std::make_unique<Label>();
        nameLabel->SetText(projectName);
        nameLabel->AddClass("project-card-title");

        const std::string projectKey = NormalizePathKey(projectPath);
        const auto openedIt = m_RecentProjectLastOpened.find(projectKey);
        const std::string lastOpenedText = openedIt != m_RecentProjectLastOpened.end()
            ? FormatEditedTime(openedIt->second)
            : "edited recently";
        const uintmax_t projectSizeBytes = CalculateProjectSizeBytes(projectPath);
        auto lastOpenedLabel = std::make_unique<Label>();
        lastOpenedLabel->SetText(projectSizeBytes > 0
                                     ? lastOpenedText + " · " + FormatProjectSize(projectSizeBytes)
                                     : lastOpenedText);
        lastOpenedLabel->AddClass("project-card-meta");

        cardBody->AddChild(std::move(nameLabel));
        cardBody->AddChild(std::move(lastOpenedLabel));
        lift.AddChild(std::move(preview));
        lift.AddChild(std::move(cardBody));

        addCardOutline(lift);
        m_RecentProjectsContainer->AddChild(std::move(card));
        ++projectIndex;
    }
    
    UpdateLayoutForRecentProjects();
}

void ProjectFolderPickerModal::OnRecentProjectClicked(const std::filesystem::path& path)
{
    m_SelectedPath = NormalizePath(path);
    
    // Path is no longer displayed in the browse bar
    
    // CRITICAL: Hide recent projects list IMMEDIATELY before any other operations.
    SetRecentProjectsVisible(false, true);
    // Force immediate UI update before proceeding
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    
    // Show loading and defer the folder loading
    // Note: AddToRecentProjects will be called in SetProjectFolder after successful load,
    // which will move it to the front of the list
    ShowLoading();
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    
    m_PendingFolderLoad = true;
    m_PendingPath = m_SelectedPath;
}

} // namespace GameEngine
