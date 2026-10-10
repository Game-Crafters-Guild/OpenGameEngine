#include "Engine/GameUI/GameUIHost.h"

#include "Engine/GameUI/GameplayUI.h"
#include "AssetCore/Asset.h"
#include "Components/UI/UIDocument.h"
// JobSystem types must be fully defined before the ECS Query template (Query.h
// uses JobSystem::TaskHandle for its parallel paths). A normal system TU gets
// these transitively via its system header; GameUIHost has no such header.
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "ECS/ECS.h"   // EntityHandle
#include "ECS/World.h" // World
#include "ECS/Components.h"
#include "ECS/Query.h" // ECS::Query<> (complete type for the retained DocQueryCache member)
#include "Engine/UI/FontResolver.h"
#include "Input/InputSystem.h" // kKeyActionRelease
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"                // Rendering::Utils::ReadFile
#include "Rendering/Core/RenderGraph/RGFrame.h"     // RGLoadOp, RGTexture (full)
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h" // GameEngine::Style::ZIndex (includes UIStyle.h)
#include "Core/Time.h"          // Time::GetDeltaTime for the UI animation clock

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine {

// Clamp ceiling for the UI animation delta: a frame hitch (asset import, shader
// compile, a modal) mustn't snap CSS transitions/keyframes to completion. Living
// with the ticker means the shipped Player gets the guard too, not just the editor.
static constexpr float kMaxAnimationStepSeconds = 0.1f;

// GE_GAMEUI_DIAG=1: per-frame [GameUI] trace of subtree bind state, the layout
// size override, the bound hud rects, and the composite result — the headless
// HUD-verify aid. Off by default.
static bool GameUiDiagEnabled()
{
    static const bool kEnabled = []
    {
        const char* v = std::getenv("GE_GAMEUI_DIAG");
        return v && v[0] == '1';
    }();
    return kEnabled;
}

// Retains the Query across frames (see header). Constructed directly from the
// World* rather than via World::Query<>(): read-only iteration needs no
// AutoComponentRegistrar pass (component type ids are consteval and the matching
// archetypes already exist when any UIDocument entity does), and a retained Query
// lets UpdateCache's structural-version gate skip the per-frame archetype rescan +
// allocation a fresh temporary always repays.
struct GameUIHost::DocQueryCache
{
    explicit DocQueryCache(ECS::World& world) : OwnerWorld(&world), Query(&world) {}
    ECS::World* OwnerWorld = nullptr;
    ECS::Query<ECS::Read<Components::UIDocument>> Query;
};

GameUIHost::GameUIHost(Rendering::IDevice* device, AssetManager* assetManager,
                       JobSystem::WorkStealingThreadPool* jobSystem)
    : m_AssetManager(assetManager), m_JobSystem(jobSystem)
{
    m_UI = std::make_unique<UIManager>(device, assetManager);
    if (jobSystem)
        m_UI->SetJobSystem(jobSystem);
    // F4-F12 over a play surface belong to the game, not to the UI module's
    // diagnostics. The environment opt-in scopes them to a developer; this
    // scopes them to the editor's own chrome, so a developer who turns them on
    // does not take the game's function keys with them.
    m_UI->SetDebugKeysEnabled(false);
    // Keys follow focus on a game surface, never the cursor. The chrome fallback
    // — a key with nothing focused goes to the hovered element — would hand the
    // game's Space to whatever HUD button the pointer was left resting on.
    m_UI->SetKeyHoverFallbackEnabled(false);
    // The wheel follows the cursor on a game surface, never focus. A HUD floats
    // over the world and lets the pointer through, so most of the screen hovers
    // nothing — and the chrome fallbacks (focused element, then first focusable)
    // would hand every tick over the world to a HUD list instead of to the game.
    m_UI->SetScrollFocusFallbackEnabled(false);
    // Empty root container; per-entity document subtrees mount as its children.
    m_UI->SetRoot(std::make_unique<UIElement>());
}

GameUIHost::~GameUIHost()
{
    if (GameUI::GetHost() == this)
        GameUI::SetHost(nullptr);
}

std::unique_ptr<GameUIHost> GameUIHost::CreateForHost(
    Rendering::IDevice* device, AssetManager* assetManager,
    JobSystem::WorkStealingThreadPool* jobSystem, const std::filesystem::path& fontsDir,
    bool neverClearTarget)
{
    auto host = std::make_unique<GameUIHost>(device, assetManager, jobSystem);
    host->ConfigureFonts(fontsDir);
    host->SetNeverClearTarget(neverClearTarget);
    return host;
}

// The default HUD font is the same ~515KB TTF for every host; read it once per
// process and share the bytes so N hosts (Player + editor Game View + up to one
// per Scene View pane) don't each re-read it from disk. References into the cache
// stay valid across later inserts (unordered_map never relocates elements).
static const std::vector<uint8_t>& ReadDefaultFontBytesCached(const std::string& path)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::vector<uint8_t>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(path);
    if (it == cache.end())
        it = cache.emplace(path, Rendering::Utils::ReadFile(path.c_str())).first;
    return it->second;
}

