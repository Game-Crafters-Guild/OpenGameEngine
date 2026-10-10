#include "ShaderGraph/MaterialGraphPreviewHost.h"

#include "EditorContext.h"
#include "Graph/NodeColorSettings.h"
#include "Types/StringId.h"
#include "Graph/SgGraphModelBridge.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Platform/SystemMetrics.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "ShaderGraph/GraphPreviewMaterialRuntime.h"
#include "ShaderGraph/MaterialGraphPreviewModel.h"
#include "Types/Fnv1a.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgPropertyBinding.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace GameEngine {

namespace {

constexpr const char* kEnginePrefix = "engine:";
constexpr size_t kEnginePrefixLen = 7;
constexpr const char* kMaterialEnginePrefix = "engine:editor_material_thumb_";
constexpr float kPreviewMarginPx = 24.0f;
constexpr float kPreviewMinSizePx = 160.0f;
constexpr float kPreviewFallbackSizePx = 220.0f;
constexpr float kPreviewCollapsedHeightPx = 48.0f;
// Radians per frame, matching the released-drag spin the thumbnail orbit used.
constexpr float kDefaultOrbitSpeed = 0.02f;
constexpr float kTwoPi = 6.28318530718f;

/** Keep the accumulated angle in one turn: it advances every frame for as long
    as the editor is open, and an unwrapped float loses enough mantissa over a
    long session for the step itself to start quantizing. */
float WrapAngle(float radians)
{
    radians = std::fmod(radians, kTwoPi);
    return radians < 0.f ? radians + kTwoPi : radians;
}

std::string ResourceNameWithoutEnginePrefix(const std::string& s)
{
    if (s.rfind(kEnginePrefix, 0) == 0)
        return s.substr(kEnginePrefixLen);
    return s;
}

MaterialDocument BuildPreviewMaterialDocument(const Editor::MaterialGraphPreviewModel& preview,
                                               const std::filesystem::path& graphSourcePath)
{
    const Graph::Model& model = preview.Model;
    MaterialDocument matDoc = MaterialDocument::CreateDefaultPBR("Graph Live Preview");
    matDoc.surfaceGraph.clear();
    matDoc.surfaceGraphGuid.clear();
    matDoc.surfaceShader = "live_preview_built.glsl";
    if (!model.LightingModel.empty())
        matDoc.lightingModel = model.LightingModel;
    ApplyMaterialGraphVariablesToPreviewDocument(model, matDoc);
    Editor::ApplyPreviewLaneValues(preview, matDoc);
    if (!graphSourcePath.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(graphSourcePath, ec))
            Engine::Renderer::MergeShaderGraphTagsIntoDocument(graphSourcePath, matDoc);
    }
    return matDoc;
}

} // namespace

MaterialGraphPreviewHost::MaterialGraphPreviewHost()
{
    AddClass("node-graph-preview");
    EnsureUi();
}

void MaterialGraphPreviewHost::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
}

