#include "UI/Controls/CurvePresetPicker.h"

#include "EditorContextMenu/UIContextMenu.h"
#include "Panels/RenameLayoutModal.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Controls/CurvePresets.h"
#include "UI/UIPrimitive.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "UI/EditorIcons.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

// Logical sizing (multiplied by content scale when painting; used raw when hit-testing,
// mirroring CurveField's logical-vs-physical split).
constexpr float kPadXPx = 0.0f;
constexpr float kGridPadTopPx = 6.0f;
constexpr float kCellHPx = 50.0f;
constexpr float kGridPadBottomPx = 6.0f;
constexpr float kPadBottomPx = 8.0f;
constexpr int kColumns = 4;
constexpr const char kEditorPlusIconPath[] = "Icons/plus.png";
constexpr const char kEditorAssetMountAlias[] = "editor";
// The curve area inside a cell leaves a strip at the bottom for the name label.
constexpr float kCellLabelStripPx = 13.0f;
constexpr float kCellInnerPadPx = 5.0f;
constexpr float kCellLabelFontPx = 10.5f;
constexpr float kCellLabelTopMarginPx = 4.0f;
constexpr float kCellLabelBottomMarginPx = 4.0f;
constexpr float kSaveSlotPlusOffsetYPx = 4.0f;

const uint32_t kLabelColor = UI::PackColor(0.82f, 0.85f, 0.89f, 1.0f);
const uint32_t kCurveColor = UI::PackColor(0.36f, 0.70f, 1.0f, 1.0f);
const uint32_t kBezierAccent = UI::PackColor(0.46f, 0.86f, 0.48f, 1.0f);
const uint32_t kUserAccent = UI::PackColor(1.0f, 0.74f, 0.36f, 1.0f);

bool IsContextMenuButton(int button)
{
    return button == 1 || button == 2;
}

// Flatten the two preset sources into a single addressable list: built-ins first, then user.
const CurvePresets::CurvePreset* PresetAt(int index)
{
    const auto& builtin = CurvePresets::Builtin();
    if (index < static_cast<int>(builtin.size()))
        return &builtin[static_cast<size_t>(index)];
    const auto& user = CurvePresets::UserPresetLibrary::Get().Presets();
    const int u = index - static_cast<int>(builtin.size());
    if (u >= 0 && u < static_cast<int>(user.size()))
        return &user[static_cast<size_t>(u)];
    return nullptr;
}

std::string CopyNameForPreset(const std::string& name)
{
    const auto& presets = CurvePresets::UserPresetLibrary::Get().Presets();
    auto exists = [&presets](const std::string& candidate) {
        return std::any_of(presets.begin(), presets.end(),
                           [&](const CurvePresets::CurvePreset& preset) { return preset.Name == candidate; });
    };

    const std::string base = name.empty() ? "Preset" : name;
    std::string candidate = base + " Copy";
    if (!exists(candidate))
        return candidate;

    for (int ordinal = 2;; ++ordinal)
    {
        candidate = base + " Copy " + std::to_string(ordinal);
        if (!exists(candidate))
            return candidate;
    }
}

} // namespace

CurvePresetPicker::CurvePresetPicker()
{
    AddClass("curve-preset-picker");
    UpdateHeight();
}

CurvePresetPicker::~CurvePresetPicker()
{
    if (m_Alive)
        *m_Alive = false;
}

int CurvePresetPicker::BuiltinCount() const
{
    return static_cast<int>(CurvePresets::Builtin().size());
}

int CurvePresetPicker::UserCount() const
{
    return static_cast<int>(CurvePresets::UserPresetLibrary::Get().Presets().size());
}

bool CurvePresetPicker::CellRect(int index, Rect& out) const
{
    if (index < 0 || index >= TotalCount())
        return false;
    const float gridW = std::max(1.0f, GetLayoutWidth() - 2.0f * kPadXPx);
    const float cellW = gridW / static_cast<float>(kColumns);
    const int row = index / kColumns;
    const int col = index % kColumns;
    out.X = GetLayoutX() + kPadXPx + static_cast<float>(col) * cellW;
    out.Y = GetLayoutY() + kGridPadTopPx + static_cast<float>(row) * kCellHPx;
    out.W = cellW;
    out.H = kCellHPx;
    return true;
}

bool CurvePresetPicker::PointIn(const Rect& r, float x, float y)
{
    return x >= r.X && x <= r.X + r.W && y >= r.Y && y <= r.Y + r.H;
}

int CurvePresetPicker::HitTestCell(float globalX, float globalY) const
{
    const int total = TotalCount();
    for (int i = 0; i < total; ++i)
    {
        Rect r;
        if (CellRect(i, r) && PointIn(r, globalX, globalY))
            return i;
    }
    return -1;
}