void GameUIHost::ConfigureFonts(const std::filesystem::path& assetsDir)
{
    if (!m_AssetManager || !m_UI)
        return;

    auto& reg = m_AssetManager->GetRegistry();
    const std::filesystem::path fontPath = assetsDir / kDefaultFontPath;
    if (!reg.IsAssetRegistered(fontPath))
        reg.RegisterAsset(fontPath);
    const GUID fontGuid = reg.GetAssetGUID(fontPath);
    const std::vector<uint8_t>& bytes = ReadDefaultFontBytesCached(fontPath.string());
    if (!m_UI->SetDefaultFontBytes(bytes, fontGuid))
        Logger::Log::Warning("Game UI: failed to install default font '{}'", fontPath.string());

    auto fontResolver = std::make_shared<GameEngine::Engine::UI::FontResolver>(reg, m_JobSystem);
    m_UI->SetFontResolver(
        [fontResolver](const std::string& family, int weight, FontStyle style, FontVariant variant,
                       UIManager::FontResolveCallback onReady)
        {
            if (!fontResolver || !onReady)
                return;
            fontResolver->ResolveAsync(
                family, weight,
                (style == FontStyle::Italic)    ? GameEngine::Engine::UI::FontStyle::Italic
                : (style == FontStyle::Oblique) ? GameEngine::Engine::UI::FontStyle::Oblique
                                                : GameEngine::Engine::UI::FontStyle::Normal,
                (variant == FontVariant::SmallCaps) ? GameEngine::Engine::UI::FontVariant::SmallCaps
                                                    : GameEngine::Engine::UI::FontVariant::Normal,
                [onReady = std::move(onReady)](GameEngine::Engine::UI::FontBytes fb) mutable
                {
                    UIManager::FontResolveResult r{};
                    r.Bytes = std::move(fb.bytes);
                    r.FaceIndex = fb.faceIndex;
                    r.DebugName = std::move(fb.debugName);
                    r.AssetGuid = fb.assetGuid;
                    onReady(std::move(r));
                });
        });
    m_UI->RequestFontFamily("Roboto");
}