void MaterialGraphPreviewHost::EnsureUi()
{
    if (m_Image)
        return;

    auto title = std::make_unique<Label>();
    title->SetText("PREVIEW");
    title->AddClass("node-graph-preview-title");
    title->SetTooltip("Drag the top edge to move. Double-click to collapse or expand.");
    m_Title = title.get();
    AddChild(std::move(title));

    auto image = std::make_unique<UIElement>();
    image->AddClass("node-graph-preview-image");
    image->Overrides()
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true});

    image->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button != 0 || !m_Active)
            return;
        m_IsDragging = true;
        m_DidDrag = false;
        m_LastDragX = e.X;
        m_LastDragDeltaX = 0.f;
        e.Capture(m_Image);
        e.Stop();
    });

    image->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (!m_IsDragging || !m_Active)
            return;
        const float dx = e.X - m_LastDragX;
        m_LastDragX = e.X;
        m_LastDragDeltaX = dx;
        if (std::fabs(dx) > 0.5f)
            m_DidDrag = true;

        constexpr float kSensitivity = 0.0025f;
        const float scale = ModelThumbnailHandler::GetOrbitSpeedScale();
        m_OrbitVelocity = -dx * kSensitivity * scale;
        m_OrbitYaw = WrapAngle(m_OrbitYaw + m_OrbitVelocity);
        if (std::fabs(dx) > 1e-3f)
            m_LastDragOrbitSign = dx > 0.f ? -1.f : 1.f;
        e.Stop();
    });

    image->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button != 0 || !m_IsDragging)
            return;
        m_IsDragging = false;

        if (!m_DidDrag)
        {
            m_OrbitPaused = !m_OrbitPaused;
            m_OrbitVelocity = 0.f;
            e.Stop();
            return;
        }

        constexpr float kReleaseMovementEpsilon = 1e-3f;
        if (std::fabs(m_LastDragDeltaX) <= kReleaseMovementEpsilon)
        {
            m_OrbitVelocity = 0.f;
            e.Stop();
            return;
        }

        if (ModelThumbnailHandler::GetRotatePreviewsEnabled() && !m_OrbitPaused)
        {
            const float sign = (std::fabs(m_LastDragOrbitSign) > 1e-3f) ? m_LastDragOrbitSign : 1.f;
            m_OrbitVelocity =
                sign * kDefaultOrbitSpeed * ModelThumbnailHandler::GetOrbitSpeedScale();
        }
        else
        {
            m_OrbitVelocity = 0.f;
        }
        e.Stop();
    });

    m_Image = image.get();
    AddChild(std::move(image));

    AddMoveHandle("node-graph-preview-move-top");
    AddResizeZone(ResizeAnchor::Left, "node-graph-preview-resize-left");
    AddResizeZone(ResizeAnchor::Right, "node-graph-preview-resize-right");
    AddResizeZone(ResizeAnchor::Bottom, "node-graph-preview-resize-bottom");
}

void MaterialGraphPreviewHost::AddMoveHandle(const char* className)
{
    auto moveHandle = std::make_unique<UIElement>();
    moveHandle->AddClass("node-graph-preview-edge-zone");
    moveHandle->AddClass(className);
    moveHandle->SetTooltip("Drag to move. Double-click to collapse or expand.");
    /* The handle caps a 220px-tall image; a tooltip below the cursor would
       cover the sphere it describes. The host hugs the canvas right edge, so
       the free space is to its left. */
    moveHandle->SetTooltipPlacement(UIElement::TooltipPlacement::Left);
    UIElement* handle = moveHandle.get();
    /* The handle is invisible, so the title is the affordance: it brightens
       while the pointer is over the strip and stays lit through the drag. */
    moveHandle->RegisterEventHandler(kEventMouseEnter,
                                     [this](UIEvent&) { SetMoveHint(true); });
    moveHandle->RegisterEventHandler(kEventMouseLeave,
                                     [this](UIEvent&) { SetMoveHint(m_IsMovingPreview); });
    moveHandle->RegisterEventHandler(kEventMouseDown, [this, handle](UIEvent& e) {
        if (e.Button != 0 || !m_Active)
            return;
        BeginMove(e.X, e.Y);
        SetMoveHint(true);
        e.Capture(handle);
        e.Stop();
    });
    moveHandle->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (!m_IsMovingPreview)
            return;
        UpdateMove(e.X, e.Y);
        e.Stop();
    });
    moveHandle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button != 0 || !m_IsMovingPreview)
            return;
        m_IsMovingPreview = false;
        SetMoveHint(false);
        if (!m_MoveDidDrag)
        {
            using clock = std::chrono::steady_clock;
            auto now = clock::now();
            const bool isDoubleClick = (now - m_MoveHandleLastClickTime) < GameEngine::Platform::GetDoubleClickInterval();
            m_MoveHandleLastClickTime = now;
            if (isDoubleClick)
                ToggleCollapsed();
        }
        e.Stop();
    });
    AddChild(std::move(moveHandle));
}

void MaterialGraphPreviewHost::SetMoveHint(bool on)
{
    if (!m_Title)
        return;
    if (on)
        m_Title->AddClass("move-hint");
    else
        m_Title->RemoveClass("move-hint");
    m_Title->MarkDirty(UIElement::VisualDirty);
}

