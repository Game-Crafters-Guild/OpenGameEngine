#include "Graph/BlendSpace1DEditor.h"

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

namespace GameEngine {

namespace {

constexpr float kTrackMargin = 40.f;
constexpr float kTrackThickness = 3.f;
constexpr float kSampleSize = 14.f;
constexpr float kSampleSelectedSize = 16.f;
constexpr float kSampleHitRadius = 10.f;
constexpr float kTrackHitHalf = 16.f;
constexpr float kDragThresholdPx = 4.f;
constexpr float kFontSizePx = 12.f;
constexpr float kCaptionOffsetY = -28.f;
constexpr float kPreviewLabelOffsetY = -46.f;
constexpr float kSampleLabelOffsetY = 12.f;
static_assert(kCaptionOffsetY - kPreviewLabelOffsetY >= kFontSizePx + 6.f);
constexpr float kClipStripHeight = 36.f;
constexpr uint32_t kTrackColor = 0xFF5A5A5Au;
constexpr uint32_t kSampleColor = 0xFF6EA8FEu;
constexpr uint32_t kSampleSelectedColor = 0xFF9DC0FFu;
constexpr uint32_t kSampleRingColor = 0xFFE8F0FFu;
constexpr uint32_t kLabelColor = 0xFFD0D0D0u;
constexpr uint32_t kCaptionColor = 0xFFB8B8B8u;
constexpr uint32_t kPreviewColor = 0xFFE6B84Du;
constexpr float kPreviewLineHalf = 10.f;
constexpr float kPreviewMarkerSize = 10.f;

std::string FormatPosition(float position)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(position));
    return buf;
}

} // namespace

BlendSpace1DEditor::BlendSpace1DEditor()
{
    AddClass("blend-space-1d-editor");
    SetFocusable(true);
    m_DisplayAxis = BlendSpace1DAxis::FromSamples(m_Samples);

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
        CommitSamples("Set Blend Space Clip", [this, index, text]()
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

void BlendSpace1DEditor::SetSamples(std::vector<BlendSpace1DSampleDesc> samples)
{
    m_Samples = std::move(samples);
    if (m_SelectedIndex >= static_cast<int>(m_Samples.size()))
        m_SelectedIndex = -1;
    if (!m_Dragging)
        RecomputeDisplayAxis();
    SyncClipField();
    MarkDirty(VisualDirty);
}

void BlendSpace1DEditor::SetParameterName(std::string name)
{
    m_ParameterName = std::move(name);
    MarkDirty(VisualDirty);
}

void BlendSpace1DEditor::SetPreviewPosition(bool active, float position)
{
    if (active && !std::isfinite(position))
        active = false;
    if (m_PreviewActive == active && (!active || m_PreviewPosition == position))
        return;
    m_PreviewActive = active;
    m_PreviewPosition = position;
    MarkDirty(VisualDirty);
}

BlendSpace1DEditor::TrackLayout BlendSpace1DEditor::MakeTrackLayout() const
{
    TrackLayout track;
    track.Left = kTrackMargin;
    track.Width = std::max(0.f, GetLayoutWidth() - 2.f * kTrackMargin);
    const float usable = std::max(0.f, GetLayoutHeight() - ClipStripHeight());
    track.CenterY = usable * 0.5f;
    return track;
}

float BlendSpace1DEditor::ClipStripHeight() const
{
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Samples.size()))
        return 0.f;
    return kClipStripHeight;
}

bool BlendSpace1DEditor::HitClipStrip(float localY) const
{
    const float strip = ClipStripHeight();
    if (strip <= 0.f)
        return false;
    return localY >= GetLayoutHeight() - strip;
}

const BlendSpace1DAxis& BlendSpace1DEditor::ActiveAxis() const
{
    return m_Dragging ? m_DragAxis : m_DisplayAxis;
}

int BlendSpace1DEditor::HitSample(float localX, float localY, const TrackLayout& track) const
{
    const BlendSpace1DAxis& axis = ActiveAxis();
    const float hitR2 = kSampleHitRadius * kSampleHitRadius;
    int best = -1;
    float bestDist = hitR2;
    for (int i = 0; i < static_cast<int>(m_Samples.size()); ++i)
    {
        const float sx = axis.XFromPosition(m_Samples[static_cast<size_t>(i)].Position, track.Left, track.Width);
        const float dx = localX - sx;
        const float dy = localY - track.CenterY;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestDist)
        {
            bestDist = d2;
            best = i;
        }
    }
    return best;
}

bool BlendSpace1DEditor::HitTrack(float localX, float localY, const TrackLayout& track) const
{
    if (track.Width <= 0.f)
        return false;
    if (localX < track.Left || localX > track.Left + track.Width)
        return false;
    return std::abs(localY - track.CenterY) <= kTrackHitHalf;
}

void BlendSpace1DEditor::NotifyChanged()
{
    if (m_OnChanged)
        m_OnChanged();
}

void BlendSpace1DEditor::CommitSamples(const std::string& actionName, std::function<void()> mutate)
{
    if (m_UndoScope)
        m_UndoScope(actionName, std::move(mutate));
    else if (mutate)
        mutate();
}

void BlendSpace1DEditor::RecomputeDisplayAxis()
{
    m_DisplayAxis = BlendSpace1DAxis::FromSamples(m_Samples);
}

void BlendSpace1DEditor::EnsureClipFieldRegistry()
{
    if (!m_ClipField)
        return;
    AssetManager& am = EngineCore::GetInstance().GetAssetManager();
    m_ClipField->SetAssetRegistry(&am.GetRegistry());
}

