#include "Panels/WebPanel.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "AssetCore/AssetTypes.h"
#include "Core/Engine.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"
#include "Core/WindowInputRouter.h"
#include "Platform/WebView.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <sstream>
#include <filesystem>

namespace
{
constexpr int kWebViewOffScreenX = -10000;
constexpr int kWebViewOffScreenY = -10000;
constexpr const char* kLastURLPrefKey = "ui.webpanel.lastUrl";
GameEngine::WebPanel* s_NativeDragPanel = nullptr;

bool IsNativeDragAssetSupported(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec))
        return false;

    std::string ext = path.extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".webp" || ext == ".gif" || ext == ".bmp";
}
}

namespace GameEngine
{

// Same as AssetsBrowserController / BookmarksPanel / SettingsPanel: asset grid drag state
// (defined in BookmarksPanel.cpp).
struct DragState
{
    bool active = false;
    std::filesystem::path assetPath;
    std::string scenePath;
    std::string entityId;
};
extern DragState g_DragState;

WebPanel::WebPanel()
    : DockPanel("Web")
{
    AddClass("web-panel");
    s_NativeDragPanel = this;
}

WebPanel::~WebPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_LayoutLoadHandle)
        m_LayoutLoadHandle->Cancel();
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    if (s_NativeDragPanel == this)
        s_NativeDragPanel = nullptr;

    for (auto& tab : m_Tabs)
    {
        if (tab.view)
        {
            tab.view->Destroy();
            tab.view.reset();
        }
    }
}

bool WebPanel::TryStartNativeAssetDrag(const std::filesystem::path& path)
{
    WebPanel* panel = s_NativeDragPanel;
    if (!panel || !panel->m_WebViewContainer)
        return false;
    Platform::IWebView* view = panel->ActiveView();
    if (!view || !view->IsVisible() || !IsNativeDragAssetSupported(path))
        return false;

    return view->BeginFileDrag(path);
}

WebPanel::WebTab* WebPanel::ActiveTab()
{
    if (m_ActiveTab >= m_Tabs.size())
        return nullptr;
    return &m_Tabs[m_ActiveTab];
}

Platform::IWebView* WebPanel::ActiveView()
{
    WebTab* tab = ActiveTab();
    return tab ? tab->view.get() : nullptr;
}

void WebPanel::MoveViewOffScreen(Platform::IWebView& view)
{
    view.SetPosition(kWebViewOffScreenX, kWebViewOffScreenY);
    view.SetVisible(true);
}

void WebPanel::OnPostLayout()
{
    float width = GetLayoutWidth();
    float height = GetLayoutHeight();
    bool isVisible = (width > 0.0f && height > 0.0f);

    if (Platform::IWebView* view = ActiveView())
    {
        if (m_WasVisible && !isVisible)
        {
            MoveViewOffScreen(*view);
        }
        else if (!m_WasVisible && isVisible)
        {
            if (WebTab* tab = ActiveTab(); tab && !tab->url.empty())
            {
                view->SetVisible(true);
            }
            if (m_WebViewContainer && m_BindApplied)
            {
                UpdateWebViewSize();
            }
        }
        else if (isVisible && m_WebViewContainer && m_BindApplied)
        {
            UpdateWebViewSize();
        }
    }

    m_WasVisible = isVisible;

    if (m_BindApplied || m_BindPending || m_BindFailed)
        return;
    if (!GetOwnerManager())
        return;

    m_BindPending = true;
    // A dropped action never runs, so it would never release the latch: release it here
    // instead and let the next laid-out frame try again.
    if (!this->PostAction([this]()
                          { this->BindFromAssetsDeferred(); }))
        m_BindPending = false;
}

void WebPanel::BindFromAssetsDeferred()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the
        // latch and let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    // m_BindPending stays set: LoadBindAttachLayoutAndStyle hands it to the async
    // load, whose completion is what resolves it — and is also where the element
    // pointers and the web view get set up, since the UXML children exist only then.
    LoadBindAttachLayoutAndStyle();
}

