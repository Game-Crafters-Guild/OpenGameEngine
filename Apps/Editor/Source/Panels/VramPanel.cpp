#include "Panels/VramPanel.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Editor/Settings/AxisLayoutSerializer.h"
#include "Editor/Settings/SettingsStore.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TableView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine {

namespace {

constexpr float kRowHeightPx = 20.0f;

constexpr StringId kColName     = "vram.col.name"_sid;
constexpr StringId kColType     = "vram.col.type"_sid;
constexpr StringId kColLifetime = "vram.col.lifetime"_sid;
constexpr StringId kColDetails  = "vram.col.details"_sid;

// CSS colour class per data column, indexed by column position (the trailing fill
// track has no class — the binder never sees it).
constexpr const char* kCellColorClass[] = {
    "vram-cell-name", "vram-cell-type", "vram-cell-lifetime", "vram-cell-details"};
constexpr int kColorClassCount = static_cast<int>(std::size(kCellColorClass));

std::string FormatBytes(std::uint64_t bytes)
{
    char buf[64];
    const double b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024.0 * 1024.0)
        std::snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024.0 * 1024.0)
        std::snprintf(buf, sizeof(buf), "%.2f MB", b / (1024.0 * 1024.0));
    else if (b >= 1024.0)
        std::snprintf(buf, sizeof(buf), "%.2f KB", b / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string FormatCount(std::size_t n)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu", n);
    return buf;
}

} // namespace

ListId VramPanel::RowProvider::GetItemId(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Rows.size()))
        return 0;
    const Entry& e = m_Rows[static_cast<std::size_t>(index)];
    // Stable across rebuilds so the selection model re-resolves the same row.
    return HashStringId(e.Type + "|" + e.Name);
}

float VramPanel::RowProvider::GetItemHeight(int /*index*/) const
{
    return kRowHeightPx;
}

void VramPanel::RowProvider::ConsumeChanges(std::uint64_t sinceVersion, ListChangeSet& out) const
{
    out.Version = m_Version;
    out.Ids.clear();
    out.Kind = (sinceVersion < m_Version) ? ChangeSetKind::All : ChangeSetKind::None;
}

VramPanel::VramPanel()
    : DockPanel("VRAM")
{
    AddClass("vram-panel");
    m_RowProvider = std::make_unique<RowProvider>(m_SortedEntries);
    BuildUI();
}

VramPanel::~VramPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    // The TableView (and its ListView body) is destroyed after our members, so drop
    // its borrowed provider pointer before m_RowProvider / m_SortedEntries go away.
    if (m_Table)
        m_Table->Body().SetDataProvider(nullptr);
}

void VramPanel::BuildUI()
{
    // Toolbar: source toggle + device-info label. Sorting now lives on the table headers.
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("vram-toolbar");

    auto sourceBtn = std::make_unique<Button>();
    sourceBtn->AddClass("small");
    sourceBtn->AddClass("secondary");
    sourceBtn->AddClass("vram-btn-source");
    sourceBtn->SetText("Source: GPU");
    sourceBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { CycleListSource(); });
    m_SourceButton = sourceBtn.get();
    toolbar->AddChild(std::move(sourceBtn));

    auto deviceInfo = std::make_unique<Label>();
    deviceInfo->AddClass("vram-toolbar-device-info");
    deviceInfo->SetText("—");
    m_DeviceInfoLabel = deviceInfo.get();
    toolbar->AddChild(std::move(deviceInfo));

    AddChild(std::move(toolbar));

    // Summary cards
    auto summary = std::make_unique<UIElement>();
    summary->AddClass("vram-summary");
    {
        auto addCard = [&summary](const char* label, Label*& outValue) {
            auto card = std::make_unique<UIElement>();
            card->AddClass("vram-summary-card");
            auto lbl = std::make_unique<Label>();
            lbl->AddClass("vram-summary-label");
            lbl->SetText(label);
            card->AddChild(std::move(lbl));
            auto val = std::make_unique<Label>();
            val->AddClass("vram-summary-value");
            val->SetText("—");
            outValue = val.get();
            card->AddChild(std::move(val));
            summary->AddChild(std::move(card));
        };
        addCard("Total VRAM", m_TotalVramLabel);
        addCard("Allocations", m_AllocationsLabel);
        addCard("Buffers", m_BuffersLabel);
        addCard("Textures", m_TexturesLabel);
        addCard("RG Resources", m_ResourcesLabel);
    }
    AddChild(std::move(summary));

    auto table = std::make_unique<TableView>();
    m_Table = table.get();
    AddChild(std::move(table));

    // Source-specific empty hint, shown (and the table hidden) when the list is empty.
    auto empty = std::make_unique<Label>();
    empty->AddClass("vram-empty");
    empty->Overrides().Set(Style::Display, DisplayMode::None);
    m_EmptyLabel = empty.get();
    AddChild(std::move(empty));

    ConfigureTable();
}