void MaterialGraphPreviewHost::AddResizeZone(ResizeAnchor anchor, const char* className)
{
    auto resizeZone = std::make_unique<UIElement>();
    resizeZone->AddClass("node-graph-preview-edge-zone");
    resizeZone->AddClass(className);
    UIElement* handle = resizeZone.get();
    resizeZone->RegisterEventHandler(kEventMouseDown, [this, anchor, handle](UIEvent& e) {
        if (e.Button != 0 || !m_Active)
            return;
        BeginResize(anchor, e.X, e.Y);
        e.Capture(handle);
        e.Stop();
    });
    resizeZone->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (!m_IsResizingPreview)
            return;
        UpdateResize(e.X, e.Y);
        e.Stop();
    });
    resizeZone->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button != 0 || !m_IsResizingPreview)
            return;
        m_IsResizingPreview = false;
        m_ResizeAnchor = ResizeAnchor::None;
        e.Stop();
    });
    AddChild(std::move(resizeZone));
}

bool MaterialGraphPreviewHost::EnsurePreviewBoundsInitialized()
{
    if (m_HasPreviewBounds)
        return true;

    UIElement* bodyEl = GetParent();
    if (!bodyEl)
        return false;

    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    if (bodyW <= 1.0f || bodyH <= 1.0f)
        return false;

    const float maxSize = std::max(kPreviewMinSizePx, std::min(bodyW, bodyH) - kPreviewMarginPx * 2.0f);
    const float layoutSize = std::max({GetLayoutWidth(), GetLayoutHeight(), kPreviewFallbackSizePx});
    const float size = std::clamp(layoutSize, kPreviewMinSizePx, maxSize);
    // First place is always the CSS default (top-right of the body). Capturing
    // GetLayoutX/Y on the first pass ports a mid-layout left coordinate and the
    // preview jumps once the body grows.
    ApplyPreviewBounds(bodyW - size - kPreviewMarginPx, kPreviewMarginPx, size, size);
    return m_HasPreviewBounds;
}

void MaterialGraphPreviewHost::ApplyPreviewBounds(float left, float top, float width, float height)
{
    UIElement* bodyEl = GetParent();
    if (!bodyEl)
        return;

    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    if (bodyW <= 1.0f || bodyH <= 1.0f)
        return;

    const float maxSize = std::max(kPreviewMinSizePx, std::min(bodyW, bodyH) - kPreviewMarginPx * 2.0f);
    const float requestedSize = std::max(width, height);
    m_PreferredPreviewLeft = left;
    m_PreferredPreviewTop = top;
    m_PreferredPreviewSize = std::max(requestedSize, kPreviewMinSizePx);

    const float size = std::clamp(requestedSize, kPreviewMinSizePx, maxSize);
    left = std::clamp(left, kPreviewMarginPx, std::max(kPreviewMarginPx, bodyW - size - kPreviewMarginPx));
    top = std::clamp(top, kPreviewMarginPx, std::max(kPreviewMarginPx, bodyH - size - kPreviewMarginPx));
    const float roundedLeft = std::round(left);
    const float roundedTop = std::round(top);
    const float roundedSize = std::round(size);

    Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(roundedLeft))
        .Set(Style::PositionTop, StyleLength::Px(roundedTop))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(roundedSize))
        .Set(Style::Height, StyleLength::Px(m_Collapsed ? kPreviewCollapsedHeightPx : roundedSize))
        .Set(Style::ZIndex, 10);

    m_HasPreviewBounds = true;
    m_PreviewLeft = roundedLeft;
    m_PreviewTop = roundedTop;
    m_PreviewSize = roundedSize;
    m_LastParentWidth = bodyW;
    m_LastParentHeight = bodyH;

    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();
}

void MaterialGraphPreviewHost::BeginMove(float x, float y)
{
    UIElement* bodyEl = GetParent();
    if (!bodyEl)
        return;
    EnsurePreviewBoundsInitialized();

    m_IsMovingPreview = true;
    m_UserPlacedPreview = true;
    m_MoveDidDrag = false;
    m_GestureStartX = x;
    m_GestureStartY = y;
    m_GestureStartLeft = m_HasPreviewBounds ? m_PreviewLeft : GetLayoutX() - bodyEl->GetLayoutX();
    m_GestureStartTop = m_HasPreviewBounds ? m_PreviewTop : GetLayoutY() - bodyEl->GetLayoutY();
    const float size = m_HasPreviewBounds
                           ? std::max(m_PreviewSize, kPreviewMinSizePx)
                           : std::max({GetLayoutWidth(), GetLayoutHeight(), kPreviewFallbackSizePx});
    m_GestureStartWidth = size;
    m_GestureStartHeight = size;
    ApplyPreviewBounds(m_GestureStartLeft, m_GestureStartTop, m_GestureStartWidth, m_GestureStartHeight);
}