void WebPanel::MarkBindFailed(std::string_view reason)
{
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error(
        "WebPanel: layout bind failed ({}). The panel stays unbound; "
        "check that UI/panels/WebPanel.uxml is staged under the editor asset mount.",
        reason);
}

void WebPanel::LoadBindAttachLayoutAndStyle()
{
    // Loads are asynchronous: a blocking future.get() here would stall the UI thread,
    // and on the failure path it would do so once per layout pass.
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path layoutAssetPath = std::filesystem::path("UI") / "panels" / "WebPanel.uxml";
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "WebPanel.css";

    const GUID layoutGuid = am.ResolveAssetGuid(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    // The stylesheet is independent of the layout: a panel that fails to bind its
    // .uxml still wants its own styles on the root. The handle guard is redundant under
    // the once-latch and kept only so this reads identically to the other two panels.
    if (!styleGuid.IsNull() && !m_PanelStyleLoadHandle)
    {
        m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
            am.LoadAsset(styleGuid,
                         [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                                 return;
                             post.Post([this, styleGuid]()
                                              {
                                                  UIManager* ui = GetOwnerManager();
                                                  if (!ui)
                                                      return;
                                                  auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a = am2.GetAsset(styleGuid);
                                                  if (a && a->GetType() == AssetType::UIStyle)
                                                  {
                                                      (void)ui->AttachStyleToSubtreeFromAsset(
                                                          this, *static_cast<UIStyleAsset*>(a.get()));
                                                  }
                                              });
                         },
                         AssetLoadPriority::High));
    }

    if (layoutGuid.IsNull())
    {
        MarkBindFailed("the layout guid did not resolve from the editor asset source");
        return;
    }

    // Reached once per panel: OnPostLayout holds m_BindPending for the whole attempt,
    // so there is no second entry to guard against and every exit below resolves it.
    m_LayoutLoadHandle = std::make_unique<AssetLoadHandle>(
        am.LoadAsset(layoutGuid,
                     [this, post = GetPostHandle(), layoutGuid](Result<SharedPtr<Asset>, AssetError> r)
                     {
                         // The load completes on a worker; every latch mutation below
                         // runs on the UI thread through PostAction.
                         const bool loaded = r.IsOk() && r.Value() &&
                                             r.Value()->GetType() == AssetType::UILayout;
                         post.Post([this, layoutGuid, loaded]()
                                          {
                                              if (!loaded)
                                                  return MarkBindFailed("the layout asset did not load");
                                              UIManager* ui = GetOwnerManager();
                                              if (!ui || m_BindApplied)
                                              {
                                                  m_BindPending = false;
                                                  return;
                                              }
                                              auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                              auto a = am2.GetAsset(layoutGuid);
                                              if (!a || a->GetType() != AssetType::UILayout)
                                                  return MarkBindFailed("the loaded layout asset is not a UILayout");
                                              const bool ok = ui->BindLayoutToSubtreeChildrenFromAsset(
                                                  this, *static_cast<UILayoutAsset*>(a.get()));
                                              if (!ok)
                                                  return MarkBindFailed("binding the layout into the panel subtree was rejected");

                                              m_BindApplied = true;
                                              m_BindPending = false;
                                              // Success-only by construction, and that is not a
                                              // narrowing: both used to run on every deferred pass, but
                                              // RefreshElementPointers resolves ids the UXML has not yet
                                              // created and SetupWebView early-returns without
                                              // m_WebViewContainer. They did nothing until the bind
                                              // succeeded, which is exactly here.
                                              RefreshElementPointers();
                                              SetupWebView();
                                          });
                     },
                     AssetLoadPriority::High));
}