void VramPanel::ConfigureTable()
{
    // Fixed-width, resizable, sortable columns + a trailing fill track. Details is
    // right-aligned and carries the byte size (the default sort).
    std::vector<TrackDef> columns;
    auto addCol = [&columns](StringId key, const char* title, float size, float minSize,
                             TrackDef::Align align) {
        TrackDef t;
        t.Key       = key;
        t.Title     = title;
        t.Size      = size;
        t.MinSize   = minSize;
        t.Sortable  = true;
        t.Resizable = true;
        t.Alignment = align;
        columns.push_back(std::move(t));
    };
    addCol(kColName,     "Name",     380.0f, 120.0f, TrackDef::Align::Start);
    addCol(kColType,     "Type",      90.0f,  60.0f, TrackDef::Align::Start);
    addCol(kColLifetime, "Lifetime", 124.0f,  70.0f, TrackDef::Align::Start);
    addCol(kColDetails,  "Details",  330.0f, 120.0f, TrackDef::Align::End);
    columns.push_back(TrackDef::MakeFill());
    m_Table->SetColumns(std::move(columns));

    // Restore persisted column sizes before the first layout.
    {
        Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
        prefs.Load();
        Editor::LoadAxisLayout(prefs, m_ColumnLayoutKey, m_Table->Columns());
        m_Table->RefreshColumnLayout();
    }

    m_Table->SetRowProvider(m_RowProvider.get());
    m_Table->SetCellBinder(
        [this](UIElement* cell, int colIndex, const TrackDef&, ListId, int rowIndex, IListDataProvider*) {
            BindCell(cell, colIndex, rowIndex);
        });
    m_Table->SetOnSort([this](StringId key, SortDirection dir) {
        SortEntries(key, dir);
        m_RowProvider->BumpVersion();
        m_Table->Refresh();
    });
    m_Table->SetOnLayoutChanged([this]() { SaveColumnLayout(); });

    // Default sort: largest resources first (Details column carries the byte size).
    m_Table->SetSortIndicator(kColDetails, SortDirection::Descending);

    // Single-row selection highlight + keyboard nav. The model + stable row ids keep
    // the selection across the periodic Refresh().
    ListView& body = m_Table->Body();
    body.SetSelectionModel(&m_Selection);
    body.SetFocusable(true);
    body.RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Mods != 0 || (e.Key != Input::kKeyCode_Up && e.Key != Input::kKeyCode_Down))
            return;
        ListView& b = m_Table->Body();
        const int count = b.GetItemCount();
        if (count <= 0)
            return;
        int index = b.GetSelectedIndex();
        if (index < 0)
            index = 0;
        index += (e.Key == Input::kKeyCode_Up) ? -1 : 1;
        index = std::clamp(index, 0, count - 1);
        b.SetSelectedIndexKeyboard(index, /*extendRange*/ false, /*scrollIntoView*/ true);
        e.Stop();
    });
}