void MaterialGraphPreviewHost::UpdateMove(float x, float y)
{
    if (!m_IsMovingPreview)
        return;
    constexpr float kDragThresholdPx = 3.f;
    const float dx = x - m_GestureStartX;
    const float dy = y - m_GestureStartY;
    if (dx * dx + dy * dy > kDragThresholdPx * kDragThresholdPx)
        m_MoveDidDrag = true;
    ApplyPreviewBounds(m_GestureStartLeft + (x - m_GestureStartX),
                       m_GestureStartTop + (y - m_GestureStartY),
                       m_GestureStartWidth, m_GestureStartHeight);
}

void MaterialGraphPreviewHost::ToggleCollapsed()
{
    m_Collapsed = !m_Collapsed;
    ApplyCollapsedState();
}

void MaterialGraphPreviewHost::ApplyCollapsedState()
{
    if (m_Collapsed)
        AddClass("node-graph-preview-collapsed");
    else
        RemoveClass("node-graph-preview-collapsed");

    EnsurePreviewBoundsInitialized();
    const float size = m_HasPreviewBounds
                           ? std::max(m_PreviewSize, kPreviewMinSizePx)
                           : std::max({GetLayoutWidth(), GetLayoutHeight(), kPreviewFallbackSizePx});
    ApplyPreviewBounds(m_HasPreviewBounds ? m_PreviewLeft : 0.f,
                       m_HasPreviewBounds ? m_PreviewTop : 0.f,
                       size, size);

    MarkDirty(LayoutDirty | VisualDirty);
    RequestRelayout();
}

void MaterialGraphPreviewHost::BeginResize(ResizeAnchor anchor, float x, float y)
{
    if (m_Collapsed)
        return;

    UIElement* bodyEl = GetParent();
    if (!bodyEl)
        return;
    EnsurePreviewBoundsInitialized();

    m_IsMovingPreview = false;
    m_IsDragging = false;
    m_IsResizingPreview = true;
    m_UserPlacedPreview = true;
    m_ResizeAnchor = anchor;
    m_GestureStartX = x;
    m_GestureStartY = y;
    m_GestureStartLeft = m_HasPreviewBounds ? m_PreviewLeft : GetLayoutX() - bodyEl->GetLayoutX();
    m_GestureStartTop = m_HasPreviewBounds ? m_PreviewTop : GetLayoutY() - bodyEl->GetLayoutY();
    const float size = m_HasPreviewBounds
                           ? std::max(m_PreviewSize, kPreviewMinSizePx)
                           : std::max({GetLayoutWidth(), GetLayoutHeight(), kPreviewFallbackSizePx});
    m_GestureStartWidth = size;
    m_GestureStartHeight = size;
    ApplyPreviewBounds(m_GestureStartLeft, m_GestureStartTop, m_GestureStartWidth, m_GestureStartHeight);
}

void MaterialGraphPreviewHost::UpdateResize(float x, float y)
{
    if (!m_IsResizingPreview)
        return;

    UIElement* bodyEl = GetParent();
    if (!bodyEl)
        return;

    const float bodyW = bodyEl->GetLayoutWidth();
    const float bodyH = bodyEl->GetLayoutHeight();
    if (bodyW <= 1.0f || bodyH <= 1.0f)
        return;

    const float dx = x - m_GestureStartX;
    const float dy = y - m_GestureStartY;
    const float startSize = std::max({m_GestureStartWidth, m_GestureStartHeight, kPreviewMinSizePx});
    const float rawSize = [&]() {
        switch (m_ResizeAnchor)
        {
        case ResizeAnchor::Left:
            return startSize - dx;
        case ResizeAnchor::Right:
            return startSize + dx;
        case ResizeAnchor::Bottom:
            return startSize + dy;
        case ResizeAnchor::None:
            return startSize;
        }
        return startSize;
    }();

    float maxSize = startSize;
    switch (m_ResizeAnchor)
    {
    case ResizeAnchor::Left:
        maxSize = std::min(m_GestureStartLeft + m_GestureStartWidth - kPreviewMarginPx,
                           bodyH - m_GestureStartTop - kPreviewMarginPx);
        break;
    case ResizeAnchor::Right:
    case ResizeAnchor::Bottom:
        maxSize = std::min(bodyW - m_GestureStartLeft - kPreviewMarginPx,
                           bodyH - m_GestureStartTop - kPreviewMarginPx);
        break;
    case ResizeAnchor::None:
        return;
    }
    const float size = std::clamp(rawSize, kPreviewMinSizePx, std::max(kPreviewMinSizePx, maxSize));

    float left = m_GestureStartLeft;
    float top = m_GestureStartTop;

    switch (m_ResizeAnchor)
    {
    case ResizeAnchor::Left:
        left = m_GestureStartLeft + m_GestureStartWidth - size;
        break;
    case ResizeAnchor::Right:
    case ResizeAnchor::Bottom:
        break;
    case ResizeAnchor::None:
        return;
    }

    ApplyPreviewBounds(left, top, size, size);
}