void WebPanel::RefreshElementPointers()
{
    m_Container = FindById("WebContainer");
    m_URLField = dynamic_cast<TextField*>(FindById("WebURLField"));
    m_TabBar = FindById("WebTabBar");
    m_WebViewContainer = FindById("WebViewContainer");
    m_ErrorLabel = dynamic_cast<Label*>(FindById("WebErrorLabel"));

    if (m_WebViewContainer && !m_WebViewContainer->HasClass("web-asset-drop-initialized"))
    {
        m_WebViewContainer->AddClass("web-asset-drop-initialized");

        // Handle asset drag from the Assets panel (g_DragState) and load images directly
        // into the embedded web view when dropped over the WebViewContainer area.
        m_WebViewContainer->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
        {
            if (e.Button != 0)
                return;

            if (!g_DragState.active || g_DragState.assetPath.empty())
                return;

            std::filesystem::path droppedPath = g_DragState.assetPath;

            // Clear drag state so other panels don't try to consume it again.
            g_DragState.active = false;
            g_DragState.assetPath.clear();
            g_DragState.scenePath.clear();
            g_DragState.entityId.clear();

            if (!std::filesystem::exists(droppedPath))
                return;

            std::string ext = droppedPath.extension().string();
            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Only handle common image formats for now.
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" &&
                ext != ".webp" && ext != ".gif" && ext != ".bmp")
            {
                return;
            }

            Platform::IWebView* view = ActiveView();
            if (!view)
                return;

            std::string full = droppedPath.lexically_normal().string();

            // Build a basic file:// URL. Browsers generally handle spaces and native
            // separators here; we just normalize slashes for consistency.
            for (char& ch : full)
            {
                if (ch == '\\')
                    ch = '/';
            }

            std::string fileUrl = "file://";
            if (!full.empty() && full[0] != '/')
            {
                // Ensure absolute-style path for POSIX-style URLs when needed.
                fileUrl += '/';
            }
            fileUrl += full;

            if (view->LoadURL(fileUrl))
            {
                if (m_ErrorLabel)
                    m_ErrorLabel->SetText("");
                Logger::Log::Info("WebPanel: Loaded dropped image in web view: {}", fileUrl);
                e.Stop();
            }
        });
    }

    if (m_URLField)
    {
        m_URLField->SetOnValueChanged([this](const std::string& value) {
            OnURLChanged(value);
        });

        m_URLField->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
            if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
            {
                OnLoadButtonClicked();
                e.Stop();
            }
        });
    }

    if (auto* newTabButton = dynamic_cast<Button*>(FindById("WebNewTabButton")))
    {
        newTabButton->SetTooltip("New tab");
        newTabButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OpenBlankTab(); });
    }

    if (m_ErrorLabel)
    {
        m_ErrorLabel->SetText("");
    }
}

void WebPanel::OnURLChanged(const std::string& /*url*/)
{
    if (m_ErrorLabel)
    {
        m_ErrorLabel->SetText("");
    }
}

namespace
{
std::string NormalizeGenericURL(std::string url)
{
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
    };
    trim(url);
    if (url.empty())
        return "";
    if (url.find("http://") == 0 || url.find("https://") == 0)
        return url;
    return "https://" + url;
}

std::string TabTitleForURL(const std::string& url)
{
    if (url.rfind(GameEngine::WebPanel::kHelpDocsURL, 0) == 0)
        return "Docs";
    if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos)
        return "YouTube";

    // Host without scheme: "https://www.example.com/path" -> "www.example.com"
    std::string host = url;
    if (auto pos = host.find("://"); pos != std::string::npos)
        host.erase(0, pos + 3);
    if (auto pos = host.find('/'); pos != std::string::npos)
        host.erase(pos);
    return host.empty() ? "New Tab" : host;
}
} // namespace

void WebPanel::OnLoadButtonClicked()
{
    if (!m_URLField)
        return;

    std::string url = m_URLField->GetValue();
    if (url.empty())
        return;

    LoadURL(url);

    // Remember the page the user chose so the panel reopens on it next session.
    if (WebTab* tab = ActiveTab(); tab && !tab->url.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        prefs.SetString(kLastURLPrefKey, tab->url);
        (void)prefs.Save(&err);
    }
}