void VramPanel::BindCell(UIElement* cell, int colIndex, int rowIndex)
{
    if (!cell || rowIndex < 0 || rowIndex >= static_cast<int>(m_SortedEntries.size()))
        return;

    Label* label = nullptr;
    if (cell->GetChildren().empty())
    {
        // A cell's column is fixed for its lifetime, so the colour class is set once.
        auto lbl = std::make_unique<Label>();
        if (colIndex >= 0 && colIndex < kColorClassCount)
            lbl->AddClass(kCellColorClass[colIndex]);
        label = lbl.get();
        cell->AddChild(std::move(lbl));
    }
    else
    {
        label = dynamic_cast<Label*>(cell->GetChildren()[0].get());
    }
    if (!label)
        return;

    const Entry& e = m_SortedEntries[static_cast<std::size_t>(rowIndex)];
    switch (colIndex)
    {
    case 0: label->SetText(e.Name);     break;
    case 1: label->SetText(e.Type);     break;
    case 2: label->SetText(e.Lifetime); break;
    case 3: label->SetText(e.Details);  break;
    default: break;
    }
}

void VramPanel::SortEntries(StringId key, SortDirection dir)
{
    const bool ascending = (dir != SortDirection::Descending);

    // Compares two entries on the active column. Details sorts by raw byte size,
    // not its formatted string.
    auto less = [key](const Entry& a, const Entry& b) {
        if (key == kColDetails)
            return a.SizeBytes < b.SizeBytes;
        if (key == kColType)
            return a.Type < b.Type;
        if (key == kColLifetime)
            return a.Lifetime < b.Lifetime;
        return a.Name < b.Name;
    };

    // Descending swaps operands rather than negating: negating breaks strict weak
    // ordering on equal keys (many equal sizes), which trips std::sort's debug assert.
    std::sort(m_SortedEntries.begin(), m_SortedEntries.end(),
              [&less, ascending](const Entry& a, const Entry& b) {
                  return ascending ? less(a, b) : less(b, a);
              });
}

void VramPanel::Update(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame)
{
    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return;

    // Refresh device totals every frame — they're cheap reads.
    UpdateTotals(device, frame);

    // Rebuild the resource list only periodically (DOM churn); the signature check
    // in RefreshList also no-ops when the resource set is unchanged.
    if ((++m_FrameCounter & 15) != 0)
        return;
    RefreshList(device, frame);
}

void VramPanel::UpdateTotals(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame)
{
    if (device)
    {
        const auto allocBytes = device->DebugGetAllocatedBytes();
        if (m_TotalVramLabel && allocBytes != m_LastAllocatedBytes)
        {
            m_LastAllocatedBytes = allocBytes;
            m_TotalVramLabel->SetText(FormatBytes(allocBytes));
        }

        const auto allocCount = device->DebugGetAllocationCount();
        if (m_AllocationsLabel && allocCount != m_LastAllocationCount)
        {
            m_LastAllocationCount = allocCount;
            m_AllocationsLabel->SetText(FormatCount(allocCount));
        }

        const auto bufCount = device->DebugGetBufferRegistryCount();
        if (m_BuffersLabel && bufCount != m_LastBufferCount)
        {
            m_LastBufferCount = bufCount;
            m_BuffersLabel->SetText(FormatCount(bufCount));
        }

        const auto imgCount = device->DebugGetImageRegistryCount();
        if (m_TexturesLabel && imgCount != m_LastImageCount)
        {
            m_LastImageCount = imgCount;
            m_TexturesLabel->SetText(FormatCount(imgCount));
        }

        if (m_DeviceInfoLabel && m_CachedHardwareDescription.empty())
        {
            std::string hw = device->GetHardwareDescription();
            m_CachedHardwareDescription = hw.empty() ? "(unknown device)" : std::move(hw);
            m_DeviceInfoLabel->SetText(m_CachedHardwareDescription);
        }
    }
    else
    {
        if (m_LastAllocatedBytes != static_cast<std::uint64_t>(-1))
        {
            m_LastAllocatedBytes = static_cast<std::uint64_t>(-1);
            m_LastAllocationCount = static_cast<std::size_t>(-1);
            m_LastBufferCount = static_cast<std::size_t>(-1);
            m_LastImageCount = static_cast<std::size_t>(-1);
            m_CachedHardwareDescription.clear();
            if (m_TotalVramLabel) m_TotalVramLabel->SetText("—");
            if (m_AllocationsLabel) m_AllocationsLabel->SetText("—");
            if (m_BuffersLabel) m_BuffersLabel->SetText("—");
            if (m_TexturesLabel) m_TexturesLabel->SetText("—");
            if (m_DeviceInfoLabel) m_DeviceInfoLabel->SetText("—");
        }
    }

    // RG-resource count from the last compiled graph. At panel-update time the main
    // window's RGFrame holds the previous frame's declarations (the next BeginFrame,
    // which clears them, hasn't run yet), so this is a stable read.
    if (m_ResourcesLabel)
    {
        const std::size_t count = frame ? frame->Graph().ResourceCount() : static_cast<std::size_t>(-1);
        if (count != m_LastResourceCount)
        {
            m_LastResourceCount = count;
            m_ResourcesLabel->SetText(count == static_cast<std::size_t>(-1) ? "—" : FormatCount(count));
        }
    }
}

