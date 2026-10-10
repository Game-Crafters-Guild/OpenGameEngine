#include "Graph/BlendSpace2DEditor.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Input/KeyCodes.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/AssetField.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace GameEngine {

namespace {

constexpr float kPadMargin = 48.f;
constexpr float kSampleSize = 14.f;
constexpr float kSampleSelectedSize = 16.f;
constexpr float kSampleHitRadius = 10.f;
constexpr float kDragThresholdPx = 4.f;
constexpr float kFontSizePx = 12.f;
constexpr float kClipStripHeight = 36.f;
constexpr uint32_t kPadColor = 0xFF3A3A3Au;
constexpr uint32_t kAxisColor = 0xFF5A5A5Au;
constexpr uint32_t kSampleColor = 0xFF6EA8FEu;
constexpr uint32_t kSampleSelectedColor = 0xFF9DC0FFu;
constexpr uint32_t kSampleRingColor = 0xFFE8F0FFu;
constexpr uint32_t kLabelColor = 0xFFD0D0D0u;
constexpr uint32_t kCaptionColor = 0xFFB8B8B8u;
constexpr uint32_t kPreviewColor = 0xFFE6B84Du;
constexpr float kPreviewMarkerSize = 10.f;
constexpr float kPreviewCrossHalf = 8.f;
constexpr float kAxisCaptionBelow = 8.f;
constexpr float kAxisValueBelow = 22.f;
constexpr float kAxisValueLeft = -22.f;

std::string FormatAxisValue(float value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
    return buf;
}

} // namespace

BlendSpace2DEditor::BlendSpace2DEditor()
{
    AddClass("blend-space-2d-editor");
    SetFocusable(true);

    auto clipField = std::make_unique<AssetField>();
    clipField->AddClass("blend-space-1d-clip-row");
    clipField->AddClass("dropdown-asset-field");
    clipField->AddClass("hidden");
    clipField->SetAcceptedTypes({AssetType::Animation});
    clipField->SetTooltip("Animation clip");
    clipField->SetOnValueChanged([this](const GUID& guid)
    {
        if (m_UpdatingClipField)
            return;
        if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Samples.size()))
            return;
        const int index = m_SelectedIndex;
        const std::string text = guid.IsNull() ? std::string{} : guid.ToString();
        CommitSamples("Set Blend Space 2D Clip", [this, index, text]()
        {
            if (index < 0 || index >= static_cast<int>(m_Samples.size()))
                return;
            m_Samples[static_cast<size_t>(index)].ClipGuid = text;
            NotifyChanged();
        });
    });
    m_ClipField = clipField.get();
    AddChild(std::move(clipField));
}

void BlendSpace2DEditor::SetSamples(std::vector<BlendSpace2DSampleDesc> samples)
{
    m_Samples = std::move(samples);
    if (m_SelectedIndex >= static_cast<int>(m_Samples.size()))
        m_SelectedIndex = -1;
    SyncClipField();
    MarkDirty(VisualDirty);
}

void BlendSpace2DEditor::SetParameterNameX(std::string name)
{
    m_ParameterNameX = std::move(name);
    MarkDirty(VisualDirty);
}

void BlendSpace2DEditor::SetParameterNameY(std::string name)
{
    m_ParameterNameY = std::move(name);
    MarkDirty(VisualDirty);
}

void BlendSpace2DEditor::SetPreviewPosition(bool active, float x, float y)
{
    if (active && (!std::isfinite(x) || !std::isfinite(y)))
        active = false;
    if (m_PreviewActive == active && (!active || (m_PreviewX == x && m_PreviewY == y)))
        return;
    m_PreviewActive = active;
    m_PreviewX = x;
    m_PreviewY = y;
    MarkDirty(VisualDirty);
}

BlendSpace2DEditor::PadLayout BlendSpace2DEditor::MakePadLayout() const
{
    PadLayout pad;
    pad.Left = kPadMargin;
    pad.Top = kPadMargin;
    pad.Width = std::max(0.f, GetLayoutWidth() - 2.f * kPadMargin);
    pad.Height = std::max(0.f, GetLayoutHeight() - ClipStripHeight() - 2.f * kPadMargin);
    return pad;
}

float BlendSpace2DEditor::ClipStripHeight() const
{
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Samples.size()))
        return 0.f;
    return kClipStripHeight;
}

bool BlendSpace2DEditor::HitClipStrip(float localY) const
{
    const float strip = ClipStripHeight();
    if (strip <= 0.f)
        return false;
    return localY >= GetLayoutHeight() - strip;
}

