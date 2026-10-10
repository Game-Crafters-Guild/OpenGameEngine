#pragma once

#include "AssetCore/GUID.h"
#include "Graph/GraphModel.h"
#include "ShaderGraph/GraphNodePreviewBinding.h"
#include "UI/UIElement.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine {

class Label;
struct EditorContext;

/** Top-right 3D material preview sphere for the node graph editor. */
class MaterialGraphPreviewHost : public UIElement {
public:
    MaterialGraphPreviewHost();

    void SetContext(const EditorContext* ctx);

    void SetVisibleForMaterialGraph(bool visible);
    void RefreshFromModel(const Graph::Model& model,
                          const std::string& compiledSurfaceBody,
                          const std::filesystem::path& graphSourcePath = {});
    void RefreshMaterialProperties(const Graph::Model& model);
    /** False until a RefreshFromModel has produced the live preview material —
        the signal that an earlier pass found nothing to work with. */
    bool HasPreviewMaterial() const { return !m_PreviewMaterialGuid.IsNull(); }
    const GUID& PreviewMaterialGuid() const { return m_PreviewMaterialGuid; }
    /** Where the preview sphere has been spun to, in radians. */
    float OrbitYaw() const { return m_OrbitYaw; }

    /** Paints this panel from the preview atlas cell the controller assigned it,
        the same way a node plate paints its own. */
    void ApplyPreviewBinding(const Editor::GraphNodePreviewBinding& binding);
    /** Called when the node graph body has a stable layout, so the panel can
        settle its own bounds. */
    void OnParentLayoutReady();
    void Update(float dt);
    void ClearPreview();
    void SetIblEnabled(bool enabled);

private:
    enum class ResizeAnchor {
        None,
        Left,
        Right,
        Bottom
    };

    void EnsureUi();
    bool EnsurePreviewBoundsInitialized();
    void ApplyPreviewBounds(float left, float top, float width, float height);
    void AddMoveHandle(const char* className);
    /** Light the PREVIEW title while the move strip is hovered or dragged. */
    void SetMoveHint(bool on);
    void AddResizeZone(ResizeAnchor anchor, const char* className);
    void BeginMove(float x, float y);
    void UpdateMove(float x, float y);
    void ToggleCollapsed();
    void ApplyCollapsedState();
    void BeginResize(ResizeAnchor anchor, float x, float y);
    void UpdateResize(float x, float y);
    bool WritePreviewAssets(const Graph::Model& model,
                            const std::string& compiledSurfaceBody,
                            const std::filesystem::path& graphSourcePath);

    const EditorContext* m_Context = nullptr;
    UIElement* m_Image = nullptr;
    Label* m_Title = nullptr;

    std::filesystem::path m_PreviewMaterialPath;
    std::filesystem::path m_GraphSourcePath;
    GUID m_PreviewMaterialGuid{};
    /** Hash of the surface GLSL the live preview material was last built from:
        an edit that leaves it identical skips the reload and base compile. */
    std::uint64_t m_PreviewSourceHash = 0;
    /** Mirrors the controller's default; the toolbar pushes changes to both. */
    bool m_IblEnabled = false;
    bool m_Active = false;
    bool m_IsDragging = false;
    bool m_DidDrag = false;
    float m_LastDragX = 0.f;
    float m_LastDragDeltaX = 0.f;
    float m_LastDragOrbitSign = 0.f;

    bool m_IsMovingPreview = false;
    bool m_IsResizingPreview = false;
    bool m_MoveDidDrag = false;
    bool m_Collapsed = false;
    bool m_HasPreviewBounds = false;
    bool m_UserPlacedPreview = false;
    ResizeAnchor m_ResizeAnchor = ResizeAnchor::None;
    float m_PreviewLeft = 0.f;
    float m_PreviewTop = 0.f;
    float m_PreviewSize = 0.f;
    float m_PreferredPreviewLeft = 0.f;
    float m_PreferredPreviewTop = 0.f;
    float m_PreferredPreviewSize = 0.f;
    float m_LastParentWidth = 0.f;
    float m_LastParentHeight = 0.f;
    float m_GestureStartX = 0.f;
    float m_GestureStartY = 0.f;
    float m_GestureStartLeft = 0.f;
    float m_GestureStartTop = 0.f;
    float m_GestureStartWidth = 0.f;
    float m_GestureStartHeight = 0.f;
    std::chrono::steady_clock::time_point m_MoveHandleLastClickTime{};

    /** The atlas cell this panel currently paints, and the sprite rect into it.
        Kept so a rebind that changes nothing skips the style write. */
    std::string m_BoundResource;
    float m_BoundSizeXPercent = 0.f;
    float m_BoundSizeYPercent = 0.f;
    float m_BoundPosXPercent = 0.f;
    float m_BoundPosYPercent = 0.f;

    /** Orbit, in radians per frame, integrated locally now that the sphere is an
        atlas cell rather than a thumbnail slot. */
    float m_OrbitYaw = 0.f;
    float m_OrbitVelocity = 0.f;
    bool m_OrbitPaused = false;
};

} // namespace GameEngine