void VramPanel::RefreshList(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame* frame)
{
    namespace RG = Rendering::RenderGraph;

    std::vector<Entry> next;
    if (m_ListSource == ListSource::GpuAllocations)
    {
        // Live device allocations by name + real bytes (Godot's Video RAM view).
        if (device)
        {
            device->DebugEnumerateResources([&next](const Rendering::DebugResourceInfo& r) {
                Entry e;
                e.Name = r.Name;
                e.SizeBytes = r.Bytes;
                e.Lifetime = "GPU";
                if (r.Type == Rendering::DebugResourceInfo::Kind::Buffer)
                {
                    e.Type = "Buffer";
                    e.Details = FormatBytes(r.Bytes);
                }
                else
                {
                    e.Type = "Texture";
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%ux%u %s  ·  %s", r.Width, r.Height,
                                  Rendering::ToString(r.Format), FormatBytes(r.Bytes).c_str());
                    e.Details = buf;
                }
                next.push_back(std::move(e));
            });
        }
    }
    else if (frame)
    {
        // This frame's render-graph resources. The graph is declared fresh each frame
        // and only cleared at the next BeginFrame, which runs after this editor-update
        // pass — so reading it here yields the previous frame's stable declarations.
        const RG::RGGraph& graph = frame->Graph();
        const std::size_t count = graph.ResourceCount();
        next.reserve(count);
        for (RG::RGResourceId id = 0; id < count; ++id)
        {
            // A declared-but-culled resource is never realized; skip it so the list
            // reflects what actually consumes VRAM this frame.
            if (!graph.IsResourceUsed(id))
                continue;

            const RG::RGResourceDesc& d = graph.ResourceDesc(id);
            // Acceleration-structure memory is the backend's, not the graph's
            // (its storage is reported with the scene AS pool).
            if (d.Kind == RG::RGResourceKind::AccelerationStructure)
                continue;
            const char* nameC = graph.ResourceName(id);

            Entry e;
            e.Name = nameC ? nameC : "(unnamed)";
            e.Lifetime = graph.IsExternal(id) ? "Imported" : "Transient";

            if (d.Kind == RG::RGResourceKind::Buffer)
            {
                e.Type = "Buffer";
                e.SizeBytes = d.SizeBytes;
                e.Details = FormatBytes(d.SizeBytes);
            }
            else
            {
                e.Type = "Texture";
                const auto fmt = static_cast<Rendering::TextureFormat>(d.Format);
                const std::uint64_t layers = d.ArrayLayers ? d.ArrayLayers : 1;
                const std::uint64_t bpp = Rendering::BytesPerPixel(fmt);
                e.SizeBytes = static_cast<std::uint64_t>(d.Width) * d.Height * bpp * layers;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%ux%u %s%s", d.Width, d.Height,
                              Rendering::ToString(fmt), layers > 1 ? "  (array)" : "");
                e.Details = buf;
            }
            next.push_back(std::move(e));
        }
    }

    // Cheap signature so the table rebuilds only when the resource set changes.
    std::size_t sig = next.size();
    for (const auto& e : next)
    {
        sig = sig * 1099511628211ull + std::hash<std::string>{}(e.Name);
        sig = sig * 1099511628211ull + std::hash<std::string>{}(e.Details);
    }
    if (sig == m_LastResourceSignature && (!m_SortedEntries.empty() == !next.empty()))
        return;
    m_LastResourceSignature = sig;
    m_SortedEntries = std::move(next);

    const auto [sortKey, sortDir] = m_Table->Sort();
    SortEntries(sortKey, sortDir);
    m_RowProvider->BumpVersion();
    // Un-hide the table before rebinding so the rows virtualize on the same layout
    // pass (the empty hint hides the table while there's nothing to show).
    UpdateEmptyState();
    m_Table->Refresh();
}