BlendSpace2DAxis BlendSpace2DEditor::MakeAxisX() const
{
    return BlendSpace2DAxis::FromSamples(m_Samples, true);
}

BlendSpace2DAxis BlendSpace2DEditor::MakeAxisY() const
{
    return BlendSpace2DAxis::FromSamples(m_Samples, false);
}

void BlendSpace2DEditor::SampleToPad(const BlendSpace2DSampleDesc& sample, const PadLayout& pad,
                                    const BlendSpace2DAxis& axisX, const BlendSpace2DAxis& axisY, float& outX,
                                    float& outY) const
{
    const float spanX = axisX.Max - axisX.Min;
    const float spanY = axisY.Max - axisY.Min;
    const float tx = spanX == 0.f ? 0.5f : (sample.X - axisX.Min) / spanX;
    const float ty = spanY == 0.f ? 0.5f : (sample.Y - axisY.Min) / spanY;
    outX = pad.Left + tx * pad.Width;
    outY = pad.Top + (1.f - ty) * pad.Height;
}

void BlendSpace2DEditor::ParameterToPad(float x, float y, const PadLayout& pad,
                                       const BlendSpace2DAxis& axisX, const BlendSpace2DAxis& axisY,
                                       float& outX, float& outY) const
{
    outX = pad.Left + axisX.ClampedT(x) * pad.Width;
    outY = pad.Top + (1.f - axisY.ClampedT(y)) * pad.Height;
}

void BlendSpace2DEditor::PadToSample(float localX, float localY, const PadLayout& pad, float& outX,
                                    float& outY) const
{
    const BlendSpace2DAxis axisX = MakeAxisX();
    const BlendSpace2DAxis axisY = MakeAxisY();
    const float tx = pad.Width <= 0.f ? 0.f : std::clamp((localX - pad.Left) / pad.Width, 0.f, 1.f);
    const float ty = pad.Height <= 0.f ? 0.f : std::clamp(1.f - (localY - pad.Top) / pad.Height, 0.f, 1.f);
    outX = axisX.Min + tx * (axisX.Max - axisX.Min);
    outY = axisY.Min + ty * (axisY.Max - axisY.Min);
}

bool BlendSpace2DEditor::HitPad(float localX, float localY, const PadLayout& pad) const
{
    return localX >= pad.Left && localX <= pad.Left + pad.Width && localY >= pad.Top &&
           localY <= pad.Top + pad.Height;
}

int BlendSpace2DEditor::HitSample(float localX, float localY, const PadLayout& pad) const
{
    const float hitR2 = kSampleHitRadius * kSampleHitRadius;
    int best = -1;
    float bestDist = hitR2;
    const BlendSpace2DAxis axisX = MakeAxisX();
    const BlendSpace2DAxis axisY = MakeAxisY();
    for (int i = 0; i < static_cast<int>(m_Samples.size()); ++i)
    {
        float sx = 0.f;
        float sy = 0.f;
        SampleToPad(m_Samples[static_cast<size_t>(i)], pad, axisX, axisY, sx, sy);
        const float dx = localX - sx;
        const float dy = localY - sy;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestDist)
        {
            bestDist = d2;
            best = i;
        }
    }
    return best;
}

void BlendSpace2DEditor::NotifyChanged()
{
    if (m_OnChanged)
        m_OnChanged();
}

void BlendSpace2DEditor::CommitSamples(const std::string& actionName, std::function<void()> mutate)
{
    if (m_UndoScope)
        m_UndoScope(actionName, std::move(mutate));
    else if (mutate)
        mutate();
}

void BlendSpace2DEditor::EnsureClipFieldRegistry()
{
    if (!m_ClipField)
        return;
    AssetManager& am = EngineCore::GetInstance().GetAssetManager();
    m_ClipField->SetAssetRegistry(&am.GetRegistry());
}

void BlendSpace2DEditor::SyncClipField()
{
    if (!m_ClipField)
        return;
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Samples.size()))
    {
        m_ClipField->AddClass("hidden");
        m_ClipField->MarkDirty(LayoutDirty | VisualDirty);
        return;
    }

    EnsureClipFieldRegistry();
    m_ClipField->RemoveClass("hidden");
    const GUID guid(m_Samples[static_cast<size_t>(m_SelectedIndex)].ClipGuid);
    m_UpdatingClipField = true;
    m_ClipField->SetValue(guid);
    m_UpdatingClipField = false;
    m_ClipField->MarkDirty(LayoutDirty | VisualDirty);
}