void MaterialGraphPreviewHost::SetVisibleForMaterialGraph(bool visible)
{
    const bool wasActive = m_Active;
    m_Active = visible;
    Overrides().Set(Style::Display, visible ? DisplayMode::Flex : DisplayMode::None);
    if (visible && !wasActive)
    {
        // The thumbnail orbit this replaces started turning on its own when the
        // preference allowed it; keep that.
        m_OrbitPaused = false;
        m_OrbitVelocity = ModelThumbnailHandler::GetRotatePreviewsEnabled()
                              ? kDefaultOrbitSpeed * ModelThumbnailHandler::GetOrbitSpeedScale()
                              : 0.f;
    }
    if (!visible)
    {
        m_IsMovingPreview = false;
        m_IsResizingPreview = false;
        m_ResizeAnchor = ResizeAnchor::None;
        m_IsDragging = false;
        ClearPreview();
    }
    MarkDirty(LayoutDirty | VisualDirty);
}

void MaterialGraphPreviewHost::ClearPreview()
{
    if (m_Image && m_Image->GetOwnerManager() != nullptr)
        UI::Layout::ClearBackgroundOverride(*m_Image);
    m_BoundResource.clear();
    m_PreviewMaterialPath.clear();
    m_PreviewMaterialGuid = GUID{};
    m_PreviewSourceHash = 0;
}

void MaterialGraphPreviewHost::SetIblEnabled(bool enabled)
{
    // The atlas owns the IBL keyword for every cell, this one included, so the
    // flag is only remembered here for the toolbar's benefit.
    m_IblEnabled = enabled;
}