void VramPanel::CycleListSource()
{
    m_ListSource = (m_ListSource == ListSource::GpuAllocations) ? ListSource::RenderGraph
                                                               : ListSource::GpuAllocations;
    if (m_SourceButton)
        m_SourceButton->SetText(m_ListSource == ListSource::GpuAllocations ? "Source: GPU"
                                                                          : "Source: Render Graph");
    // Force the next RefreshList to rebuild against the new source (sentinel that no
    // real signature matches, so even an empty source repaints the list).
    m_LastResourceSignature = static_cast<std::size_t>(-1);
    m_FrameCounter = 15; // next Update tick passes the rebuild throttle
    m_SortedEntries.clear();
    m_Selection.Clear(); // a GPU-row id must not linger when switching to Render Graph
}

void VramPanel::UpdateEmptyState()
{
    const bool empty = m_SortedEntries.empty();
    if (m_EmptyLabel)
    {
        m_EmptyLabel->SetText(m_ListSource == ListSource::GpuAllocations
            ? "No GPU allocations reported (backend has no resource enumeration, or nothing is allocated yet)."
            : "No render graph resources. Play the scene or run a frame to populate.");
        m_EmptyLabel->Overrides().Set(Style::Display, empty ? DisplayMode::Flex : DisplayMode::None);
    }
    if (m_Table)
        m_Table->Overrides().Set(Style::Display, empty ? DisplayMode::None : DisplayMode::Flex);
}

void VramPanel::SaveColumnLayout()
{
    if (!m_Table)
        return;
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    Editor::SaveAxisLayout(prefs, m_ColumnLayoutKey, m_Table->Columns());
    prefs.Save();
}

void VramPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
}

void VramPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath =
        std::filesystem::path("UI") / "panels" / "VramPanel.css";
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, kAssetSourceAliasEditor);
    if (styleGuid.IsNull() || m_PanelStyleLoadHandle)
        return;
    m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
        am.LoadAsset(styleGuid,
                     [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                     {
                         if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                         {
                             post.Post([this]() { m_PanelStyleLoadHandle.reset(); });
                             return;
                         }
                         post.Post([this, styleGuid]()
                         {
                             UIManager* ui2 = GetOwnerManager();
                             if (!ui2)
                                 return;
                             auto& am2 = EngineCore::GetInstance().GetAssetManager();
                             auto a2 = am2.GetAsset(styleGuid);
                             if (a2 && a2->GetType() == AssetType::UIStyle)
                             {
                                 (void)ui2->AttachStyleToSubtreeFromAsset(
                                     this, *static_cast<UIStyleAsset*>(a2.get()));
                                 m_StyleAttached = true;
                             }
                         });
                     },
                     AssetLoadPriority::High));
}

} // namespace GameEngine