void GameUIHost::SyncFromWorld(ECS::World& world)
{
    if (!m_UI)
        return;
    ++m_FrameCounter;
    UIElement* root = m_UI->GetRootElement();
    if (!root)
        return;

    if (!m_DocQuery || m_DocQuery->OwnerWorld != &world)
        m_DocQuery = std::make_unique<DocQueryCache>(world);
    m_DocQuery->Query.Each(
        [&](ECS::EntityHandle e, const Components::UIDocument& doc)
        {
            const uint64_t id = e.id;
            SubtreeState& st = m_Subtrees[id];
            if (!st.Root)
            {
                auto child = std::make_unique<UIElement>();
                st.Root = child.get();
                // A hot-reload reconcile rebuilds the bound subtree without passing
                // through SyncFromWorld. The handler lives on the container, so it ends
                // with the subtree.
                st.Root->RegisterEventHandler(kEventLayoutReconciled, [this](UIEvent&) { ++m_BindGeneration; });
                root->AddChild(std::move(child));
            }
            st.SeenFrame = m_FrameCounter;
            st.Fullscreen = (doc.RenderMode == Components::UIRenderMode::Fullscreen);

            // Layering: SortOrder -> CSS z-index on the per-entity container.
            if (st.SortOrder != doc.SortOrder)
            {
                st.SortOrder = doc.SortOrder;
                st.Root->Overrides().Set(Style::ZIndex, static_cast<int>(doc.SortOrder));
            }

            // Layout (.uxml): kick an async load on change, poll GetAsset, bind on the UI thread.
            const GUID layoutGuid = doc.Layout.ToGuid();
            if (st.LayoutGuid != layoutGuid)
            {
                st.LayoutGuid = layoutGuid;
                st.LayoutApplied = false;
                st.LayoutFailed = false;
                st.LayoutLoad = AssetLoadHandle{};
                if (!layoutGuid.IsNull())
                    st.LayoutLoad = m_AssetManager->LoadAsset(
                        layoutGuid, [](Result<SharedPtr<Asset>, AssetError>) {}, AssetLoadPriority::High);
            }
            if (!st.LayoutApplied && !st.LayoutFailed && !layoutGuid.IsNull())
            {
                auto a = m_AssetManager->GetAsset(layoutGuid);
                if (a)
                {
                    // Wrong-type asset (e.g. a .css assigned to Layout) never binds; latch +
                    // warn once instead of re-polling the registry every frame forever.
                    if (a->GetType() == AssetType::UILayout)
                    {
                        if (m_UI->BindLayoutToSubtreeChildrenFromAsset(st.Root, *static_cast<UILayoutAsset*>(a.get())))
                        {
                            st.LayoutApplied = true;
                            ++m_BindGeneration;
                        }
                    }
                    else
                    {
                        Logger::Log::Warning("Game UI: UIDocument.Layout asset {} is not a UILayout (.uxml); ignoring.",
                                             layoutGuid.ToString());
                        st.LayoutFailed = true;
                    }
                }
            }

            // Style (.css): kick an async load on change, poll GetAsset, attach on the
            // UI thread. Unlike layout (ReconcileChildren diffs), a style swap must
            // REPLACE: AttachStyleToSubtreeFromAsset only adds sheets (dedup), never
            // removing the previous style. ClearSubtreeStyle (UIManager owns the
            // unregister-then-remove order) drops the old style before the new attaches.
            const GUID styleGuid = doc.Style.ToGuid();
            if (st.StyleGuid != styleGuid)
            {
                if (st.Root) // st.StyleGuid is still the previous GUID here
                    m_UI->ClearSubtreeStyle(st.Root, st.StyleGuid);
                st.StyleGuid = styleGuid;
                st.StyleApplied = false;
                st.StyleFailed = false;
                st.StyleLoad = AssetLoadHandle{};
                if (!styleGuid.IsNull())
                    st.StyleLoad = m_AssetManager->LoadAsset(
                        styleGuid, [](Result<SharedPtr<Asset>, AssetError>) {}, AssetLoadPriority::High);
            }
            if (!st.StyleApplied && !st.StyleFailed && !styleGuid.IsNull())
            {
                auto a = m_AssetManager->GetAsset(styleGuid);
                if (a)
                {
                    if (a->GetType() == AssetType::UIStyle)
                    {
                        // Defer the attach until @import deps are cached so AttachStyle's
                        // import resolution doesn't block this render-thread frame; once
                        // resident the attach is non-blocking. A permanently-failed import
                        // latches StyleFailed (warn once) instead of re-polling forever.
                        auto* style = static_cast<UIStyleAsset*>(a.get());
                        switch (m_UI->QueryStyleImports(*style))
                        {
                        case UIManager::StyleImportStatus::Resident:
                            m_UI->AttachStyleToSubtreeFromAsset(st.Root, *style);
                            st.StyleApplied = true;
                            break;
                        case UIManager::StyleImportStatus::Failed:
                            // A dead @import (deleted/corrupt): attach anyway — the cascade
                            // builder skips the missing import gracefully and keeps every valid
                            // sheet — and latch StyleApplied so we stop re-polling (the dead
                            // import will never resolve). Partial styling beats no styling.
                            Logger::Log::Warning(
                                "Game UI: UIDocument.Style {} has an @import that failed to load; "
                                "applying the rest.",
                                styleGuid.ToString());
                            m_UI->AttachStyleToSubtreeFromAsset(st.Root, *style);
                            st.StyleApplied = true;
                            break;
                        case UIManager::StyleImportStatus::Pending:
                            break; // imports still loading → retry next frame
                        }
                    }
                    else
                    {
                        Logger::Log::Warning("Game UI: UIDocument.Style asset {} is not a UIStyle (.css); ignoring.",
                                             styleGuid.ToString());
                        st.StyleFailed = true;
                    }
                }
            }
        });

    // Mark-and-sweep: drop subtrees whose entity/component went away or was
    // switched off this frame (the query no longer visits it).
    for (auto it = m_Subtrees.begin(); it != m_Subtrees.end();)
    {
        if (it->second.SeenFrame != m_FrameCounter)
            it = EraseSubtree(it);
        else
            ++it;
    }

    // After the sweep: the frame's subtree set is final, so a Fullscreen document
    // that went away this frame no longer suppresses anything.
    ApplyFullscreenOverlaySuppression();

    if (GameUiDiagEnabled() && (m_FrameCounter <= 10 || (m_FrameCounter % 180) == 0))
    {
        size_t applied = 0;
        for (auto& [id, st] : m_Subtrees)
            if (st.LayoutApplied)
                ++applied;
        Logger::Log::Info("[GameUI] frame={} subtrees={} layoutApplied={} rootChildren={}", m_FrameCounter,
                          m_Subtrees.size(), applied, root->GetChildren().size());
    }
}