void CurvePresetPicker::UpdateHeight()
{
    const int rows = (TotalCount() + kColumns - 1) / kColumns;
    const float h = kGridPadTopPx + static_cast<float>(rows) * kCellHPx + kGridPadBottomPx + kPadBottomPx;
    Overrides().Set(Style::Height, StyleLength::Px(h)).Set(Style::MinHeight, StyleLength::Px(h));
    // Mark layout + visual dirty; the manager's deferred relayout reflows the rows below us on the
    // next frame. (No parent poke / RequestRelayout needed -- the height change marking us
    // LayoutDirty is enough now that the update loop carries the frame's dirty hint into the
    // layout-signature gate.)
    MarkDirty(LayoutDirty | VisualDirty);
}

void CurvePresetPicker::Refresh()
{
    if (m_ActivePreset >= PresetCount())
        m_ActivePreset = -1;
    if (m_Hover >= TotalCount())
        SetHoveredCell(-1);
    UpdateHeight();
}

void CurvePresetPicker::SetHoveredCell(int index)
{
    if (index == m_Hover)
        return;
    m_Hover = index;
    MarkDirty(VisualDirty);
}

void CurvePresetPicker::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseLeave)
    {
        SetHoveredCell(-1);
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        SetHoveredCell(HitTestCell(e.X, e.Y));
        return;
    }

    if ((e.Id == kEventMouseDown || e.Id == kEventMouseUp) && IsContextMenuButton(e.Button))
    {
        const int cell = HitTestCell(e.X, e.Y);
        if (cell >= 0)
        {
            if (!IsSaveSlot(cell) && e.Id == kEventMouseUp)
                ShowPresetContextMenu(cell, e.X, e.Y);
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        const int cell = HitTestCell(e.X, e.Y);
        if (cell >= 0 && IsSaveSlot(cell))
        {
            if (m_OnRequestSave)
                m_OnRequestSave();
            e.Stop();
            return;
        }
        if (cell >= 0)
        {
            if (const CurvePresets::CurvePreset* preset = PresetAt(cell); preset && m_OnPick)
            {
                m_ActivePreset = cell;
                MarkDirty(VisualDirty);
                m_OnPick(*preset);
            }
            e.Stop();
            return;
        }
    }
}

void CurvePresetPicker::ShowPresetContextMenu(int presetIndex, float x, float y)
{
    if (!m_ContextMenuWindow || presetIndex < 0 || IsSaveSlot(presetIndex))
        return;

    const CurvePresets::CurvePreset* preset = PresetAt(presetIndex);
    if (!preset)
        return;

    const bool isUserPreset = IsUserPreset(presetIndex);
    const int userIndex = presetIndex - BuiltinCount();
    CurvePresets::CurvePreset presetCopy = *preset;

    static std::shared_ptr<INativeContextMenu> s_Menu;
    if (!s_Menu)
    {
        s_Menu = CreateContextMenu();
        if (!s_Menu)
            return;
    }

    enum : uint32_t
    {
        kCmdRename = 1,
        kCmdDuplicate = 2,
        kCmdDelete = 3,
    };

    std::shared_ptr<bool> alive = m_Alive;
    s_Menu->Clear();
    s_Menu->SetCommandHandler(
        [alive, this, userIndex, isUserPreset, presetCopy](uint32_t cmd)
        {
            if (!alive || !*alive)
                return;

            auto& library = CurvePresets::UserPresetLibrary::Get();

            if (cmd == kCmdRename)
            {
                if (isUserPreset)
                    ShowRenamePresetModal(userIndex);
                return;
            }
            else if (cmd == kCmdDuplicate)
            {
                CurvePresets::CurvePreset copy = presetCopy;
                copy.Name = CopyNameForPreset(copy.Name);
                library.Add(std::move(copy));
                m_ActivePreset = BuiltinCount() + static_cast<int>(library.Presets().size()) - 1;
            }
            else if (cmd == kCmdDelete)
            {
                if (!isUserPreset)
                    return;
                const auto& presetsRef = library.Presets();
                if (userIndex < 0 || userIndex >= static_cast<int>(presetsRef.size()))
                    return;
                library.Remove(userIndex);
                const int removedPresetIndex = BuiltinCount() + userIndex;
                if (m_ActivePreset == removedPresetIndex)
                    m_ActivePreset = -1;
                else if (m_ActivePreset > removedPresetIndex)
                    --m_ActivePreset;
            }

            m_Hover = -1;
            UpdateHeight();
        });

    s_Menu->AddItem(0, "Rename...", kCmdRename, isUserPreset ? MenuItemFlag_None : MenuItemFlag_Disabled);
    s_Menu->SetItemIcon(kCmdRename, EditorIcons::kBrush);
    s_Menu->AddItem(0, "Duplicate preset", kCmdDuplicate, MenuItemFlag_None);
    s_Menu->SetItemIcon(kCmdDuplicate, EditorIcons::kCopy);
    s_Menu->AddItem(0, "Delete preset", kCmdDelete, isUserPreset ? MenuItemFlag_None : MenuItemFlag_Disabled);
    s_Menu->SetItemIcon(kCmdDelete, EditorIcons::kTrash);
    s_Menu->Show(m_ContextMenuWindow, static_cast<int>(x), static_cast<int>(y));
}