void BlendSpace2DEditor::OnEvent(UIEvent& e)
{
    const PadLayout pad = MakePadLayout();
    const float localX = e.X - GetLayoutX();
    const float localY = e.Y - GetLayoutY();

    if (e.Id == kEventMouseDown && HitClipStrip(localY))
    {
        e.Stop();
        return;
    }

    if (e.Id == kEventKeyDown && e.Mods == 0 &&
        (e.Key == Input::kKeyCode_Delete || e.Key == Input::kKeyCode_Backspace))
    {
        if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Samples.size()))
            return;
        const int index = m_SelectedIndex;
        CommitSamples("Delete Blend Space 2D Sample", [this, index]()
        {
            if (index < 0 || index >= static_cast<int>(m_Samples.size()))
                return;
            m_Samples.erase(m_Samples.begin() + index);
            m_SelectedIndex = -1;
            SyncClipField();
            NotifyChanged();
            MarkDirty(VisualDirty);
        });
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_PressedIndex = HitSample(localX, localY, pad);
        m_PressX = localX;
        m_PressY = localY;
        m_Dragging = false;
        m_PendingAdd = false;
        if (m_PressedIndex >= 0)
        {
            m_SelectedIndex = m_PressedIndex;
            m_DragStartX = m_Samples[static_cast<size_t>(m_PressedIndex)].X;
            m_DragStartY = m_Samples[static_cast<size_t>(m_PressedIndex)].Y;
            SyncClipField();
            MarkDirty(VisualDirty);
        }
        else if (HitPad(localX, localY, pad))
        {
            m_PendingAdd = true;
        }
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_PressedIndex < 0 && !m_PendingAdd)
            return;
        const float dx = localX - m_PressX;
        const float dy = localY - m_PressY;
        if (!m_Dragging && (dx * dx + dy * dy) >= kDragThresholdPx * kDragThresholdPx)
            m_Dragging = true;
        if (m_Dragging && m_PressedIndex >= 0 && m_PressedIndex < static_cast<int>(m_Samples.size()))
        {
            float x = 0.f;
            float y = 0.f;
            PadToSample(localX, localY, pad, x, y);
            m_Samples[static_cast<size_t>(m_PressedIndex)].X = x;
            m_Samples[static_cast<size_t>(m_PressedIndex)].Y = y;
            NotifyChanged();
            MarkDirty(VisualDirty);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging && m_PressedIndex >= 0 && m_PressedIndex < static_cast<int>(m_Samples.size()))
        {
            const int index = m_PressedIndex;
            const float finalX = m_Samples[static_cast<size_t>(index)].X;
            const float finalY = m_Samples[static_cast<size_t>(index)].Y;
            m_Samples[static_cast<size_t>(index)].X = m_DragStartX;
            m_Samples[static_cast<size_t>(index)].Y = m_DragStartY;
            CommitSamples("Move Blend Space 2D Sample", [this, index, finalX, finalY]()
            {
                if (index < 0 || index >= static_cast<int>(m_Samples.size()))
                    return;
                m_Samples[static_cast<size_t>(index)].X = finalX;
                m_Samples[static_cast<size_t>(index)].Y = finalY;
                NotifyChanged();
                MarkDirty(VisualDirty);
            });
        }
        else if (m_PendingAdd)
        {
            float x = 0.f;
            float y = 0.f;
            PadToSample(localX, localY, pad, x, y);
            CommitSamples("Add Blend Space 2D Sample", [this, x, y]()
            {
                BlendSpace2DSampleDesc sample;
                sample.X = x;
                sample.Y = y;
                m_Samples.push_back(std::move(sample));
                m_SelectedIndex = static_cast<int>(m_Samples.size()) - 1;
                SyncClipField();
                NotifyChanged();
                MarkDirty(VisualDirty);
            });
        }
        m_PressedIndex = -1;
        m_Dragging = false;
        m_PendingAdd = false;
        e.Stop();
    }
}