void GameUIHost::ApplyFullscreenOverlaySuppression()
{
    // A Fullscreen document owns the screen, so Overlay (HUD) documents are hidden
    // while one is ready to cover it — otherwise a HUD with a higher SortOrder floats
    // over the menu. Readiness is the same predicate the composite Clear uses: a menu
    // that is still loading, or whose layout never binds, has nothing to draw in the
    // HUD's place, so it must not hide it.
    //
    // The suppression is display:none on the per-entity container the host owns —
    // never a change to what is registered on the document's elements. Handlers
    // (a gameplay script's, and the built-in controls' own) outlive it untouched and
    // resume with the container; a hidden subtree simply neither lays out, draws, nor
    // hit-tests. Reset — not a Set back to some assumed default — is what restores it:
    // the bind copies the layout root's classes onto this container, so the document's
    // own CSS can be selecting its display, and only dropping the override lets the
    // cascade recover whatever that was.
    //
    // Toggled on change only: re-setting an override every frame would dirty the
    // cascade for every document on every frame.
    //
    // Deliberately NOT gated on m_NeverClearTarget: which documents are visible is a
    // property of the document set, not of the target. The Scene View preview
    // composites onto a target it doesn't own — it never clears, so a menu draws over
    // the editor's scene instead of over black — but it must still show the document
    // set the game will show. A HUD floating over the menu only in the Scene View
    // would be a WYSIWYG lie about the very thing the preview exists to answer.
    const bool anyReadyFullscreen = AnyReadyFullscreenSubtree();
    for (auto& [id, st] : m_Subtrees)
    {
        if (!st.Root)
            continue;
        const bool hide = anyReadyFullscreen && !st.Fullscreen;
        if (hide == st.OverlayHidden)
            continue;
        if (hide)
            st.Root->Overrides().Set(Style::Display, DisplayMode::None);
        else
            st.Root->Overrides().Reset(Style::Display);
        st.OverlayHidden = hide;
    }
}

void GameUIHost::DropFocusInside(const UIElement& subtreeRoot)
{
    if (!m_UI || m_UI->GetFocusedElementId().empty())
        return;
    UIElement* root = m_UI->GetRootElement();
    // Resolve the id the way the manager's own dispatch does, so this asks
    // exactly "is the element that would receive the next key inside the subtree
    // leaving the tree".
    UIElement* focused = root ? root->FindById(m_UI->GetFocusedElementId()) : nullptr;
    for (UIElement* p = focused; p; p = p->GetParent())
    {
        if (p == &subtreeRoot)
        {
            m_UI->ClearFocus();
            return;
        }
    }
}

void GameUIHost::RemoveSubtree(uint64_t entityId)
{
    auto it = m_Subtrees.find(entityId);
    if (it != m_Subtrees.end())
        EraseSubtree(it);
}

GameUIHost::SubtreeMap::iterator GameUIHost::EraseSubtree(SubtreeMap::iterator it)
{
    if (UIElement* root = m_UI ? m_UI->GetRootElement() : nullptr; root && it->second.Root)
    {
        DropFocusInside(*it->second.Root);
        root->RemoveChild(it->second.Root);
    }
    ++m_BindGeneration;
    return m_Subtrees.erase(it);
}