void WebPanel::Navigate(const std::string& url)
{
    if (m_Tabs.empty())
    {
        // Web views are created after the first layout (SetupWebView); load then.
        m_PendingURL = url;
        if (m_URLField)
            m_URLField->SetValueWithoutNotify(url);
        return;
    }

    // A tab already on this URL (or a page under it) just gets activated.
    for (size_t i = 0; i < m_Tabs.size(); ++i)
    {
        if (!m_Tabs[i].url.empty() && m_Tabs[i].url.rfind(url, 0) == 0)
        {
            ActivateTab(i);
            return;
        }
    }

    // Never replace a page the user loaded: only a blank active tab is reused.
    WebTab* active = ActiveTab();
    if (!active || !active->url.empty())
    {
        const size_t tabCountBefore = m_Tabs.size();
        OpenBlankTab();
        if (m_Tabs.size() == tabCountBefore)
            return; // web view creation failed; error label already set
    }

    if (m_URLField)
        m_URLField->SetValueWithoutNotify(url);
    LoadURL(url);
}

void WebPanel::LoadURL(const std::string& url)
{
    WebTab* tab = ActiveTab();
    if (!tab || !tab->view)
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Web view not available");
        Logger::Log::Error("WebPanel: Web view not initialized");
        return;
    }

    std::string embedURL = ConvertToEmbedURL(url);
    if (!embedURL.empty())
    {
        LoadYouTubeVideo(embedURL);
        return;
    }

    std::string normalized = NormalizeGenericURL(url);
    if (normalized.empty())
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Invalid URL");
        Logger::Log::Warning("WebPanel: Invalid URL: {}", url);
        return;
    }

    // Load first so native view sets dark background before showing (reduces white flash)
    if (tab->view->LoadURL(normalized))
    {
        tab->url = normalized;
        tab->title = TabTitleForURL(normalized);
        tab->view->SetVisible(true);
        UpdateWebViewSize();
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("");
        Logger::Log::Info("WebPanel: Loading URL: {}", normalized);
    }
    else
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Failed to load URL.");
        Logger::Log::Error("WebPanel: Failed to load URL: {}", normalized);
    }
    RebuildTabBar();
}

void WebPanel::LoadYouTubeVideo(const std::string& embedURL)
{
    WebTab* tab = ActiveTab();
    if (!tab || !tab->view)
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Web view not available");
        return;
    }

    std::string watchURL = ConvertEmbedToWatchURL(embedURL);
    if (watchURL.empty())
        watchURL = embedURL;

    Logger::Log::Info("WebPanel: Loading YouTube in dark wrapper: {}", watchURL);

    std::string htmlWrapper = R"(<!DOCTYPE html>
<html style="background-color: #272727 !important; margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden;">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta name="referrer" content="strict-origin-when-cross-origin">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        html, body {
            background-color: #272727 !important;
            width: 100%;
            height: 100%;
            overflow: hidden;
        }
        iframe {
            width: 100%;
            height: 100%;
            border: none;
            background-color: #272727;
        }
    </style>
</head>
<body style="background-color: #272727 !important; margin: 0; padding: 0; width: 100%; height: 100%;">
    <iframe src=")" + watchURL + R"("
            style="width: 100%; height: 100%; border: none; background-color: #272727;"
            referrerpolicy="strict-origin-when-cross-origin"
            allowfullscreen
            allow="autoplay; encrypted-media; picture-in-picture">
    </iframe>
</body>
</html>)";

    tab->view->SetVisible(true);
    if (tab->view->LoadHTML(htmlWrapper, "https://www.youtube.com"))
    {
        tab->url = embedURL;
        tab->title = TabTitleForURL(embedURL);
        UpdateWebViewSize();
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("");
        Logger::Log::Info("WebPanel: Successfully loading YouTube in dark wrapper");
    }
    else
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Failed to load YouTube page. Please check the URL.");
        Logger::Log::Error("WebPanel: Failed to load HTML wrapper");
    }
    RebuildTabBar();
}

