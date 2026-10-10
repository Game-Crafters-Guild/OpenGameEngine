#pragma once

#include <string>
#include <unordered_map>

namespace GameEngine
{

class Label;
class PolyhavenDownloadManager;
class UIElement;
namespace ECS { class World; }

// Screen-space download progress pills for Polyhaven model downloads. One pill
// follows the cursor while a not-yet-downloaded model is dragged over the
// scene view; after a drop, every in-flight placeholder download gets its own
// pill, horizontally centred under the scene view's compile banner, until its
// model replaces the placeholder.
//
// Owned by PolyhavenDownloadManager, which reads progress on its behalf and
// ticks Update() from its main-thread poll. The scene view only publishes the
// host element the pills are parented to and forwards the drag positions it
// receives; it holds no pill state.
class DownloadPillOverlay
{
public:
    explicit DownloadPillOverlay(const PolyhavenDownloadManager& downloads);

    DownloadPillOverlay(const DownloadPillOverlay&) = delete;
    DownloadPillOverlay& operator=(const DownloadPillOverlay&) = delete;

    // Element new pills are parented to, and the banner the post-drop stack
    // starts under (its laid-out height is read every Update, so the offset
    // tracks the banner's CSS). Pills already built stay under their parent.
    void SetHost(UIElement* host, const UIElement* stackBanner);

    // Drag-time pill at the cursor (host-local), trailing it by a small offset.
    // Progress for `slug` is read from the download manager.
    void ShowDragPill(const std::string& slug, const std::string& name, float hostX, float hostY);
    // Retires the drag pill; the post-drop stack resumes on the next Update.
    void HideDragPill();

    // Display name for a dropped download's pill. A drop that records none
    // (the hierarchy panel's) shows the slug.
    void SetDisplayName(const std::string& slug, const std::string& name);

    // Builds, re-flows and retires the post-drop pills against the manager's
    // active placeholder downloads. Main thread, outside UI dispatch, so
    // retired pills are removed from the host here.
    void Update(const ECS::World* world);

private:
    struct Widgets
    {
        UIElement* Pill = nullptr; // non-owning; the host owns the element
        UIElement* Image = nullptr;
        Label* Text = nullptr;
        UIElement* Fill = nullptr;
        std::string ImageSlug;   // slug whose thumbnail is currently applied
        int LastPercent = -1000; // last percent written, to skip no-op updates
        int LastRow = -1;        // stack row last applied, so a re-flow repositions
        float LastTopY = -1.0f;  // stack origin last applied (tracks the compile banner)
        float LastLeftX = -1.0f; // centred left edge last applied (tracks the pill's width)
    };

    // Build the pill element tree under the host and fill `out` with the child views.
    void Build(const std::string& id, Widgets& out);
    static void ApplyState(Widgets& w, const std::string& name, float hostX, float hostY,
                           bool hasFraction, float fraction);
    // Apply the asset's cached thumbnail (or collapse the image when none exists).
    // Re-applies only on slug change, so it is cheap to call every frame.
    static void ApplyImage(Widgets& w, const std::string& slug);
    static void Hide(Widgets& w);

    const PolyhavenDownloadManager& m_Downloads;
    UIElement* m_Host = nullptr;               // not owned
    const UIElement* m_StackBanner = nullptr;  // not owned
    // Single drag-time pill, anchored at the cursor.
    Widgets m_DragPill;
    // True between ShowDragPill and HideDragPill: the drag owns the overlay and
    // the post-drop stack is left as it is.
    bool m_DragPillShown = false;
    // Post-drop pills, one per in-flight placeholder download (keyed by slug).
    // Each retires when the slug leaves the download manager's active list.
    std::unordered_map<std::string, Widgets> m_PostDropPills;
    std::unordered_map<std::string, std::string> m_DisplayNames;
};

} // namespace GameEngine
