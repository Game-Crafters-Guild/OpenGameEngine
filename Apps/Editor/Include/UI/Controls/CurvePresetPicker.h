#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
namespace Platform
{
class Window;
}
class RenameLayoutModal;
namespace CurvePresets
{
struct CurvePreset;
}

/// A grid of curve-shape presets: the curated built-ins plus the user's saved library
/// (see CurvePresets). Picking a thumbnail applies that shape to the host curve via the
/// pick callback; the host keeps ownership of the curve and its undo. The trailing "+" slot
/// asks the host to capture the current curve as a new user preset (the host prompts for a name
/// and calls CurvePresets::UserPresetLibrary::Add). Right-clicking a user preset exposes
/// duplicate, rename, and delete commands for the shared library.
///
/// It owns no curve; hosts embed it and wire picked presets to their CurveField.
class CurvePresetPicker : public UIElement
{
  public:
    CurvePresetPicker();
    ~CurvePresetPicker() override;

    void SetOnPick(std::function<void(const CurvePresets::CurvePreset&)> callback)
    {
        m_OnPick = std::move(callback);
    }
    void SetOnRequestSave(std::function<void()> callback) { m_OnRequestSave = std::move(callback); }
    void SetContextMenuWindow(Platform::Window* window) { m_ContextMenuWindow = window; }

    /// Re-measure after the user library changed elsewhere (save/rename/delete from another editor).
    void Refresh();

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

  private:
    struct Rect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    int BuiltinCount() const;
    int UserCount() const;
    int PresetCount() const { return BuiltinCount() + UserCount(); }
    int SaveSlotIndex() const { return PresetCount(); }
    int TotalCount() const { return PresetCount() + 1; }
    bool IsSaveSlot(int index) const { return index == SaveSlotIndex(); }
    bool IsUserPreset(int index) const { return index >= BuiltinCount() && index < PresetCount(); }

    bool CellRect(int index, Rect& out) const;
    static bool PointIn(const Rect& r, float x, float y);
    int HitTestCell(float globalX, float globalY) const;
    void UpdateHeight();
    void SetHoveredCell(int index);
    void ShowPresetContextMenu(int presetIndex, float x, float y);
    RenameLayoutModal* EnsureRenameModal();
    void ShowRenamePresetModal(int userIndex);

    int m_Hover = -1; // hovered preset index, or -1
    int m_ActivePreset = -1; // last picked preset index, or -1

    std::function<void(const CurvePresets::CurvePreset&)> m_OnPick;
    std::function<void()> m_OnRequestSave;
    Platform::Window* m_ContextMenuWindow = nullptr;
    RenameLayoutModal* m_RenameModal = nullptr;
    std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
};

} // namespace GameEngine