std::string WebPanel::ConvertToEmbedURL(const std::string& url)
{
    if (url.empty())
        return "";

    if (url.find("youtube.com/embed/") != std::string::npos || url.find("youtube-nocookie.com/embed/") != std::string::npos)
    {
        return url;
    }

    std::string videoId;
    std::string timestamp;

    std::regex watchPattern(R"(youtube\.com/watch\?v=([a-zA-Z0-9_-]{11}))");
    std::smatch match;
    if (std::regex_search(url, match, watchPattern))
    {
        videoId = match[1].str();
        std::regex timePattern(R"([?&]t=([0-9]+[hms]?)+)");
        std::smatch timeMatch;
        if (std::regex_search(url, timeMatch, timePattern))
        {
            timestamp = timeMatch[1].str();
        }
    }
    else
    {
        std::regex shortPattern(R"(youtu\.be/([a-zA-Z0-9_-]{11}))");
        if (std::regex_search(url, match, shortPattern))
        {
            videoId = match[1].str();
            std::regex timePattern(R"([?&]t=([0-9]+[hms]?)+)");
            std::smatch timeMatch;
            if (std::regex_search(url, timeMatch, timePattern))
            {
                timestamp = timeMatch[1].str();
            }
        }
        else
        {
            std::regex watchWithParamsPattern(R"([?&]v=([a-zA-Z0-9_-]{11}))");
            if (std::regex_search(url, match, watchWithParamsPattern))
            {
                videoId = match[1].str();
                std::regex timePattern(R"([?&]t=([0-9]+[hms]?)+)");
                std::smatch timeMatch;
                if (std::regex_search(url, timeMatch, timePattern))
                {
                    timestamp = timeMatch[1].str();
                }
            }
        }
    }

    if (videoId.empty())
        return "";

    std::ostringstream embedURL;
    embedURL << "https://www.youtube.com/embed/" << videoId
             << "?autoplay=1"
             << "&rel=0"
             << "&origin=https://www.youtube.com"
             << "&enablejsapi=1"
             << "&playsinline=1";

    if (!timestamp.empty())
    {
        embedURL << "&start=" << ConvertTimestampToSeconds(timestamp);
    }

    return embedURL.str();
}

std::string WebPanel::ConvertEmbedToWatchURL(const std::string& embedURL)
{
    if (embedURL.empty())
        return "";

    std::regex embedPattern(R"(youtube\.com/embed/([a-zA-Z0-9_-]{11}))");
    std::smatch match;

    if (std::regex_search(embedURL, match, embedPattern))
    {
        std::string videoId = match[1].str();
        std::regex startPattern(R"([?&]start=(\d+))");
        std::smatch startMatch;
        std::string timestampParam = "";

        if (std::regex_search(embedURL, startMatch, startPattern))
        {
            int seconds = std::stoi(startMatch[1].str());
            timestampParam = "&t=" + std::to_string(seconds) + "s";
        }

        return "https://www.youtube.com/watch?v=" + videoId + timestampParam;
    }

    if (embedURL.find("youtube.com/watch") != std::string::npos)
    {
        return embedURL;
    }

    return "";
}

int WebPanel::ConvertTimestampToSeconds(const std::string& timestamp)
{
    int totalSeconds = 0;

    std::regex hourPattern(R"((\d+)h)");
    std::smatch hourMatch;
    if (std::regex_search(timestamp, hourMatch, hourPattern))
    {
        totalSeconds += std::stoi(hourMatch[1].str()) * 3600;
    }

    std::regex minPattern(R"((\d+)m)");
    std::smatch minMatch;
    if (std::regex_search(timestamp, minMatch, minPattern))
    {
        totalSeconds += std::stoi(minMatch[1].str()) * 60;
    }

    std::regex secPattern(R"((\d+)s)");
    std::smatch secMatch;
    if (std::regex_search(timestamp, secMatch, secPattern))
    {
        totalSeconds += std::stoi(secMatch[1].str());
    }
    else
    {
        std::regex numPattern(R"(\d+)");
        std::smatch numMatch;
        if (std::regex_search(timestamp, numMatch, numPattern))
        {
            totalSeconds = std::stoi(numMatch[0].str());
        }
    }

    return totalSeconds;
}