void BlendSpace2DEditor::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                                              float x, float y, float w, float h)
{
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    using namespace UI;
    const float cs = std::max(0.01f, ctx.ContentScale);
    const float canvasX = x / cs;
    const float canvasY = y / cs;

    auto emitRectRounded = [&](float rx, float ry, float rw, float rh, uint32_t color, float r)
    {
        ctx.Emit(MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color, r * cs, r * cs, r * cs, r * cs));
    };

    const PadLayout pad = MakePadLayout();
    emitRectRounded(canvasX + pad.Left, canvasY + pad.Top, pad.Width, pad.Height, kPadColor, 4.f);
    emitRectRounded(canvasX + pad.Left, canvasY + pad.Top + pad.Height * 0.5f - 0.5f, pad.Width, 1.f, kAxisColor,
                    0.f);
    emitRectRounded(canvasX + pad.Left + pad.Width * 0.5f - 0.5f, canvasY + pad.Top, 1.f, pad.Height, kAxisColor,
                    0.f);

    auto emitLabel = [&](const std::string& text, float logicalX, float logicalY, uint32_t color)
    {
        if (text.empty() || !ctx.FontAtlas)
            return;
        const float pixelSize = std::max(1.f, kFontSizePx * cs);
        const float textW = ctx.FontAtlas->MeasureUtf8(text, pixelSize).width;
        const float px = (canvasX + logicalX) * cs - textW * 0.5f;
        const float py = (canvasY + logicalY) * cs;
        ctx.EmitText(text, px, py, kFontSizePx, color, ctx.FontAtlas);
    };

    const BlendSpace2DAxis axisX = MakeAxisX();
    const BlendSpace2DAxis axisY = MakeAxisY();

    if (!m_ParameterNameX.empty())
        emitLabel(m_ParameterNameX, pad.Left + pad.Width * 0.5f, pad.Top + pad.Height + kAxisCaptionBelow,
                  kCaptionColor);
    emitLabel(FormatAxisValue(axisX.Min), pad.Left, pad.Top + pad.Height + kAxisValueBelow, kLabelColor);
    emitLabel(FormatAxisValue(axisX.Max), pad.Left + pad.Width, pad.Top + pad.Height + kAxisValueBelow, kLabelColor);
    if (!m_ParameterNameY.empty())
        emitLabel(m_ParameterNameY, pad.Left + kAxisValueLeft, pad.Top + pad.Height * 0.5f - 6.f, kCaptionColor);
    emitLabel(FormatAxisValue(axisY.Max), pad.Left + kAxisValueLeft, pad.Top - 18.f, kLabelColor);
    emitLabel(FormatAxisValue(axisY.Min), pad.Left + kAxisValueLeft, pad.Top + pad.Height + kAxisValueBelow,
              kLabelColor);

    if (m_PreviewActive)
    {
        float px = 0.f;
        float py = 0.f;
        ParameterToPad(m_PreviewX, m_PreviewY, pad, axisX, axisY, px, py);
        emitRectRounded(canvasX + px - kPreviewCrossHalf, canvasY + py - 1.f, kPreviewCrossHalf * 2.f, 2.f,
                        kPreviewColor, 1.f);
        emitRectRounded(canvasX + px - 1.f, canvasY + py - kPreviewCrossHalf, 2.f, kPreviewCrossHalf * 2.f,
                        kPreviewColor, 1.f);
        const float half = kPreviewMarkerSize * 0.5f;
        emitRectRounded(canvasX + px - half, canvasY + py - half, kPreviewMarkerSize, kPreviewMarkerSize,
                        kPreviewColor, half);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.1f, %.1f", static_cast<double>(m_PreviewX),
                      static_cast<double>(m_PreviewY));
        emitLabel(buf, px + 16.f, py - 6.f, kPreviewColor);
    }

    for (int i = 0; i < static_cast<int>(m_Samples.size()); ++i)
    {
        const BlendSpace2DSampleDesc& sample = m_Samples[static_cast<size_t>(i)];
        float sx = 0.f;
        float sy = 0.f;
        SampleToPad(sample, pad, axisX, axisY, sx, sy);
        const bool selected = (i == m_SelectedIndex);
        const float size = selected ? kSampleSelectedSize : kSampleSize;
        const float half = size * 0.5f;
        if (selected)
        {
            const float ring = size + 4.f;
            emitRectRounded(canvasX + sx - ring * 0.5f, canvasY + sy - ring * 0.5f, ring, ring, kSampleRingColor,
                            ring * 0.5f);
        }
        emitRectRounded(canvasX + sx - half, canvasY + sy - half, size, size,
                        selected ? kSampleSelectedColor : kSampleColor, half);
        if (!sample.Label.empty())
            emitLabel(sample.Label, sx, sy + 12.f, kLabelColor);
    }
}

} // namespace GameEngine