bool MaterialGraphPreviewHost::WritePreviewAssets(const Graph::Model& model,
                                                  const std::string& compiledSurfaceBody,
                                                  const std::filesystem::path& graphSourcePath)
{
    if (!m_Context)
    {
        Logger::Log::Warning("GraphPreview: editor assets root is not set");
        return false;
    }
    if (compiledSurfaceBody.empty())
        return false;

    const Editor::GraphPreviewCacheSource cacheSource =
        Editor::ResolveGraphPreviewCacheSource(m_Context);
    const std::filesystem::path cacheRoot = cacheSource.Root;
    if (cacheRoot.empty())
    {
        Logger::Log::Warning("GraphPreview: editor assets root is not set");
        return false;
    }

    const std::filesystem::path cacheDir = cacheRoot / "Generated" / "GraphPreview";
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec)
    {
        Logger::Log::Warning("GraphPreview: failed to create '{}'", cacheDir.string());
        return false;
    }

    const std::filesystem::path builtGlslPath = cacheDir / "live_preview_built.glsl";
    const std::filesystem::path matPath = cacheDir / "live_preview.material";
    /* The preview projection, so a constant's value is a user-lane read: the
       surface then stays byte-identical across a scrub and only the uniform
       moves. */
    const Editor::MaterialGraphPreviewModel preview = Editor::MakeMaterialGraphPreviewModel(model);
    std::string surfaceSource;
    std::vector<std::string> materializeErrors;
    if (!Engine::Renderer::MaterializeShaderGraphSurfaceFromModel(
            preview.Model, "Graph Live Preview", builtGlslPath, surfaceSource, materializeErrors))
    {
        if (!materializeErrors.empty())
            Logger::Log::Warning("GraphPreview: {}", materializeErrors.front());
        else
            Logger::Log::Warning("GraphPreview: failed to materialize '{}'",
                                 builtGlslPath.string());
        return false;
    }

    /* An edit that leaves the surface identical — a node moved, the canvas
       panned, a parameter value scrubbed — must not reach the shader path: a
       rewrite re-arms the file watcher, and the reload plus base compile behind
       it is a main-thread stall on every keystroke. Values travel through
       RefreshMaterialProperties instead. */
    const std::uint64_t sourceHash = Hashing::Fnv1a64(surfaceSource);
    if (sourceHash == m_PreviewSourceHash && !m_PreviewMaterialGuid.IsNull())
    {
        m_PreviewMaterialPath = matPath;
        /* The surface has not moved, but the values it reads may have: they live
           in uniform lanes now, so they still have to reach the runtime. Skipping
           this is what left the preview sphere a compile behind the node plates,
           which push their values on every sync. */
        MaterialDocument unchangedDoc = BuildPreviewMaterialDocument(preview, graphSourcePath);
        unchangedDoc.surfaceShader = builtGlslPath.string();
        Editor::SyncGraphPreviewMaterial(m_Context, matPath, unchangedDoc,
                                         Editor::GraphPreviewCompile::PropertiesOnly,
                                         cacheSource.Alias);
        return true;
    }

    if (!ShaderGraph::WriteGraphFileTextIfChanged(builtGlslPath, surfaceSource))
    {
        Logger::Log::Warning("GraphPreview: failed to write '{}'", builtGlslPath.string());
        return false;
    }

    MaterialDocument matDoc = BuildPreviewMaterialDocument(preview, graphSourcePath);
    // Store the surface shader as an ABSOLUTE path. The thumbnail/preview variant
    // is compiled through a synthetic cache material whose directory is not the
    // GraphPreview dir, so a relative "live_preview_built.glsl" resolves to the
    // wrong place, the #include yields no EvaluateSurface, the fragment fails to
    // compile, and the preview falls back to the magenta missing-texture.
    matDoc.surfaceShader = builtGlslPath.string();

    if (!Editor::WriteGraphPreviewMaterialFile(matPath, matDoc))
    {
        Logger::Log::Warning("GraphPreview: failed to write '{}'", matPath.string());
        return false;
    }

    /* Async, like the node previews. A base-shader compile is ~750ms of shaderc
       on this material, and taking it inline froze the frame thread on every
       structural edit. The thumbnail request retries across frames, so the
       sphere goes stale for a beat instead of the editor going still. */
    const GUID materialGuid = Editor::SyncGraphPreviewMaterial(
        m_Context, matPath, matDoc, Editor::GraphPreviewCompile::Shader, cacheSource.Alias);
    if (materialGuid.IsNull())
        return false;

    m_PreviewMaterialPath = matPath;
    m_PreviewMaterialGuid = materialGuid;
    m_PreviewSourceHash = sourceHash;
    return true;
}

void MaterialGraphPreviewHost::RefreshMaterialProperties(const Graph::Model& model)
{
    if (!m_Active || m_PreviewMaterialPath.empty() || !m_Context)
        return;
    if (!std::filesystem::exists(m_PreviewMaterialPath))
        return;

    MaterialDocument matDoc =
        BuildPreviewMaterialDocument(Editor::MakeMaterialGraphPreviewModel(model), {});
    // Absolute surface path (see WritePreviewAssets) so the synthetic
    // thumbnail-variant build resolves the #include correctly.
    matDoc.surfaceShader = (m_PreviewMaterialPath.parent_path() / "live_preview_built.glsl").string();
    // Property drags update only runtime constants; touching the cache file here
    // wakes asset hot-reload on every color-picker tick.

    const Editor::GraphPreviewCacheSource cacheSource =
        Editor::ResolveGraphPreviewCacheSource(m_Context);
    const GUID materialGuid =
        Editor::SyncGraphPreviewMaterial(m_Context, m_PreviewMaterialPath, matDoc,
                                         Editor::GraphPreviewCompile::PropertiesOnly,
                                         cacheSource.Alias);
    if (!materialGuid.IsNull())
        m_PreviewMaterialGuid = materialGuid;
}