void WebPanel::UpdateWebViewSize()
{
    Platform::IWebView* view = ActiveView();
    if (!view || !m_WebViewContainer)
        return;

    float x = m_WebViewContainer->GetLayoutX();
    float y = m_WebViewContainer->GetLayoutY();
    float width = m_WebViewContainer->GetLayoutWidth();
    float height = m_WebViewContainer->GetLayoutHeight();

    // GetLayout* returns CSS-logical pixels (Yoga's space, shrunk by Additional UI Scale).
    // The native web view expects window client-space pixels, so undo the multiplier.
    const float scale = WindowInputRouter::UiLogicalToClientScale(m_ParentWindow, m_WebViewContainer->GetOwnerManager());
    if (width > 0 && height > 0)
    {
        view->Resize(static_cast<int>(std::lround(width * scale)), static_cast<int>(std::lround(height * scale)));
        view->SetPosition(static_cast<int>(std::lround(x * scale)), static_cast<int>(std::lround(y * scale)));
    }
}

void WebPanel::SetWebViewOffScreen(bool offScreen)
{
    if (offScreen)
    {
        for (auto& tab : m_Tabs)
        {
            if (tab.view)
                MoveViewOffScreen(*tab.view);
        }
        return;
    }

    // Inactive tabs stay off-screen; only the active tab returns to the container.
    if (Platform::IWebView* view = ActiveView())
    {
        view->SetVisible(true);
        if (m_WebViewContainer && m_BindApplied)
            UpdateWebViewSize();
    }
}

bool WebPanel::CreateTabWebView(WebTab& tab)
{
    if (!m_ParentWindow)
        return false;

    tab.view = Platform::CreateWebView(m_ParentWindow);
    if (!tab.view || !tab.view->GetNativeView())
    {
        tab.view.reset();
        return false;
    }

    tab.view->SetBackgroundColor(39, 39, 39);
    // Load minimal dark page so clicking in content area never shows white (blank default)
    const char* darkPlaceholder = R"(<!DOCTYPE html><html><head><meta charset="UTF-8"></head>)"
        R"(<body style="margin:0;background:#272727;width:100%;height:100%;"></body></html>)";
    tab.view->LoadHTML(darkPlaceholder, "about:blank");
    tab.view->SetPosition(kWebViewOffScreenX, kWebViewOffScreenY);
    tab.view->SetVisible(false);

    // Clicking into the page should defocus engine UI (e.g. the URL field).
    tab.view->SetOnMouseDown([this]()
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearFocus();
    });
    return true;
}

void WebPanel::OpenBlankTab()
{
    WebTab tab;
    if (!CreateTabWebView(tab))
    {
        if (m_ErrorLabel)
            m_ErrorLabel->SetText("Web view not available");
        return;
    }
    m_Tabs.push_back(std::move(tab));
    ActivateTab(m_Tabs.size() - 1);
}

void WebPanel::ActivateTab(size_t index)
{
    if (index >= m_Tabs.size())
        return;

    m_ActiveTab = index;
    for (size_t i = 0; i < m_Tabs.size(); ++i)
    {
        if (i != index && m_Tabs[i].view)
            MoveViewOffScreen(*m_Tabs[i].view);
    }

    WebTab& tab = m_Tabs[index];
    if (m_URLField)
        m_URLField->SetValueWithoutNotify(tab.url);
    if (tab.view)
    {
        if (!tab.url.empty())
            tab.view->SetVisible(true);
        UpdateWebViewSize();
    }
    RebuildTabBar();
}