uint64_t GameUIHost::GetBindGeneration() const
{
    return m_BindGeneration;
}

UIElement* GameUIHost::FindDocumentRoot(uint64_t entityId) const
{
    const auto it = m_Subtrees.find(entityId);
    return it != m_Subtrees.end() ? it->second.Root : nullptr;
}

UIElement* GameUIHost::FindElementById(uint64_t entityId, std::string_view id)
{
    UIElement* root = FindDocumentRoot(entityId);
    return root ? root->FindById(id) : nullptr;
}

UIElement* GameUIHost::FindElementByIdAny(std::string_view id, uint64_t* outOwnerEntityId)
{
    // Lowest owning entity id wins a shared id. m_Subtrees is an unordered_map whose
    // iteration order changes across rehashes (documents mount/unmount every
    // SyncFromWorld), so "first hit" could flip frame to frame — and
    // GE_GameUI_FindElement mints a DURABLE handle from a single lookup, so an
    // unstable winner would hand two callers handles into different documents and
    // every later write would follow the one that happened to win.
    UIElement* found = nullptr;
    uint64_t bestId = ~uint64_t{0};
    for (const auto& [entityId, st] : m_Subtrees)
    {
        if (!st.Root)
            continue;
        UIElement* el = st.Root->FindById(id);
        if (!el)
            continue;
        if (entityId < bestId)
        {
            bestId = entityId;
            found = el;
        }
    }
    if (found && outOwnerEntityId)
        *outOwnerEntityId = bestId;
    return found;
}

void GameUIHost::ResetBoundDocuments()
{
    UpdatePointer(nullptr);
    if (m_Subtrees.empty())
        return;
    std::vector<uint64_t> ids;
    ids.reserve(m_Subtrees.size());
    for (const auto& [id, _] : m_Subtrees)
        ids.push_back(id);
    for (const uint64_t id : ids)
        RemoveSubtree(id);
}

bool GameUIHost::PointerTreeReplacedUnderPress() const
{
    if (!m_PrimaryDown || !m_UI)
        return false;
    const UIElement* root = m_UI->GetRootElement();
    const uint64_t rootId = root ? root->GetInstanceId() : 0;
    return m_PointerBindGeneration != GetBindGeneration() || m_PointerRootInstanceId != rootId;
}

void GameUIHost::UpdatePointer(const PointerState* pointer)
{
    if (!m_UI)
        return;
    UIElement* root = m_UI->GetRootElement();
    if (!pointer || !root || !std::isfinite(pointer->X) || !std::isfinite(pointer->Y) ||
        PointerTreeReplacedUnderPress())
    {
        const bool sourceHoldsPrimary =
            pointer ? pointer->PrimaryDown : (m_PrimaryDown || m_WaitForPrimaryRelease);
        CancelPointer(sourceHoldsPrimary ? PrimaryButtonState::Held : PrimaryButtonState::Released);
        if (!pointer || !root)
            return;
    }
    if (!std::isfinite(pointer->X) || !std::isfinite(pointer->Y))
        return;
    // This host has its own UIManager; it may never receive platform key events.
    // Replace the previous snapshot so a later plain click cannot inherit modifiers.
    m_UI->ReconcileModifierKeys(pointer->Modifiers);
    if (m_WaitForPrimaryRelease)
    {
        // A move carrying the old held state cannot arm a new document. Consume
        // its release too: raw MouseUp handlers must not see an orphan release.
        m_WaitForPrimaryRelease = pointer->PrimaryDown;
        OnMouseMove({pointer->X, pointer->Y});
        m_Interactive = true;
        m_PointerWasOver = true;
        return;
    }
    SetPointer(pointer->X, pointer->Y, pointer->PrimaryDown);
}

void GameUIHost::OnMouseMove(Mathematics::Vector2 position)
{
    if (!m_UI || !std::isfinite(position.x) || !std::isfinite(position.y))
        return;
    if (PointerTreeReplacedUnderPress())
        CancelPointer(PrimaryButtonState::Held);
    m_PointerX = position.x;
    m_PointerY = position.y;
    const float scale = m_UI->GetContentScale();
    m_UI->OnMouseMove(position.x / scale, position.y / scale);
    m_Interactive = true;
    m_PointerWasOver = true;
}