void MaterialGraphPreviewHost::RefreshFromModel(const Graph::Model& model,
                                                const std::string& compiledSurfaceBody,
                                                const std::filesystem::path& graphSourcePath)
{
    EnsureUi();
    if (!m_Active || !m_Context)
        return;

    m_GraphSourcePath = graphSourcePath;
    WritePreviewAssets(model, compiledSurfaceBody, graphSourcePath);
}

void MaterialGraphPreviewHost::OnParentLayoutReady()
{
    if (!m_Active)
        return;
    if (!m_HasPreviewBounds)
        EnsurePreviewBoundsInitialized();
}

void MaterialGraphPreviewHost::ApplyPreviewBinding(const Editor::GraphNodePreviewBinding& binding)
{
    EnsureUi();
    if (!m_Image || m_Image->GetOwnerManager() == nullptr)
        return;

    /* This panel is not a node, so it publishes the plate variable for its own
       subtree the way GraphPortedNode does for the plates inside a node. */
    Overrides().SetCustomColor(HashStringId("--graph-plate-bg"),
                               NodeColorSettings::GetNodePlateColorArgb());

    if (binding.Resource.empty())
    {
        if (!m_BoundResource.empty())
        {
            UI::Layout::ClearBackgroundOverride(*m_Image);
            m_BoundResource.clear();
        }
        return;
    }

    if (binding.Resource == m_BoundResource && binding.SizeXPercent == m_BoundSizeXPercent
        && binding.SizeYPercent == m_BoundSizeYPercent
        && binding.PosXPercent == m_BoundPosXPercent
        && binding.PosYPercent == m_BoundPosYPercent)
    {
        return;
    }

    UI::Layout::SetBackgroundResourceName(*m_Image, binding.Resource);
    /* Sprite addressing into the panel's preview atlas, exactly as a node plate
       does it. Must be set AFTER SetBackgroundResourceName, which force-overrides
       background-size to Contain. */
    m_Image->Overrides()
        .Set(Style::BackgroundSize,
             BackgroundSizeValue{BackgroundSizeMode::Explicit, binding.SizeXPercent, true,
                                 binding.SizeYPercent, true})
        .Set(Style::BackgroundPosition,
             BackgroundPositionValue{binding.PosXPercent, true, binding.PosYPercent, true});
    m_BoundResource = binding.Resource;
    m_BoundSizeXPercent = binding.SizeXPercent;
    m_BoundSizeYPercent = binding.SizeYPercent;
    m_BoundPosXPercent = binding.PosXPercent;
    m_BoundPosYPercent = binding.PosYPercent;
}

void MaterialGraphPreviewHost::Update(float dt)
{
    (void)dt;
    if (!m_Active || !m_Image || m_Image->GetOwnerManager() == nullptr)
        return;

    /* Per frame, not per second: the drag sensitivity and the released spin
       speed are both authored as radians per frame, and the thumbnail orbit
       they replace integrated the same way. */
    if (!m_IsDragging && !m_OrbitPaused)
        m_OrbitYaw = WrapAngle(m_OrbitYaw + m_OrbitVelocity);

    if (!m_IsMovingPreview && !m_IsResizingPreview)
    {
        if (!m_HasPreviewBounds)
        {
            EnsurePreviewBoundsInitialized();
        }
        else if (UIElement* bodyEl = GetParent())
        {
            const float bodyW = bodyEl->GetLayoutWidth();
            const float bodyH = bodyEl->GetLayoutHeight();
            if (bodyW > 1.0f && bodyH > 1.0f &&
                (std::fabs(bodyW - m_LastParentWidth) > 0.5f ||
                 std::fabs(bodyH - m_LastParentHeight) > 0.5f))
            {
                const float preferredSize = m_PreferredPreviewSize > 0.0f
                                                ? m_PreferredPreviewSize
                                                : m_PreviewSize;
                if (m_UserPlacedPreview)
                {
                    ApplyPreviewBounds(m_PreferredPreviewLeft, m_PreferredPreviewTop,
                                       preferredSize, preferredSize);
                }
                else
                {
                    const float maxSize = std::max(
                        kPreviewMinSizePx, std::min(bodyW, bodyH) - kPreviewMarginPx * 2.0f);
                    const float size = std::clamp(preferredSize, kPreviewMinSizePx, maxSize);
                    ApplyPreviewBounds(bodyW - size - kPreviewMarginPx, kPreviewMarginPx, size, size);
                }
            }
        }
    }

}

} // namespace GameEngine