void BlendSpace1DEditor::SyncClipField()
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

void BlendSpace1DEditor::OnEvent(UIEvent& e)
{
    const TrackLayout track = MakeTrackLayout();
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
        CommitSamples("Delete Blend Space Sample", [this, index]()
        {
            if (index < 0 || index >= static_cast<int>(m_Samples.size()))
                return;
            m_Samples.erase(m_Samples.begin() + index);
            m_SelectedIndex = -1;
            RecomputeDisplayAxis();
            SyncClipField();
            NotifyChanged();
            MarkDirty(VisualDirty);
        });
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_PressedIndex = HitSample(localX, localY, track);
        m_PressX = localX;
        m_Dragging = false;
        m_PendingAdd = false;
        m_DragAxis = m_DisplayAxis;
        if (m_PressedIndex >= 0)
        {
            m_SelectedIndex = m_PressedIndex;
            m_DragStartPosition = m_Samples[static_cast<size_t>(m_PressedIndex)].Position;
            SyncClipField();
            MarkDirty(VisualDirty);
        }
        else if (HitTrack(localX, localY, track))
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
        if (!m_Dragging && std::abs(dx) >= kDragThresholdPx)
        {
            m_Dragging = true;
            if (m_PressedIndex >= 0)
                m_DragAxis = m_DisplayAxis;
        }
        if (m_Dragging && m_PressedIndex >= 0 &&
            m_PressedIndex < static_cast<int>(m_Samples.size()))
        {
            m_Samples[static_cast<size_t>(m_PressedIndex)].Position =
                m_DragAxis.PositionFromX(localX, track.Left, track.Width);
            NotifyChanged();
            MarkDirty(VisualDirty);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging && m_PressedIndex >= 0 &&
            m_PressedIndex < static_cast<int>(m_Samples.size()))
        {
            const int index = m_PressedIndex;
            const float finalPos = m_Samples[static_cast<size_t>(index)].Position;
            m_Samples[static_cast<size_t>(index)].Position = m_DragStartPosition;
            CommitSamples("Move Blend Space Sample", [this, index, finalPos]()
            {
                if (index < 0 || index >= static_cast<int>(m_Samples.size()))
                    return;
                m_Samples[static_cast<size_t>(index)].Position = finalPos;
                RecomputeDisplayAxis();
                NotifyChanged();
                MarkDirty(VisualDirty);
            });
        }
        else if (m_PendingAdd)
        {
            const float pos = m_DisplayAxis.PositionFromX(localX, track.Left, track.Width);
            CommitSamples("Add Blend Space Sample", [this, pos]()
            {
                BlendSpace1DSampleDesc sample;
                sample.Position = pos;
                m_Samples.push_back(std::move(sample));
                m_SelectedIndex = static_cast<int>(m_Samples.size()) - 1;
                RecomputeDisplayAxis();
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

void BlendSpace1DEditor::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
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
    const float canvasW = w / cs;

    auto emitRectRounded = [&](float rx, float ry, float rw, float rh, uint32_t color, float r)
    {
        ctx.Emit(MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color, r * cs, r * cs, r * cs, r * cs));
    };

    const TrackLayout track = MakeTrackLayout();
    const float trackLeft = kTrackMargin;
    const float trackWidth = std::max(0.f, canvasW - 2.f * kTrackMargin);
    const float trackY = track.CenterY;
    emitRectRounded(canvasX + trackLeft, canvasY + trackY - kTrackThickness * 0.5f,
                    trackWidth, kTrackThickness, kTrackColor, kTrackThickness * 0.5f);

    const BlendSpace1DAxis& axis = ActiveAxis();

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

    if (!m_ParameterName.empty())
        emitLabel(m_ParameterName, trackLeft + trackWidth * 0.5f, trackY + kCaptionOffsetY, kCaptionColor);

    if (m_PreviewActive && trackWidth > 0.f)
    {
        const float px = trackLeft + axis.ClampedT(m_PreviewPosition) * trackWidth;
        emitRectRounded(canvasX + px - 1.f, canvasY + trackY - kPreviewLineHalf, 2.f, kPreviewLineHalf * 2.f,
                        kPreviewColor, 1.f);
        const float half = kPreviewMarkerSize * 0.5f;
        emitRectRounded(canvasX + px - half, canvasY + trackY - half, kPreviewMarkerSize, kPreviewMarkerSize,
                        kPreviewColor, half);
        emitLabel(FormatPosition(m_PreviewPosition), px, trackY + kPreviewLabelOffsetY, kPreviewColor);
    }

    for (int i = 0; i < static_cast<int>(m_Samples.size()); ++i)
    {
        const BlendSpace1DSampleDesc& sample = m_Samples[static_cast<size_t>(i)];
        const float sx = axis.XFromPosition(sample.Position, trackLeft, trackWidth);
        const bool selected = (i == m_SelectedIndex);
        const float size = selected ? kSampleSelectedSize : kSampleSize;
        const float half = size * 0.5f;
        if (selected)
        {
            const float ring = size + 4.f;
            emitRectRounded(canvasX + sx - ring * 0.5f, canvasY + trackY - ring * 0.5f,
                            ring, ring, kSampleRingColor, ring * 0.5f);
        }
        emitRectRounded(canvasX + sx - half, canvasY + trackY - half, size, size,
                        selected ? kSampleSelectedColor : kSampleColor, half);
        const std::string label = sample.Label.empty() ? FormatPosition(sample.Position) : sample.Label;
        emitLabel(label, sx, trackY + kSampleLabelOffsetY, kLabelColor);
    }
}

} // namespace GameEngine