bool GameUIHost::OnMouseButton(int button, bool pressed, int mods)
{
    if (!m_UI)
        return false;
    // The click's mask is live platform state, and it is the only report of a
    // modifier that was already held when the window took focus.
    m_UI->SyncModifierKeys(mods);
    return DispatchMouseButton(button, pressed);
}

bool GameUIHost::DispatchMouseButton(int button, bool pressed)
{
    if (!m_UI)
        return false;
    const bool primary = button == Input::kMouseButton_Left;
    if (primary && PointerTreeReplacedUnderPress())
        CancelPointer(PrimaryButtonState::Held);
    if (m_WaitForPrimaryRelease)
    {
        // The cancelled gesture owns every edge until the source lets go: a
        // release forwarded here would reach raw MouseUp handlers as an orphan,
        // and a press would arm a control the gesture never reached.
        if (primary && !pressed)
            m_WaitForPrimaryRelease = false;
        return false;
    }
    if (primary)
    {
        if (pressed && !m_PrimaryDown)
        {
            const UIElement* root = m_UI->GetRootElement();
            m_PointerBindGeneration = GetBindGeneration();
            m_PointerRootInstanceId = root ? root->GetInstanceId() : 0;
        }
        m_PrimaryDown = pressed;
    }
    m_Interactive = true;
    m_PointerWasOver = true;
    return m_UI->OnMouseButton(button, pressed);
}

bool GameUIHost::OnScroll(Mathematics::Vector2 delta)
{
    if (!m_UI)
        return false;
    return m_UI->OnScroll(delta.x, delta.y);
}

void GameUIHost::CancelPointer(PrimaryButtonState primary)
{
    if (!m_UI)
        return;
    m_UI->ResetModifierKeys();
    m_WaitForPrimaryRelease = primary == PrimaryButtonState::Held;
    if (m_PointerWasOver || m_PrimaryDown)
        PointerLeftSurface();
}

bool GameUIHost::OnKey(int key, int action, int mods)
{
    if (!m_UI)
        return false;
    const bool consumed = m_UI->OnKey(key, action, mods);
    // Escape hands the keyboard back to the game. A text control reverts its
    // edit on Escape and keeps focus, which is right for a chrome field the user
    // can click away from; on a game surface the only way out would be clicking
    // the world, and until then every movement key is still the field's. The
    // field's own Escape has already run, so the answer above is unchanged: an
    // Escape nothing acted on still travels on to the game.
    if (action != Input::kKeyActionRelease && key == Input::kKeyCode_Escape)
        m_UI->ClearFocus();
    return consumed;
}

bool GameUIHost::OnChar(unsigned int codepoint)
{
    if (!m_UI)
        return false;
    return m_UI->OnChar(codepoint);
}

void GameUIHost::ResetKeyboardState()
{
    if (!m_UI)
        return;
    m_UI->ResetModifierKeys();
}

void GameUIHost::SetPointer(float x, float y, bool primaryDown)
{
    OnMouseMove({x, y});
    if (primaryDown != m_PrimaryDown)
        DispatchMouseButton(Input::kMouseButton_Left, primaryDown);
}

void GameUIHost::PointerLeftSurface()
{
    if (!m_UI)
        return;
    if (m_PrimaryDown)
    {
        // Cancel, not release: the user is still holding the button, just not over this
        // surface, and the release they eventually make will never reach this UIManager.
        // Reporting it as a release would resolve at the last known position — still
        // inside the armed control — and activate it.
        m_PrimaryDown = false;
        m_UI->CancelPress();
    }
    m_UI->OnCursorEnter(false);
    m_PointerWasOver = false;
    // Drop back to the cheaper passive Update path: with the cursor gone there's
    // no hit-test/hover/dispatch work to do until a pointer is fed again. Without
    // this the first hover would latch interactive Update on for the rest of the run.
    m_Interactive = false;
}