RenameLayoutModal* CurvePresetPicker::EnsureRenameModal()
{
    if (m_RenameModal)
        return m_RenameModal;

    UIManager* manager = GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!root)
        return nullptr;

    if (UIElement* existing = root->FindById("curve-preset-rename-modal"))
    {
        m_RenameModal = dynamic_cast<RenameLayoutModal*>(existing);
        if (m_RenameModal)
            return m_RenameModal;
    }

    auto modal = std::make_unique<RenameLayoutModal>();
    m_RenameModal = modal.get();
    m_RenameModal->SetId("curve-preset-rename-modal");
    root->AddChild(std::move(modal));
    return m_RenameModal;
}

void CurvePresetPicker::ShowRenamePresetModal(int userIndex)
{
    auto& library = CurvePresets::UserPresetLibrary::Get();
    const auto& presets = library.Presets();
    if (userIndex < 0 || userIndex >= static_cast<int>(presets.size()))
        return;

    RenameLayoutModal* modal = EnsureRenameModal();
    if (!modal)
        return;

    std::shared_ptr<bool> alive = m_Alive;
    modal->SetOnCommit([alive, this, userIndex](const std::string& newName)
    {
        CurvePresets::UserPresetLibrary::Get().Rename(userIndex, newName);
        if (!alive || !*alive)
            return;
        m_Hover = -1;
        MarkDirty(VisualDirty);
        UpdateHeight();
    });
    modal->SetOnCancel([]() {});
    modal->Show("Rename Curve Preset", presets[static_cast<size_t>(userIndex)].Name);
}

