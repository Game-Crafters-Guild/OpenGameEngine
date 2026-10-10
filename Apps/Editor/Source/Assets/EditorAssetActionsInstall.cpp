#include "Assets/EditorAssetActionsInstall.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Editor/Assets/EditorAssetActions.h"
#include "EditorContextMenu/EditorContextMenu.h"
#include "EditorPanelIds.h"
#include "Panels/AssetViewPanel.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/UIElement.h"
#include "UI/UiPostHandle.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// The thumbnail service names a live render it draws over frames "engine:<name>". For a
// material or a model with no cached picture yet that render is all it has, and in a slot the
// Asset View preview shares it is drawn at the preview's shape, a dot in a small square.
constexpr std::string_view kLivePrefix = "engine:";
// How long a thumbnail waits for the cached picture before it shows the live render instead. In
// time, not frames: an idle editor draws hundreds of UI frames a second.
constexpr std::chrono::seconds kPendingLimit{30};
// How often a waiting thumbnail asks for the cached picture again.
constexpr std::chrono::milliseconds kRecheckInterval{250};

bool IsLive(const std::string& image)
{
    return std::string_view(image).starts_with(kLivePrefix);
}

// `image`, an answer of the thumbnail service (absolute, or relative to the assets root), is the
// file `asset` itself: its stand-in while a downscaled copy is made.
bool IsSourceItself(const std::string& image, const std::filesystem::path& asset)
{
    std::filesystem::path path = std::filesystem::path(std::u8string(image.begin(), image.end()));
    if (path.is_relative())
        if (const AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager())
            path = assets->GetAssetRoot() / path;
    std::error_code error;
    return std::filesystem::equivalent(path, asset, error) && !error;
}

// One element's thumbnail while it waits for a picture the service has cached. Lives in the
// actions posted to the element's UI thread; the post handle drops them with the element.
struct PendingThumbnail
{
    IThumbnailProvider* Thumbnails = nullptr;
    uint64_t WindowId = 0;
    UIElement* Element = nullptr;
    UI::UiPostHandle Post;
    std::filesystem::path Asset;
    int SizePx = 0;
    // The live render's name, without kLivePrefix.
    std::string Live;
    std::chrono::steady_clock::time_point Since = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point LastCheck = std::chrono::steady_clock::now();
};

void Apply(UIElement& element, const std::string& image)
{
    ApplyThumbnailImage(element, image);
    element.RemoveClass(kThumbnailPendingClass);
}

// One UI frame of waiting. Between rechecks it only reads the clock and posts itself again;
// every kRecheckInterval it keeps the live render drawing (so the service can cache it) and
// asks for the cached picture, and after kPendingLimit it shows the live render if it is
// drawing, or marks the element kThumbnailUnavailableClass.
void WaitOneFrame(const std::shared_ptr<PendingThumbnail>& pending)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - pending->LastCheck >= kRecheckInterval)
    {
        pending->LastCheck = now;
        pending->Thumbnails->EnsureEngineThumbnailRequested(pending->WindowId, pending->Live);
        const std::string cached = pending->Thumbnails->GetOrRequest(pending->Asset, pending->SizePx, nullptr);
        if (!cached.empty() && !IsLive(cached))
        {
            Apply(*pending->Element, cached);
            return;
        }
    }
    if (now - pending->Since >= kPendingLimit)
    {
        // A live render that is drawing still beats nothing; one whose slot was given up (the
        // Asset View's preview took it) would draw an empty well, so the element says so.
        if (pending->Thumbnails->IsEngineThumbnailReadyForWindow(pending->WindowId, pending->Live))
        {
            Apply(*pending->Element, std::string(kLivePrefix) + pending->Live);
            return;
        }
        pending->Element->RemoveClass(kThumbnailPendingClass);
        pending->Element->AddClass(kThumbnailUnavailableClass);
        return;
    }
    pending->Post.Post([pending]() { WaitOneFrame(pending); });
}