void GameUIHost::Update(float deltaTime, uint32_t extentW, uint32_t extentH)
{
    if (!m_UI)
        return;
    m_UI->SetLayoutSizeOverride(extentW, extentH);
    const float previousScale = m_UI->GetContentScale();
    m_UI->Update(deltaTime, m_Interactive);
    if (m_PointerWasOver && previousScale != m_UI->GetContentScale())
        OnMouseMove({m_PointerX, m_PointerY});

    if (GameUiDiagEnabled() && (m_FrameCounter <= 10 || (m_FrameCounter % 180) == 0))
    {
        UIElement* root = m_UI->GetRootElement();
        UIElement* panel = root ? root->FindById("hud-panel") : nullptr;
        if (root)
            Logger::Log::Info("[GameUI] Update override={}x{} rootRect=({},{},{},{}) panel={}", extentW, extentH,
                              root->GetLayoutX(), root->GetLayoutY(), root->GetLayoutWidth(),
                              root->GetLayoutHeight(),
                              panel ? "found" : "MISSING");
        if (panel)
            Logger::Log::Info("[GameUI] hud-panel rect=({},{},{},{})", panel->GetLayoutX(), panel->GetLayoutY(),
                              panel->GetLayoutWidth(), panel->GetLayoutHeight());
    }
}

bool GameUIHost::AnyReadyFullscreenSubtree() const
{
    // A Fullscreen document only owns the target once its layout is actually bound.
    // Gating on RenderMode alone hands the composite a Clear while the menu still has
    // nothing to draw, so the frame goes black for as long as the .uxml is loading —
    // and permanently when the layout GUID is missing or the wrong asset type, because
    // LayoutFailed latches and no content ever arrives.
    //
    // Read from the live subtree set rather than a SyncFromWorld-time member so a frame
    // composited without a re-sync (world == nullptr) can't act on a stale verdict.
    for (const auto& [id, st] : m_Subtrees)
        if (st.Fullscreen && st.LayoutApplied && !st.LayoutFailed)
            return true;
    return false;
}

bool GameUIHost::CoversTargetOpaque() const
{
    return AnyReadyFullscreenSubtree() && !m_NeverClearTarget;
}

bool GameUIHost::RenderRG(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture target,
                          UI::UITargetSpace targetSpace)
{
    if (!m_UI)
        return false;
    // Fullscreen documents own the target (menus → Clear); Overlay documents blend
    // over the scene (HUDs → Load). Host-wide: any READY Fullscreen doc clears —
    // UNLESS this host composites onto a target it doesn't own (the Scene View preview
    // overlays the editor's shared scene+gizmos), in which case it must always Load.
    const auto loadOp = CoversTargetOpaque() ? Rendering::RenderGraph::RGLoadOp::Clear
                                             : Rendering::RenderGraph::RGLoadOp::Load;
    const bool ok = m_UI->RenderRG(frame, target, targetSpace, loadOp);
    if (GameUiDiagEnabled() && (m_FrameCounter <= 10 || (m_FrameCounter % 180) == 0))
        Logger::Log::Info("[GameUI] RenderRG ok={}", ok);
    return ok;
}

bool GameUIHost::SyncDocuments(ECS::World* world)
{
    if (world)
        SyncFromWorld(*world);
    return HasAnyDocuments();
}

void GameUIHost::UpdateAndRender(Rendering::RenderGraph::RGFrame& frame,
                                 Rendering::RenderGraph::RGTexture target,
                                 UI::UITargetSpace targetSpace, uint32_t extentW, uint32_t extentH)
{
    // No UIDocument entities → declare no UI pass at all (don't pay an empty
    // overlay pass on document-less scenes).
    if (!HasAnyDocuments())
        return;
    // The UI animation clock reads the global frame delta (Time::GetDeltaTime,
    // deterministic under editor UI replay), clamped so a frame hitch can't snap
    // CSS transitions/keyframes to completion.
    Update(std::min(Time::GetDeltaTime(), kMaxAnimationStepSeconds), extentW, extentH);
    RenderRG(frame, target, targetSpace);
}

void GameUIHost::SyncAndRender(ECS::World* world, Rendering::RenderGraph::RGFrame& frame,
                               Rendering::RenderGraph::RGTexture target, UI::UITargetSpace targetSpace,
                               uint32_t extentW, uint32_t extentH)
{
    SyncDocuments(world);
    UpdateAndRender(frame, target, targetSpace, extentW, extentH);
}

} // namespace GameEngine
