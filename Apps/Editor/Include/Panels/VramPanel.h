#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Types/StringId.h"
#include "UI/Controls/AxisModel.h"  // SortDirection
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/ListView.h"  // IListDataProvider, ListId
#include "UI/Interaction/Selection.h"

namespace GameEngine {

namespace Rendering {
    class IDevice;
    namespace RenderGraph { class RGFrame; }
}

struct AssetLoadHandle;
struct UIEvent;
class Button;
class Label;
class TableView;
class UIElement;

// Debugger VRAM panel: device allocation totals + a sortable, column-resizable
// table of GPU resources, backed by the reusable TableView control. The list has
// two sources (live device allocations, or this frame's render-graph resources),
// toggled from the toolbar.
class VramPanel : public DockPanel {
  public:
    std::string_view DeclaredTabIconClass() const override { return "hardware-chip-icon"; }

    VramPanel();
    ~VramPanel() override;

    // Called by the editor app every frame. `frame` is the main window's live
    // render graph (may be null on headless / pre-first-frame); it sources the
    // RenderGraph list + the "RG Resources" count.
    void Update(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame);
    void OnPostLayout() override;

  private:
    // GpuAllocations: live device allocations by name + real bytes (the by-asset
    // view). RenderGraph: this frame's render-graph resources.
    enum class ListSource { GpuAllocations, RenderGraph };

    struct Entry {
        std::string Name;
        std::string Type;     // "Texture", "Buffer"
        std::string Details;  // "1920x1080 R8G8B8A8_UNORM" or "4096 B"
        std::string Lifetime; // "GPU" / "Transient" / "Imported"
        std::uint64_t SizeBytes = 0;
    };

    // Borrows the panel's sorted view of the entries; the table renders directly
    // against this. Version bumps on every data/sort change so the virtualized
    // body rebinds all visible rows.
    class RowProvider : public IListDataProvider {
      public:
        explicit RowProvider(const std::vector<Entry>& rows) : m_Rows(rows) {}

        void BumpVersion() { ++m_Version; }

        int GetItemCount() const override { return static_cast<int>(m_Rows.size()); }
        ListId GetItemId(int index) const override;
        float GetItemHeight(int index) const override;
        void ConsumeChanges(std::uint64_t sinceVersion, ListChangeSet& out) const override;

      private:
        const std::vector<Entry>& m_Rows;
        std::uint64_t m_Version = 1;
    };

    void BuildUI();
    void ConfigureTable();
    void RefreshList(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame);
    void SortEntries(StringId key, SortDirection dir);
    void UpdateTotals(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame);
    void CycleListSource();
    void BindCell(UIElement* cell, int colIndex, int rowIndex);
    void SaveColumnLayout();
    void UpdateEmptyState();
    void LoadAndAttachPanelStyle();

    // UI roots
    Button* m_SourceButton = nullptr;
    Label* m_TotalVramLabel = nullptr;
    Label* m_AllocationsLabel = nullptr;
    Label* m_BuffersLabel = nullptr;
    Label* m_TexturesLabel = nullptr;
    Label* m_ResourcesLabel = nullptr;
    Label* m_DeviceInfoLabel = nullptr;
    Label* m_EmptyLabel = nullptr;
    TableView* m_Table = nullptr;

    // State
    std::vector<Entry> m_SortedEntries; // table-facing, sorted view; RowProvider borrows this
    std::unique_ptr<RowProvider> m_RowProvider;
    UI::Interaction::SelectionModel m_Selection;  // single-row highlight, survives refresh via stable ids
    ListSource m_ListSource = ListSource::GpuAllocations;
    std::size_t m_LastResourceSignature = 0;
    int m_FrameCounter = 0;

    const std::string m_ColumnLayoutKey = "ui.vram.columns";

    // Cached totals to avoid redundant SetText calls every frame
    std::uint64_t m_LastAllocatedBytes = static_cast<std::uint64_t>(-1);
    std::size_t m_LastAllocationCount = static_cast<std::size_t>(-1);
    std::size_t m_LastBufferCount = static_cast<std::size_t>(-1);
    std::size_t m_LastImageCount = static_cast<std::size_t>(-1);
    std::size_t m_LastResourceCount = static_cast<std::size_t>(-1);
    std::string m_CachedHardwareDescription;

    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
};

} // namespace GameEngine