// The service's answer for `element`, on its UI thread: a picture is shown; a live render is
// waited on (WaitOneFrame) with the element marked kThumbnailPendingClass.
void Receive(IThumbnailProvider& thumbnails, uint64_t windowId, UIElement& element, const std::filesystem::path& asset,
             int sizePx, const std::string& image)
{
    if (!IsLive(image))
    {
        Apply(element, image);
        return;
    }
    auto pending = std::make_shared<PendingThumbnail>();
    pending->Thumbnails = &thumbnails;
    pending->WindowId = windowId;
    pending->Element = &element;
    pending->Post = element.GetPostHandle();
    pending->Asset = asset;
    pending->SizePx = sizePx;
    pending->Live = image.substr(kLivePrefix.size());
    element.AddClass(kThumbnailPendingClass);
    // The first frame asks for the live render at once.
    pending->Thumbnails->EnsureEngineThumbnailRequested(windowId, pending->Live);
    WaitOneFrame(pending);
}

void ShowThumbnail(IThumbnailProvider& thumbnails, uint64_t windowId, UIElement& element,
                   const std::filesystem::path& asset, int sizePx)
{
    element.AddClass(kThumbnailPendingClass);
    // The service answers a miss later, possibly on a worker; the element's post handle brings the
    // answer back to its UI thread and drops it when the element is gone.
    const std::string immediate = thumbnails.GetOrRequest(
        asset, sizePx,
        [&thumbnails, windowId, post = element.GetPostHandle(), element = &element, asset,
         sizePx](const std::string& image) {
            if (!image.empty())
                post.Post([&thumbnails, windowId, element, asset, sizePx, image]() {
                    Receive(thumbnails, windowId, *element, asset, sizePx, image);
                });
        });
    // An image file larger than the request is answered at once with the file itself while its
    // downscaled copy is made; showing it would load the full-size image, so the copy waits for
    // the callback (which also brings the file itself when it is small enough to show as is).
    if (!immediate.empty() && !IsSourceItself(immediate, asset))
        Receive(thumbnails, windowId, element, asset, sizePx, immediate);
}

// The service's answer for an inline image, on the element's UI thread.
void ReceiveImage(UIElement& element, const std::string& image)
{
    element.RemoveClass(kThumbnailPendingClass);
    if (image.empty())
    {
        element.AddClass(kThumbnailUnavailableClass);
        return;
    }
    ApplyThumbnailImage(element, image);
}

void ShowImage(IThumbnailProvider& thumbnails, UIElement& element, const std::filesystem::path& image)
{
    element.AddClass(kThumbnailPendingClass);
    // Every answer, a cached one included, comes back through the element's post handle, which
    // drops it when the element is gone.
    thumbnails.GetOrRequestInlineImage(
        image, [post = element.GetPostHandle(), element = &element](const std::string& picture) {
            post.Post([element, picture]() { ReceiveImage(*element, picture); });
        });
}

void Preview(const EditorAssetActionsHost& host, const std::filesystem::path& image)
{
    if (!host.AssetView)
        return;
    if (host.Panels)
        host.Panels->ShowOrActivatePanel(host.Window, EditorPanelIds::AssetView);
    host.AssetView->SetAssetPreview(image, true);
}

} // namespace

void InstallEditorAssetActions(EditorAssetActionsHost host)
{
    EditorAssetActions actions;
    if (IThumbnailProvider* thumbnails = host.Thumbnails)
    {
        actions.ShowThumbnail = [thumbnails, windowId = host.ThumbnailWindowId](
                                    UIElement& element, const std::filesystem::path& asset, int sizePx) {
            ShowThumbnail(*thumbnails, windowId, element, asset, sizePx);
        };
        actions.ShowImage = [thumbnails](UIElement& element, const std::filesystem::path& image) {
            ShowImage(*thumbnails, element, image);
        };
    }
    actions.Open = host.Open;
    actions.Preview = [host](const std::filesystem::path& image) { Preview(host, image); };
    actions.ShowInFileManager = &EditorContextMenu::ShowInFileManager;
    actions.CopyPath = &EditorContextMenu::CopyPathToClipboard;
    SetEditorAssetActions(std::move(actions));
}

void UninstallEditorAssetActions()
{
    SetEditorAssetActions({});
}

} // namespace GameEngine::Editor