void CurvePresetPicker::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                             float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 1.0f || h <= 1.0f)
        return;

    const float cs = ctx.ContentScale;
    auto emitCenteredCellLabel = [&](const std::string& text, float cellX, float cellY, float cellW, float cellH,
                                     uint32_t color)
    {
        if (!ctx.FontAtlas)
            return;
        const float pixelSize = std::max(1.0f, kCellLabelFontPx * cs);
        const float textW = ctx.FontAtlas->MeasureUtf8(text, pixelSize).width;
        const float textX = cellX + (cellW - textW) * 0.5f;
        const float textY = cellY + cellH - (kCellLabelStripPx + kCellLabelBottomMarginPx) * cs;
        ctx.EmitText(text, textX, textY, kCellLabelFontPx, color, ctx.FontAtlas);
    };

    const int total = TotalCount();
    for (int i = 0; i < total; ++i)
    {
        Rect cell;
        if (!CellRect(i, cell))
            continue;
        // Cell rects are in logical (layout) space; convert to physical by re-deriving from the
        // physical origin so paint lines up with hit-testing.
        const float px = x + (cell.X - GetLayoutX()) * cs;
        const float py = y + (cell.Y - GetLayoutY()) * cs;
        const float pw = cell.W * cs;
        const float ph = cell.H * cs;

        const bool hovered = (m_Hover == i);
        const bool active = (m_ActivePreset == i);
        const bool isUser = IsUserPreset(i);
        uint32_t cellBg = UI::PackColor(0.13f, 0.14f, 0.16f, 1.0f);
        if (active)
            cellBg = UI::PackColor(0.20f, 0.28f, 0.42f, 1.0f);
        else if (hovered)
            cellBg = UI::PackColor(0.22f, 0.24f, 0.28f, 0.98f);
        UI::UIPrimitive cellRect = UI::MakeRect(px + 2.0f * cs, py + 2.0f * cs, pw - 4.0f * cs, ph - 4.0f * cs,
                                                cellBg, 4.0f * cs, 4.0f * cs, 4.0f * cs, 4.0f * cs);
        ctx.Emit(cellRect);

        if (IsSaveSlot(i))
        {
            constexpr float kPlusIconCssPx = 18.0f;
            const float iconAlpha = hovered ? 1.0f : 0.175f; // matches .plus-icon
            const float iconX = px + (pw - kPlusIconCssPx * cs) * 0.5f;
            const float iconAreaH = std::max(
                0.0f,
                ph - (kCellLabelStripPx + kCellLabelTopMarginPx + kCellLabelBottomMarginPx) * cs);
            const float iconY = py + (iconAreaH - kPlusIconCssPx * cs) * 0.5f + kSaveSlotPlusOffsetYPx * cs;
            bool drewPlus = false;
            UIManager* owner = ctx.Manager ? ctx.Manager : GetOwnerManager();
            if (owner && ctx.Textures)
            {
                uint32_t plusImgW = 0;
                uint32_t plusImgH = 0;
                const uint32_t plusTexSlot = owner->TryRegisterResolvedBackgroundTexture(
                    kEditorPlusIconPath, kEditorAssetMountAlias, *ctx.Textures, &plusImgW, &plusImgH);
                if (plusTexSlot != 0 && plusImgW > 0 && plusImgH > 0)
                {
                    ctx.Emit(UI::MakeTexturedQuad(iconX, iconY, kPlusIconCssPx * cs, kPlusIconCssPx * cs,
                                                  plusTexSlot, UI::PackColor(1.0f, 1.0f, 1.0f, iconAlpha)));
                    drewPlus = true;
                }
            }
            if (!drewPlus)
            {
                const uint32_t plusColor = UI::PackColor(1.0f, 1.0f, 1.0f, iconAlpha);
                const float cx = px + pw * 0.5f;
                const float cy = py + iconAreaH * 0.5f + kSaveSlotPlusOffsetYPx * cs;
                constexpr float kPlusHalfPx = 8.0f;
                ctx.Emit(UI::MakeLine(cx, cy - kPlusHalfPx * cs, cx, cy + kPlusHalfPx * cs, 1.25f * cs, plusColor));
                ctx.Emit(UI::MakeLine(cx - kPlusHalfPx * cs, cy, cx + kPlusHalfPx * cs, cy, 1.25f * cs, plusColor));
            }
            emitCenteredCellLabel("New Preset", px, py, pw, ph,
                                  hovered ? UI::PackColor(0.98f, 0.98f, 0.98f, 1.0f) : kLabelColor);
            continue;
        }

        // Mini curve thumbnail (sampled), fitted to the cell's value range so overshoots show.
        const CurvePresets::CurvePreset* preset = PresetAt(i);
        if (preset)
        {
            constexpr int kSamples = 28;
            float lo = 1e30f;
            float hi = -1e30f;
            float vals[kSamples];
            for (int s = 0; s < kSamples; ++s)
            {
                const float t = static_cast<float>(s) / static_cast<float>(kSamples - 1);
                vals[s] = CurvePresets::EvaluatePreset01(*preset, t);
                lo = std::min(lo, vals[s]);
                hi = std::max(hi, vals[s]);
            }
            const float pad = std::max(0.04f, (hi - lo) * 0.12f);
            lo -= pad;
            hi += pad;
            const float span = std::max(1e-4f, hi - lo);

            const float gx = px + (kCellInnerPadPx + 2.0f) * cs;
            const float gw = pw - (kCellInnerPadPx + 2.0f) * 2.0f * cs;
            const float gy = py + kCellInnerPadPx * cs;
            const float labelY = py + ph - (kCellLabelStripPx + kCellLabelBottomMarginPx) * cs;
            const float gh = std::max(1.0f * cs, labelY - kCellLabelTopMarginPx * cs - gy);
            float prevX = 0.0f;
            float prevY = 0.0f;
            for (int s = 0; s < kSamples; ++s)
            {
                const float t = static_cast<float>(s) / static_cast<float>(kSamples - 1);
                const float sx = gx + t * gw;
                const float sy = gy + (1.0f - (vals[s] - lo) / span) * gh;
                if (s > 0)
                {
                    const float curveThickness = active ? 2.2f * cs : (hovered ? 2.0f * cs : 1.5f * cs);
                    ctx.Emit(UI::MakeLine(prevX, prevY, sx, sy, curveThickness, kCurveColor));
                }
                prevX = sx;
                prevY = sy;
            }

            if (preset->Kind == CurvePresets::CurvePresetKind::CubicBezier)
                ctx.Emit(UI::MakeRect(px + 5.0f * cs, py + 4.0f * cs, 14.0f * cs, 2.0f * cs, kBezierAccent,
                                      1.0f * cs, 1.0f * cs, 1.0f * cs, 1.0f * cs));

            if (isUser)
                ctx.Emit(UI::MakeRect(px + pw - 8.0f * cs, py + 4.0f * cs, 3.0f * cs, 3.0f * cs, kUserAccent));

            emitCenteredCellLabel(
                preset->Name, px, py, pw, ph,
                (active || hovered) ? UI::PackColor(0.98f, 0.98f, 0.98f, 1.0f) : kLabelColor);
        }
    }
}

} // namespace GameEngine