void WebPanel::CloseTab(size_t index)
{
    if (index >= m_Tabs.size() || m_Tabs.size() < 2)
        return;

    if (m_Tabs[index].view)
    {
        m_Tabs[index].view->Destroy();
        m_Tabs[index].view.reset();
    }
    m_Tabs.erase(m_Tabs.begin() + static_cast<std::ptrdiff_t>(index));

    if (index < m_ActiveTab)
        --m_ActiveTab;
    ActivateTab(std::min(m_ActiveTab, m_Tabs.size() - 1));
}

void WebPanel::RebuildTabBar()
{
    if (!m_TabBar)
        return;

    m_TabBar->RemoveAllChildren();

    // A single page needs no tab strip; the "+" button lives in the URL bar.
    if (m_Tabs.size() < 2)
    {
        if (!m_TabBar->HasClass("hidden"))
            m_TabBar->AddClass("hidden");
        m_TabBar->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        return;
    }
    m_TabBar->RemoveClass("hidden");

    const bool showClose = m_Tabs.size() > 1;
    for (size_t i = 0; i < m_Tabs.size(); ++i)
    {
        auto tabEl = std::make_unique<UIElement>();
        tabEl->AddClass("web-tab");
        if (i == m_ActiveTab)
            tabEl->AddClass("active");

        auto label = std::make_unique<Button>();
        label->AddClass("web-tab-label");
        label->SetText(m_Tabs[i].title.empty() ? "New Tab" : m_Tabs[i].title);
        label->RegisterEventHandler(kEventButtonClick, [this, i](UIEvent&) { ActivateTab(i); });
        tabEl->AddChild(std::move(label));

        if (showClose)
        {
            // Icon-based close (xclose-icon): background-image tint hovers repaint
            // reliably, unlike text-color :hover, and need no baseline alignment.
            auto close = std::make_unique<Button>();
            close->AddClass("web-tab-close");
            close->AddClass("xclose-icon");
            close->SetTooltip("Close tab");
            close->RegisterEventHandler(kEventButtonClick, [this, i](UIEvent&) { CloseTab(i); });
            tabEl->AddChild(std::move(close));
        }

        m_TabBar->AddChild(std::move(tabEl));
    }

    m_TabBar->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void WebPanel::SetWindow(Platform::Window* window)
{
    if (m_ParentWindow == window)
        return;
    m_ParentWindow = window;
    // The bind that runs when the panel is first shown may already have looked
    // for a window and found none — ShowOrActivateWebPanel shows the panel and
    // only afterwards hands it one. Nothing else calls SetupWebView a second
    // time, so without this the panel stays empty for the rest of the session.
    if (m_ParentWindow)
        SetupWebView();
}

void WebPanel::SetupWebView()
{
    // Both of these are ordinary not-ready-yet states rather than failures:
    // the bind and the window arrive in either order, and whichever lands
    // second re-enters here.
    if (!m_ParentWindow || !m_WebViewContainer)
        return;

    if (!m_Tabs.empty())
        return;

    WebTab tab;
    if (!CreateTabWebView(tab))
    {
        if (m_ErrorLabel)
        {
            m_ErrorLabel->SetText("Web view not supported on this platform");
        }
        Logger::Log::Warning("WebPanel: Failed to create web view");
        return;
    }

    m_Tabs.push_back(std::move(tab));
    m_ActiveTab = 0;
    UpdateWebViewSize();

    std::string startURL = m_PendingURL;
    m_PendingURL.clear();
    if (startURL.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        (void)prefs.Load(&err);
        (void)prefs.TryGetString(kLastURLPrefKey, startURL);
    }
    if (startURL.empty())
        startURL = kHelpDocsURL;
    if (m_URLField)
        m_URLField->SetValueWithoutNotify(startURL);
    LoadURL(startURL);
}

} // namespace GameEngine
