#pragma once

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h" // AssetLoadHandle
#include "Mathematics/Vector2.h"
#include "UI/UITargetSpace.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unordered_map>

// JobSystem is a TOP-LEVEL namespace (::JobSystem), NOT GameEngine::JobSystem —
// forward-declaring it inside GameEngine would shadow the real one and break
// every JobSystem::TaskHandle reference in the ECS Query templates.
namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine {

namespace ECS { class World; }
namespace Rendering {
class IDevice;
namespace RenderGraph {
class RGFrame;
struct RGTexture;
}
} // namespace Rendering
class UIManager;
class UIElement;

// Owns ONE game UIManager and reconciles its subtree set from the UIDocument
// components in an ECS world. Each UIDocument entity maps to one direct child of
// the UI root (z-index = SortOrder); the child's subtree is bound from the
// document's .uxml + .css. Host-agnostic: the editor (per Game View session) and
// the standalone Player both own one and drive it — SyncFromWorld() each frame,
// Update(dt, extent), then RenderRG(frame, target, targetSpace) onto the scene's
// FinalColor.
//
// Reconcile is render-time (NOT an ECS system): a UIManager is device-bound,
// per-host state the ECS schedule has no handle to. The host already owns this
// object and ticks it.
class GameUIHost
{
  public:
    /// The default game UI font, relative to the asset root ConfigureFonts reads it from.
    /// A packaged game ships it as an asset manifest entry (AssetCollector::CollectDefaultUIFont).
    static constexpr const char* kDefaultFontPath = "Fonts/Roboto-Regular.ttf";

    GameUIHost(Rendering::IDevice* device, AssetManager* assetManager,
               JobSystem::WorkStealingThreadPool* jobSystem);
    ~GameUIHost();
    GameUIHost(const GameUIHost&) = delete;
    GameUIHost& operator=(const GameUIHost&) = delete;

    // Build a host for one integration site: construct, install the default UI
    // font from `fontsDir`, and record whether it composites onto a target it
    // doesn't own. One factory so the bootstrap can't drift across the three
    // call sites (Player, editor Game View, editor Scene View). `fontsDir` is
    // the root containing Fonts/Roboto-Regular.ttf — the project asset root for a
    // shipping game, the staged editor Assets for an editor preview.
    static std::unique_ptr<GameUIHost> CreateForHost(
        Rendering::IDevice* device, AssetManager* assetManager,
        JobSystem::WorkStealingThreadPool* jobSystem,
        const std::filesystem::path& fontsDir, bool neverClearTarget);

    struct PointerState
    {
        float X = 0.0f;
        float Y = 0.0f;
        bool PrimaryDown = false;
        // Snapshot for this pointer event, using Input::kMod* bits.
        int Modifiers = 0;
    };

    // Per-frame phase 1: reconcile subtrees from the world's UIDocument set.
    // Returns whether any documents exist this frame, BEFORE any pass is
    // declared — a host whose frame topology depends on the HUD existing
    // (the Player's SDR encoded-blend chain, #767 slice iii) syncs first,
    // declares its passes, then calls UpdateAndRender. Null world = keep the
    // current subtree set.
    bool SyncDocuments(ECS::World* world);

    // Input-phase delivery for a source that samples the pointer rather than
    // receiving its edges, in composite-target pixels: the host differences the
    // sample against the last one and calls the event entries below. Forward
    // every sample; nullptr says no pointer is over the surface and cancels the
    // gesture from the host's own button state. Rebinding while pressed requires
    // a release before another press can arm replacement controls.
    void UpdatePointer(const PointerState* pointer);

    // Pointer delivery for a source that has the events themselves — a router
    // whose window is the surface. Same composite-target pixels, same
    // synchronous dispatch, and no render or layout tick needed.
    //
    // A move is never consumed: hover is not a claim. A button transition and a
    // wheel tick answer whether a HUD control acted, and that answer is what
    // keeps a click the HUD took from reaching the game as well. Every button
    // arrives, not just the primary; only the primary carries the gesture state
    // a cancel has to unwind.
    void OnMouseMove(Mathematics::Vector2 position);
    bool OnMouseButton(int button, bool pressed, int mods);
    bool OnScroll(Mathematics::Vector2 delta);

    // The source's primary button at the moment a gesture is cancelled. Held makes
    // the host swallow the release that follows, so raw MouseUp handlers never see
    // an orphan release and no press can arm controls until the source lets go.
    enum class PrimaryButtonState : uint8_t
    {
        Released,
        Held
    };

    // Cancel without MouseUp while retaining the source's current button state.
    // An unavailable world/extent can acknowledge a release without dispatching
    // into its old UI; a held press cannot arm replacement controls on a move.
    void CancelPointer(PrimaryButtonState primary);

    // Keystroke delivery for a live play surface, in the same synchronous shape
    // as UpdatePointer: the host dispatches into its UIManager and answers
    // whether a HUD control acted on the event. That answer is what stops the
    // keystroke before gameplay, so a focused chat box takes the typing that
    // would otherwise walk the character.
    //
    // Keys follow focus, not the cursor: the host turns the manager's hovered-key
    // fallback off, so a HUD control the pointer merely rests on is never offered
    // a key, and a focused field keeps taking typing after the pointer leaves it.
    // There is therefore no over-the-surface gate, and with nothing focused the
    // answer is false and the keystroke travels on to the game.
    //
    // Escape ends the surface's focus, whatever held it. A chrome field can be
    // clicked away from; a HUD field the player cannot leave keeps every
    // movement key. The answer is still the focused control's — an Escape
    // nothing acted on travels on to the game.
    bool OnKey(int key, int action, int mods);

    // The character half of a keystroke. Its producing key is offered first and
    // gated on the same answer, so a control that claimed the key never sees the
    // character a second time.
    bool OnChar(unsigned int codepoint);

    // Drop modifier state derived from key events — the keyboard counterpart of
    // CancelPointer, for a surface that stops receiving keys (window focus loss)
    // and would otherwise believe a modifier whose release it never saw is still
    // held.
    void ResetKeyboardState();

    // True when a ready Fullscreen document owns this host's target: the composite
    // clears it, so whatever would have been rendered behind the menu is invisible and
    // the host that owns the target can skip producing it entirely. False for a host
    // that composites onto a target it doesn't own (the editor Scene View preview),
    // which never clears and therefore never covers anything.
    //
    // This is RenderRG's Clear condition, so a caller that drops the content behind the
    // composite is always paired with a composite that clears — the two can't drift
    // into a frame that skips the scene and then blends the UI over its leftovers.
    // Reads the live subtree set: call it after SyncDocuments has run for the frame.
    bool CoversTargetOpaque() const;

    // Per-frame phase 2: lay out at the target extent and composite onto target.
    // Declares no pass at all when no documents exist. The animation delta is
    // read from the global Time clock (Time::GetDeltaTime), not passed in.
    // `targetSpace` is the host's required declaration of the attachment's
    // colour space (see UIManager::RenderRG) — the scene FinalColor these
    // hosts composite onto carries its pipeline's output space, and an SDR
    // Player frame declares EncodedSrgb against the pre-encoded composite.
    //
    // The pointer arrives in the input phase, through UpdatePointer or the event
    // entries above; rendering neither resamples it nor synthesizes button edges.
    void UpdateAndRender(Rendering::RenderGraph::RGFrame& frame,
                         Rendering::RenderGraph::RGTexture target, UI::UITargetSpace targetSpace,
                         uint32_t extentW, uint32_t extentH);

    // Both phases in one call, for hosts whose frame topology does not depend
    // on the sync result (editor Game View / Scene View previews).
    void SyncAndRender(ECS::World* world, Rendering::RenderGraph::RGFrame& frame,
                       Rendering::RenderGraph::RGTexture target, UI::UITargetSpace targetSpace,
                       uint32_t extentW, uint32_t extentH);

    UIManager* GetUIManager() const { return m_UI.get(); }

    // Per-entity subtree root for a live UIDocument, or nullptr if unbound.
    UIElement* FindDocumentRoot(uint64_t entityId) const;

    // Element `id` inside that document's mounted subtree, or nullptr.
    UIElement* FindElementById(uint64_t entityId, std::string_view id);

    // First match of `id` across mounted documents. Shared ids resolve to the
    // lowest entity id so the winner is stable. On a match, outOwnerEntityId (if
    // non-null) receives the owning UIDocument entity, so a caller that searched
    // every document can pin later lookups to that one subtree instead of
    // re-walking all of them. Left untouched when nothing matches.
    //
    // An EMPTY id matches the first element that has no id, which is most of a
    // tree — callers taking an id from outside must reject empty before calling.
    UIElement* FindElementByIdAny(std::string_view id, uint64_t* outOwnerEntityId = nullptr);

    // Bumps when a document layout binds, a subtree is removed, or a .uxml hot
    // reload reconciles a bound subtree. The reconcile bump comes from the
    // subtree's kEventLayoutReconciled, so it is visible the moment the reconcile
    // happens rather than at the next SyncFromWorld.
    //
    // A bump does NOT mean the old elements are gone. A subtree removal destroys
    // them; a .uxml reconcile REUSES the same UIElement object whenever the
    // template still declares that id with the same type, keeping its handler
    // table intact (UIHotReload.cpp ReconcileChildren). So a binder that
    // re-registers on every bump double-subscribes on preserved elements and
    // fires twice. Re-resolve on the bump and re-register only what actually
    // stopped resolving.
    uint64_t GetBindGeneration() const;

    // Discard every bound document subtree. The next SyncFromWorld rebuilds
    // them from UIDocument. Play-stop calls this via GameUI::NotifyGameplayStopped
    // so user-module callbacks die with the elements; edit hover is CSS on
    // the rebuilt tree.
    void ResetBoundDocuments();

    // Are any UIDocument subtrees bound right now? Public because a failed element lookup cannot
    // otherwise tell "no such id" from "nothing has ever been mounted on this host" — the second
    // is not a miss a caller can poll its way out of, and the two share one return code.
    bool HasAnyDocuments() const { return !m_Subtrees.empty(); }

    // Test-only seam onto the private layout/sync steps and SetPointer, so an
    // interactivity test can drive a headless host with no world and no RG render.
    // Input-phase integrations use UpdatePointer.
    friend struct GameUIHostTestAccess;

  private:
    // Construction-time setup, driven only by CreateForHost.

    // Install the default UI font + async resolver (mirrors the editor's font
    // bootstrap). assetsDir is the root containing Fonts/Roboto-Regular.ttf.
    void ConfigureFonts(const std::filesystem::path& assetsDir);

    // When true, RenderRG always composites with Load even for Fullscreen
    // documents — for a host compositing onto a target it doesn't own (the editor
    // Scene View preview overlays the shared scene+gizmos), so a Fullscreen menu
    // can't clear the editor viewport. Player/Game View own a dedicated FinalColor
    // and leave this false.
    void SetNeverClearTarget(bool neverClear) { m_NeverClearTarget = neverClear; }

    // Reconcile subtrees against the world's UIDocument set: add new, re-bind on
    // layout/style change, remove destroyed. Cheap when nothing changed (the
    // retained query skips the archetype rescan); async asset loads are polled +
    // guarded so a re-bind never double-applies.
    void SyncFromWorld(ECS::World& world);

    // Hide every Overlay document's container while a Fullscreen document is ready
    // to own the screen, and restore them when none is. Runs at the end of
    // SyncFromWorld, once the frame's subtree set is final.
    void ApplyFullscreenOverlaySuppression();

    // Feed a pointer sample, in composite-target (UI-space) pixels: the move
    // plus the primary button's edge, if the sample carries one. Calling this
    // opts the host into interactive Update (hover/press/click).
    void SetPointer(float x, float y, bool primaryDown);
    // A button edge against the modifier state this host already holds.
    bool DispatchMouseButton(int button, bool pressed);

    // True while a press is in flight that started on a document tree this host
    // has since replaced. The controls it armed are gone, so the gesture has to
    // be cancelled rather than resolved against whatever now sits under the
    // cursor.
    bool PointerTreeReplacedUnderPress() const;

    // The pointer stream ended for this surface (Game View viewport exit). Unlike a
    // window leave in the editor chrome — where the OS keeps delivering moves and the
    // real release to a captured element — no further pointer sample will ever reach
    // this UIManager, so an in-flight press has to be terminated here.
    void PointerLeftSurface();

    // Per-frame UI tick. extentW/H size the layout to the composite target.
    void Update(float deltaTime, uint32_t extentW, uint32_t extentH);

    // Declare the UI overlay pass onto `target`. Composites with Clear when an
    // enabled Fullscreen document is ready to own the target (e.g. a menu), else
    // Load (blends a HUD over the scene).
    bool RenderRG(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture target,
                  UI::UITargetSpace targetSpace);

    // True when an enabled Fullscreen document is READY to cover the target — its
    // layout is bound and not terminally failed. A Fullscreen document that is still
    // loading (or whose layout GUID is bad) has nothing to draw, so it must not win
    // the Clear: doing so blanks the frame for as long as it stays unbound.
    bool AnyReadyFullscreenSubtree() const;

    struct SubtreeState
    {
        UIElement* Root = nullptr; // the per-entity direct child of the UI root
        GUID LayoutGuid;           // last GUID we kicked a layout load for
        GUID StyleGuid;            // last GUID we kicked a style load for
        int32_t SortOrder = 0;
        bool LayoutApplied = false; // bind guard
        bool StyleApplied = false;
        bool LayoutFailed = false;  // terminal: wrong-type layout asset (stops the per-frame poll)
        bool StyleFailed = false;   // terminal: wrong-type style asset
        bool Fullscreen = false;    // doc.RenderMode == Fullscreen → contributes to the composite clear
        bool OverlayHidden = false; // Overlay doc currently suppressed by a ready Fullscreen doc
        AssetLoadHandle LayoutLoad; // keeps the loaded layout asset alive
        AssetLoadHandle StyleLoad;  // keeps the loaded style asset alive
        uint64_t SeenFrame = 0;     // mark-and-sweep for removal
    };

    using SubtreeMap = std::unordered_map<uint64_t /*EntityId*/, SubtreeState>;

    void RemoveSubtree(uint64_t entityId);

    // The only way a bound subtree leaves the host: drop any focus it holds, detach
    // its container, forget it, and bump the bind generation. Both removal paths —
    // a disabled document and the mark-and-sweep for an entity that went away — go
    // through here, so the focus and generation rules hold however a document ends.
    // Returns the iterator following the erased entry, so a sweep can keep walking.
    SubtreeMap::iterator EraseSubtree(SubtreeMap::iterator it);

    // Focus is held by element id and a document remounts with the ids it had, so
    // a focus left behind by an unmounted subtree re-binds to the next session's
    // element: the play-stop focus in a chat box would swallow the movement keys
    // of the session after it, from its first frame. Focus dies with the elements
    // it named.
    void DropFocusInside(const UIElement& subtreeRoot);

    // Retains the World query across frames so its structural-version cache skips
    // the per-frame GetAllArchetypes() allocation + archetype rescan + global
    // cache-manager lock churn that a fresh per-frame Query<>() temporary pays.
    // Query is non-movable (holds a shared_mutex + self-registers), so it lives
    // behind a pointer, re-seated only when the queried world changes. Defined in
    // the .cpp to keep the ECS Query template out of this header.
    struct DocQueryCache;

    std::unique_ptr<UIManager> m_UI;
    AssetManager* m_AssetManager = nullptr;
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
    SubtreeMap m_Subtrees;
    std::unique_ptr<DocQueryCache> m_DocQuery;
    uint64_t m_FrameCounter = 0;
    uint64_t m_BindGeneration = 0;
    float m_PointerX=0.0f, m_PointerY=0.0f;
    bool m_Interactive = false;   // true while a pointer is over the host (set by SetPointer, cleared on leave); drives Update's interactive flag
    bool m_PrimaryDown = false;   // last primary-button state, to emit edge events
    bool m_PointerWasOver = false; // last frame had a pointer; clears hover on the over→off edge
    bool m_WaitForPrimaryRelease = false;
    uint64_t m_PointerBindGeneration = 0;
    uint64_t m_PointerRootInstanceId = 0;
    bool m_NeverClearTarget = false; // host composites onto a non-owned target → always Load
};

} // namespace GameEngine
