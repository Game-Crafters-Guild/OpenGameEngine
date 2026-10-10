#include "Inspectors/LensFlareSourceInspector.h"
#include "AssetCore/SharedFileRead.h"

#include "InspectorRegistry.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/FlareAtlasAsset.h"
#include "Assets/LensFlareDefinitionAsset.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Core/Engine.h"
#include "UI/EditorIcons.h"
#include "Editor/Assets/AsyncAssetHelpers.h"
#include "ECS/Entity.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/CurvePresetPicker.h"
#include "UI/Controls/CurvePresets.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Toggle.h"
#include "UI/Interaction/DragDropGhostRendererRegistry.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Payload.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"
#include "UndoRedo/UndoRedoService.h"

#include "EditorContextMenu/UIContextMenu.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using FlareAsset = LensFlareDefinitionAsset;
using FlareMutation = std::function<void(FlareAsset&)>;

Editor::UndoRedoService::SnapshotTarget MakeFlareSnapshotTarget(FlareAsset* flare,
                                                                 std::string label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = std::move(label);
    target.Capture = [flare](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        if (!flare)
            return false;
        GameEngine::Vector<GameEngine::uint8> bytes;
        if (!GameEngine::ReadFileBytesShared(flare->GetPath(), bytes))
            return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    };
    target.Apply = [flare](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot) -> bool
    {
        if (!flare)
            return false;
        std::ofstream stream(flare->GetPath(), std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;
        if (!snapshot.empty())
            stream.write(reinterpret_cast<const char*>(snapshot.data()),
                         static_cast<std::streamsize>(snapshot.size()));
        stream.close();
        return stream.good() && flare->Reload() == ReloadOutcome::Reloaded;
    };
    return target;
}

void CommitFlareChange(FlareAsset* flare,
                       Editor::UndoRedoService* undo,
                       const std::string& label,
                       const FlareMutation& mutation)
{
    if (!flare || !mutation)
        return;
    if (undo)
    {
        auto edit = undo->BeginInteractiveEdit(label, MakeFlareSnapshotTarget(flare, label));
        mutation(*flare);
        if (!flare->Save())
        {
            edit.Cancel();
            (void)flare->Reload();
            return;
        }
        edit.Commit();
        return;
    }
    mutation(*flare);
    (void)flare->Save();
}

// Float rows need a mutation that receives the edited value. Keeping this helper
// separate makes every call site short and keeps element access index-based.
void AddFlareFloatValue(UIElement* parent,
                        FlareAsset* flare,
                        Editor::UndoRedoService* undo,
                        const std::string& label,
                        float value,
                        std::function<void(FlareAsset&, float)> mutation,
                        float defaultValue,
                        const char* tooltip = nullptr,
                        float minValue = -std::numeric_limits<float>::infinity(),
                        float maxValue = std::numeric_limits<float>::infinity())
{
    using OptionalEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto edit = std::make_shared<OptionalEdit>();
    InspectorDrag::AddFloatRowWithDrag(
        parent, label, value,
        [flare, undo, label, edit, mutation](float v)
        {
            if (!flare)
                return;
            if (undo && !edit->has_value())
                edit->emplace(undo->BeginInteractiveEdit(label, MakeFlareSnapshotTarget(flare, label)));
            mutation(*flare, v);
        },
        [flare, undo, label, edit, mutation](float v)
        {
            if (!flare)
                return;
            if (edit->has_value())
            {
                mutation(*flare, v);
                if (flare->Save())
                {
                    auto finished = std::move(edit->value());
                    edit->reset();
                    finished.Commit();
                }
                else
                {
                    auto failed = std::move(edit->value());
                    edit->reset();
                    failed.Cancel();
                    (void)flare->Reload();
                }
                return;
            }
            CommitFlareChange(flare, undo, label,
                              [mutation, v](FlareAsset& asset) { mutation(asset, v); });
        },
        defaultValue, tooltip, minValue, maxValue);
}

void AddFlareToggle(UIElement* parent,
                    FlareAsset* flare,
                    Editor::UndoRedoService* undo,
                    const std::string& label,
                    bool value,
                    std::function<void(FlareAsset&, bool)> mutation,
                    const std::function<void()>& rebuild = {},
                    const char* tooltip = nullptr)
{
    InspectorDrag::AddToggleRow(parent, label, value,
        [flare, undo, label, mutation = std::move(mutation), rebuild](bool v)
        {
            CommitFlareChange(flare, undo, label,
                              [mutation, v](FlareAsset& asset) { mutation(asset, v); });
            if (rebuild)
                rebuild();
        }, tooltip);
}

constexpr float kFlareColorPickerMaxIntensity = 8.0f;
constexpr float kFlareElementPreviewSize = 24.0f;
constexpr float kFlareElementHeaderSwatchSize = 10.0f;
using FlareColorAccess = std::function<LensFlare::Color*(FlareAsset&)>;
using FlareColorUiChanged = std::function<void(const LensFlare::Color&)>;

struct FlareAtlasSpriteDragPayload
{
    GUID AtlasGuid = GUID::Null();
    GUID TextureGuid = GUID::Null();
    std::string SpriteName;
    float U = 0.0f;
    float V = 0.0f;
    float Width = 1.0f;
    float Height = 1.0f;
};

class FlareElementPreview : public UIElement
{
  public:
    FlareElementPreview()
    {
        // Static previews do not participate in hit testing by default. Drag
        // sources and drop targets opt in once when they are constructed, so
        // later visual refreshes cannot accidentally disable interaction.
        Overrides().Set(Style::PointerEvents, false);
    }

    void SetSprite(const GUID& textureGuid,
                   float u, float v, float width, float height,
                   uint32_t tintArgb)
    {
        m_TextureGuid = textureGuid;
        m_U = u;
        m_V = v;
        m_Width = width;
        m_Height = height;
        m_TintArgb = tintArgb;
        MarkDirty(VisualDirty);
    }

    void SetTint(uint32_t tintArgb)
    {
        if (m_TintArgb == tintArgb)
            return;
        m_TintArgb = tintArgb;
        MarkDirty(VisualDirty);
    }

    void CopySpriteRegionTo(FlareAtlasSpriteDragPayload& payload) const
    {
        payload.TextureGuid = m_TextureGuid;
        payload.U = m_U;
        payload.V = m_V;
        payload.Width = m_Width;
        payload.Height = m_Height;
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle&,
                              float x, float y, float width, float height) override
    {
        if (ctx.OffThread)
        {
            if (ctx.EscalateFlag)
                *ctx.EscalateFlag = true;
            return;
        }
        if (!ctx.Manager || !ctx.Textures || m_TextureGuid.IsNull())
            return;

        const Rendering::TextureHandle texture =
            ctx.Manager->TryGetBackgroundTextureHandleByGuid(m_TextureGuid);
        if (!texture.IsValid())
            return;
        const uint32_t textureSlot = ctx.Textures->Register(texture);
        if (textureSlot == 0)
            return;

        // Draw the selected atlas UVs directly into the thumbnail rect. A CSS
        // background cannot do this safely: oversized background positioning is
        // not clipped to the element's own border box.
        constexpr float inset = 0.0f;
        const float drawWidth = width;
        const float drawHeight = height;
        if (drawWidth <= 0.0f || drawHeight <= 0.0f)
            return;
        ctx.Emit(UI::MakeTexturedQuad(
            x + inset, y + inset, drawWidth, drawHeight, textureSlot,
            UI::PackFromARGB(m_TintArgb),
            m_U, m_V, m_U + m_Width, m_V + m_Height));
    }

  private:
    GUID m_TextureGuid = GUID::Null();
    float m_U = 0.0f;
    float m_V = 0.0f;
    float m_Width = 1.0f;
    float m_Height = 1.0f;
    uint32_t m_TintArgb = 0xFFFFFFFFu;
};

class FlareSpriteDropPreview final : public FlareElementPreview,
                                     public UI::Interaction::IDropTarget
{
public:
    using SpriteDroppedCallback = std::function<void(const std::string&)>;

    void SetAtlasGuid(const GUID& atlasGuid) { m_AtlasGuid = atlasGuid; }
    void SetOnSpriteDropped(SpriteDroppedCallback callback)
    {
        m_OnSpriteDropped = std::move(callback);
    }

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override
    {
        return m_OnSpriteDropped &&
               typeId == UI::Interaction::GetPayloadTypeId<FlareAtlasSpriteDragPayload>();
    }

    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override
    {
        if (!ContainsPoint(x, y))
            return false;
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnItem;
        return true;
    }

    UI::Interaction::DropFeedback CanDrop(
        const UI::Interaction::DropRequest& request) const override
    {
        const auto* payload = request.payload.TryGet<FlareAtlasSpriteDragPayload>();
        if (!payload || payload->SpriteName.empty())
            return {false, "No atlas sprite"};
        if (payload->AtlasGuid != m_AtlasGuid)
            return {false, "Sprite belongs to another flare atlas"};
        return {true, {}};
    }

    void PerformDrop(const UI::Interaction::DropRequest& request) override
    {
        const auto* payload = request.payload.TryGet<FlareAtlasSpriteDragPayload>();
        if (payload && m_OnSpriteDropped)
            m_OnSpriteDropped(payload->SpriteName);
    }

    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override
    {
        if (!state.Visible)
        {
            Overrides().Set(Style::BackgroundColor, 0xFF111111u);
            return;
        }
        Overrides().Set(Style::BackgroundColor,
                        state.Allowed ? 0xFF245C3Bu : 0xFF5C2424u);
    }

private:
    GUID m_AtlasGuid = GUID::Null();
    SpriteDroppedCallback m_OnSpriteDropped;
};

void RenderFlareAtlasSpriteGhost(const UI::Interaction::DragPayload& dragPayload,
                                 int,
                                 UIElement&,
                                 UIElement& icon,
                                 Label&,
                                 Label* badge,
                                 void*)
{
    const auto* payload = dragPayload.TryGet<FlareAtlasSpriteDragPayload>();
    if (!payload || payload->TextureGuid.IsNull())
        return;

    constexpr const char* kPreviewId = "flare-atlas-sprite-drag-ghost";
    auto* preview = dynamic_cast<FlareElementPreview*>(icon.FindById(kPreviewId));
    if (!preview)
    {
        auto ownedPreview = std::make_unique<FlareElementPreview>();
        preview = ownedPreview.get();
        preview->SetId(kPreviewId);
        preview->Overrides()
            .Set(Style::Width, StyleLength::Px(64.0f))
            .Set(Style::Height, StyleLength::Px(64.0f))
            .Set(Style::MinWidth, StyleLength::Px(64.0f))
            .Set(Style::MinHeight, StyleLength::Px(64.0f))
            .Set(Style::BackgroundColor, 0xFF111111u)
            .Set(Style::PointerEvents, false);
        icon.AddChild(std::move(ownedPreview));
    }
    preview->SetSprite(
        payload->TextureGuid,
        payload->U, payload->V, payload->Width, payload->Height,
        0xFFFFFFFFu);

    // The typed preview draws the atlas UV rectangle directly. Remove the
    // full-texture background installed from the thumbnail identity string.
    icon.Styles()
        .ResetBackgroundImage()
        .ResetBackgroundSize()
        .ResetBackgroundPosition()
        .ResetBackgroundRepeat();
    icon.Overrides().Reset(Style::BackgroundTint);
    if (badge)
    {
        badge->SetText("");
        badge->AddClass("hidden");
    }
}

void EnsureFlareAtlasSpriteGhostRendererRegistered()
{
    static const bool registered = []
    {
        UI::Interaction::RegisterDragGhostRenderer(
            UI::Interaction::GetPayloadTypeId<FlareAtlasSpriteDragPayload>(),
            &RenderFlareAtlasSpriteGhost);
        return true;
    }();
    (void)registered;
}

class FlareSpriteDragPreview final : public FlareElementPreview
{
public:
    void SetDragSprite(const GUID& atlasGuid, std::string spriteName)
    {
        m_AtlasGuid = atlasGuid;
        m_SpriteName = std::move(spriteName);
    }

    void OnEvent(UIEvent& event) override
    {
        if (event.Id == kEventMouseDown && event.Button == 0 &&
            ContainsPoint(event.X, event.Y) && !m_AtlasGuid.IsNull() &&
            !m_SpriteName.empty())
        {
            m_DragCandidate = true;
            m_DragStartX = event.X;
            m_DragStartY = event.Y;
            event.Capture(this);
            event.Stop();
            return;
        }

        if (event.Id == kEventMouseMove && m_DragCandidate)
        {
            constexpr float kDragThresholdPx = 5.0f;
            const float dx = event.X - m_DragStartX;
            const float dy = event.Y - m_DragStartY;
            if ((dx * dx + dy * dy) < (kDragThresholdPx * kDragThresholdPx))
                return;

            UIManager* manager = GetOwnerManager();
            UI::Interaction::DragDropManager* dragDrop =
                manager ? manager->GetDragDropManager() : nullptr;
            if (dragDrop && !dragDrop->IsDragging())
            {
                FlareAtlasSpriteDragPayload payload;
                payload.AtlasGuid = m_AtlasGuid;
                payload.SpriteName = m_SpriteName;
                CopySpriteRegionTo(payload);
                const std::string ghostThumbnail = payload.TextureGuid.IsNull()
                                                       ? std::string{}
                                                       : payload.TextureGuid.ToString();
                auto dragPayload = UI::Interaction::DragPayload::Create(std::move(payload));
                dragPayload.DisplayLabel = m_SpriteName;
                if (!ghostThumbnail.empty())
                    dragPayload.GhostThumbnailEngineName = ghostThumbnail;
                else
                    dragPayload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                UI::Interaction::DragSessionContext context{};
                context.SourceWidgetId = GetInstanceId();
                EnsureFlareAtlasSpriteGhostRendererRegistered();
                dragDrop->BeginDrag(std::move(dragPayload), context);
            }
            // A threshold crossing consumes this gesture even if a different
            // drag session was already active. Mouse-up will arm a fresh one.
            m_DragCandidate = false;
            event.Stop();
            return;
        }

        if (event.Id == kEventMouseUp && event.Button == 0)
        {
            m_DragCandidate = false;
            event.Stop();
            return;
        }
        FlareElementPreview::OnEvent(event);
    }

private:
    GUID m_AtlasGuid = GUID::Null();
    std::string m_SpriteName;
    bool m_DragCandidate = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
};

void FlareColorToPickerState(const LensFlare::Color& color, uint32_t& outArgb, float& outIntensity)
{
    const auto state =
        InspectorUI::HdrColorToPickerState({color.R, color.G, color.B, color.A}, kFlareColorPickerMaxIntensity);
    outArgb = state.Argb;
    outIntensity = state.Intensity;
}

LensFlare::Color PickerStateToFlareColor(uint32_t argb, float intensity)
{
    const ColorLinear linear = InspectorUI::PickerStateToHdrColor(argb, intensity, kFlareColorPickerMaxIntensity);
    LensFlare::Color color;
    color.R = linear.r;
    color.G = linear.g;
    color.B = linear.b;
    color.A = linear.a;
    return color;
}

std::string FormatFlareColor(const LensFlare::Color& color)
{
    char buffer[72];
    std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f, %.2f)",
                  color.R, color.G, color.B, color.A);
    return buffer;
}

void StyleFlareElementHeaderSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    constexpr uint32_t borderColor = 0xFF666666u;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kFlareElementHeaderSwatchSize))
        .Set(Style::Height, StyleLength::Px(kFlareElementHeaderSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kFlareElementHeaderSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kFlareElementHeaderSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{2.0f, 2.0f, 2.0f, 2.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor,
             BorderColorsTRBL{borderColor, borderColor, borderColor, borderColor})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::PointerEvents, false);
}

void ConfigureFlareElementPreview(FlareElementPreview* preview,
                                  const FlareAtlasAsset* atlas,
                                  const LensFlare::FlareElement& element,
                                  uint32_t tintArgb,
                                  float previewSize = kFlareElementPreviewSize)
{
    if (!preview)
        return;

    preview->Overrides()
        .Set(Style::Width, StyleLength::Px(previewSize))
        .Set(Style::Height, StyleLength::Px(previewSize))
        .Set(Style::MinWidth, StyleLength::Px(previewSize))
        .Set(Style::MinHeight, StyleLength::Px(previewSize))
        .Set(Style::BackgroundColor, 0xFF111111u)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::OverflowProp, Overflow::Hidden);
    preview->SetTooltip(element.SpriteName.empty() ? "No atlas sprite" : element.SpriteName);

    if (!atlas || atlas->GetTextureRef().empty())
        return;
    const int32_t spriteIndex = atlas->FindSprite(element.SpriteName);
    if (spriteIndex < 0 || static_cast<size_t>(spriteIndex) >= atlas->GetSprites().size())
        return;

    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const std::string& textureRef = atlas->GetTextureRef();
    GUID textureGuid(textureRef.c_str());
    if (textureGuid.IsNull())
        textureGuid = assets.ResolveAssetGuidFromReference(
            std::filesystem::path(textureRef), atlas->GetPath());
    if (textureGuid.IsNull())
        return;

    const LensFlare::AtlasSprite& sprite = atlas->GetSprites()[static_cast<size_t>(spriteIndex)];
    const float width = std::clamp(sprite.W, 1.0e-4f, 1.0f);
    const float height = std::clamp(sprite.H, 1.0e-4f, 1.0f);
    const float u = std::clamp(sprite.U, 0.0f, 1.0f - width);
    const float v = std::clamp(sprite.V, 0.0f, 1.0f - height);
    preview->SetSprite(textureGuid, u, v, width, height,
                       0xFF000000u | (tintArgb & 0x00FFFFFFu));
}

void UpdateFlareElementHeaderColor(FlareElementPreview* preview,
                                   UIElement* swatch,
                                   const LensFlare::Color& color)
{
    uint32_t argb = 0u;
    float intensity = 1.0f;
    FlareColorToPickerState(color, argb, intensity);
    if (preview)
        preview->SetTint(0xFF000000u | (argb & 0x00FFFFFFu));
    StyleFlareElementHeaderSwatch(swatch, argb);
    if (swatch)
        swatch->SetTooltip("Tint " + FormatFlareColor(color));
}

void AddFlareColorPickerRow(UIElement* parent,
                            const std::string& label,
                            FlareAsset* flare,
                            Editor::UndoRedoService* undo,
                            LensFlare::Color value,
                            FlareColorAccess access,
                            OpenColorPickerWindowFn openPicker,
                            const char* tooltip = nullptr,
                            FlareColorUiChanged onColorUiChanged = {})
{
    if (!parent || !flare || !access)
        return;

    UIElement* row = InspectorUI::AddRow(parent);
    Label* rowLabel = InspectorUI::AddLabel(
        row, label, tooltip ? tooltip : "Click to open the HDR color picker");
    if (rowLabel)
        rowLabel->AddClass("inspector-label-no-drag");
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    uint32_t argb = 0u;
    float intensity = 1.0f;
    FlareColorToPickerState(value, argb, intensity);
    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    InspectorUI::StyleColorSwatch(swatchRaw, argb);
    field->AddChild(std::move(swatch));

    auto valueLabel = std::make_unique<Label>();
    valueLabel->AddClass("inspector-text");
    valueLabel->SetText(FormatFlareColor(value));
    valueLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* valueLabelRaw = valueLabel.get();
    field->AddChild(std::move(valueLabel));

    // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
    // refs instead of holding the freed widgets.
    const auto updateUi = [flare, access, swatchRef = UIElement::MakeWeakRef(swatchRaw),
                           valueLabelRef = UIElement::MakeWeakRef(valueLabelRaw), onColorUiChanged]
    {
        LensFlare::Color* current = access(*flare);
        UIElement* swatch = swatchRef.Get();
        Label* valueLabel = valueLabelRef.Get();
        if (!current || !swatch || !valueLabel)
            return;
        uint32_t currentArgb = 0u;
        float currentIntensity = 1.0f;
        FlareColorToPickerState(*current, currentArgb, currentIntensity);
        InspectorUI::StyleColorSwatch(swatch, currentArgb);
        valueLabel->SetText(FormatFlareColor(*current));
        if (onColorUiChanged)
            onColorUiChanged(*current);
    };

    const auto clickHandler = [flare, undo, label, access, openPicker, updateUi](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Stop();
        if (!openPicker || !flare)
            return;
        LensFlare::Color* current = access(*flare);
        if (!current)
            return;
        const LensFlare::Color original = *current;
        uint32_t initialArgb = 0u;
        float initialIntensity = 1.0f;
        FlareColorToPickerState(original, initialArgb, initialIntensity);

        using OptionalEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
        auto edit = std::make_shared<OptionalEdit>();
        const std::string changeName = "Change " + label;
        const auto preview = [flare, undo, access, edit, changeName, updateUi](
                                 uint32_t newArgb, float newIntensity)
        {
            if (undo && !edit->has_value())
                edit->emplace(undo->BeginInteractiveEdit(
                    changeName, MakeFlareSnapshotTarget(flare, changeName)));
            if (LensFlare::Color* color = access(*flare))
                *color = PickerStateToFlareColor(newArgb, newIntensity);
            updateUi();
        };

        ColorPickerCallbacks callbacks;
        callbacks.onValueChanging = preview;
        callbacks.onApply = [flare, undo, access, edit, changeName, updateUi](
                                uint32_t newArgb, float newIntensity)
        {
            const LensFlare::Color next = PickerStateToFlareColor(newArgb, newIntensity);
            if (edit->has_value())
            {
                if (LensFlare::Color* color = access(*flare))
                    *color = next;
                auto finished = std::move(edit->value());
                edit->reset();
                if (flare->Save())
                    finished.Commit();
                else
                {
                    finished.Cancel();
                    (void)flare->Reload();
                }
            }
            else
            {
                CommitFlareChange(flare, undo, changeName, [access, next](FlareAsset& asset)
                {
                    if (LensFlare::Color* color = access(asset))
                        *color = next;
                });
            }
            updateUi();
        };
        callbacks.onCancel = [flare, access, edit, original, updateUi]
        {
            if (edit->has_value())
            {
                auto cancelled = std::move(edit->value());
                edit->reset();
                cancelled.Cancel();
            }
            else if (LensFlare::Color* color = access(*flare))
            {
                *color = original;
            }
            updateUi();
        };
        openPicker(initialArgb, initialIntensity, std::move(callbacks));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    valueLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
}

std::unique_ptr<Foldout> MakeFoldout(const std::string& title, bool expanded)
{
    auto foldout = std::make_unique<Foldout>();
    foldout->SetTitle(title);
    foldout->AddClass("rp-foldout");
    foldout->SetExpanded(expanded);
    return foldout;
}

void AddButton(UIElement* parent, const std::string& text, std::function<void()> action)
{
    auto button = std::make_unique<Button>();
    button->SetText(text);
    button->AddClass("inspector-button");
    button->RegisterEventHandler(kEventButtonClick, [action = std::move(action)](UIEvent&) { if (action) action(); });
    parent->AddChild(std::move(button));
}

GUID ResolveReferenceGuid(AssetManager& assets, const FlareAsset& flare, const std::string& ref)
{
    if (ref.empty())
        return GUID::Null();
    const GUID direct(ref.c_str());
    if (!direct.IsNull())
        return direct;
    return assets.ResolveAssetGuidFromReference(std::filesystem::path(ref), flare.GetPath());
}

constexpr float kFlareCurveCompactHeightPx = 110.0f;
constexpr float kFlareCurveExpandedAspectRatio = 2.45f;

std::unordered_map<std::string, bool>& FlareCurveExpandedStates()
{
    static std::unordered_map<std::string, bool> states;
    return states;
}

std::vector<Math::CurveKey> NormalizeFlareEditorKeys(std::vector<Math::CurveKey> keys)
{
    std::sort(keys.begin(), keys.end(), [](const Math::CurveKey& a, const Math::CurveKey& b)
    {
        return a.Time < b.Time;
    });
    if (keys.size() > Math::Curve::Capacity)
        keys.resize(Math::Curve::Capacity);

    const std::vector<Math::CurveKey> source = keys;
    const auto segmentSlope = [](const Math::CurveKey& a, const Math::CurveKey& b)
    {
        return (b.Value - a.Value) / std::max(1.0e-4f, b.Time - a.Time);
    };
    for (size_t i = 0; i < keys.size(); ++i)
    {
        float inTangent = source[i].InTangent;
        float outTangent = source[i].OutTangent;
        if (i > 0)
        {
            const Math::CurveKey& previous = source[i - 1];
            if (previous.Interp == Math::CurveInterp::Linear)
                inTangent = segmentSlope(previous, source[i]);
            else if (previous.Interp == Math::CurveInterp::Smooth)
                inTangent = Math::ResolveKeySlope(source.data(), static_cast<uint32_t>(source.size()),
                                                  static_cast<uint32_t>(i), true);
        }
        if (i + 1 < source.size())
        {
            if (source[i].Interp == Math::CurveInterp::Linear)
                outTangent = segmentSlope(source[i], source[i + 1]);
            else if (source[i].Interp == Math::CurveInterp::Smooth)
                outTangent = Math::ResolveKeySlope(source.data(), static_cast<uint32_t>(source.size()),
                                                   static_cast<uint32_t>(i), false);
        }
        keys[i].Time = std::clamp(keys[i].Time, 0.0f, 1.0f);
        keys[i].InTangent = inTangent;
        keys[i].OutTangent = outTangent;
        keys[i].Interp = Math::CurveInterp::Smooth;
        keys[i].TangentMode = Math::CurveTangentMode::Broken;
    }
    return keys;
}

std::vector<Math::CurveKey> FlareEditorKeys(const FlareAsset& flare, bool angleCurve)
{
    const auto& source = angleCurve ? flare.GetGlobals().AngleCurveKeys
                                    : flare.GetGlobals().DynamicEdgeCurveKeys;
    std::vector<Math::CurveKey> keys;
    keys.reserve(std::max<size_t>(source.size(), angleCurve ? 2u : 3u));
    for (const LensFlare::CurveKeyData& stored : source)
    {
        Math::CurveKey key;
        key.Time = stored.Time;
        key.Value = stored.Value;
        key.InTangent = stored.InTangent;
        key.OutTangent = stored.OutTangent;
        key.Interp = Math::CurveInterp::Smooth;
        key.TangentMode = Math::CurveTangentMode::Broken;
        keys.push_back(key);
    }

    // Empty authored arrays use these same runtime fallbacks. Showing them in
    // the graph gives a useful starting point without writing the asset until
    // the user actually edits the curve.
    if (keys.empty() && angleCurve)
    {
        keys = {{0.0f, 0.0f, 1.0f, 1.0f, Math::CurveInterp::Smooth,
                 Math::CurveTangentMode::Broken},
                {1.0f, 1.0f, 1.0f, 1.0f, Math::CurveInterp::Smooth,
                 Math::CurveTangentMode::Broken}};
    }
    else if (keys.empty())
    {
        keys = {{0.0f, 0.0f, 2.0f, 2.0f, Math::CurveInterp::Smooth,
                 Math::CurveTangentMode::Broken},
                {0.5f, 1.0f, 2.0f, -2.0f, Math::CurveInterp::Smooth,
                 Math::CurveTangentMode::Broken},
                {1.0f, 0.0f, -2.0f, -2.0f, Math::CurveInterp::Smooth,
                 Math::CurveTangentMode::Broken}};
    }
    return NormalizeFlareEditorKeys(std::move(keys));
}

void StoreFlareEditorKeys(FlareAsset& flare, bool angleCurve,
                          const std::vector<Math::CurveKey>& keys)
{
    auto& destination = angleCurve ? flare.EditGlobals().AngleCurveKeys
                                   : flare.EditGlobals().DynamicEdgeCurveKeys;
    destination.clear();
    destination.reserve(keys.size());
    for (const Math::CurveKey& key : keys)
    {
        destination.push_back({key.Time, key.Value, key.InTangent, key.OutTangent});
    }
}

void BuildCurveEditor(UIElement* parent,
                      FlareAsset* flare,
                      Editor::UndoRedoService* undo,
                      bool angleCurve,
                      Platform::Window* window)
{
    if (!parent || !flare)
        return;

    const std::string label = angleCurve ? "Angle Curve" : "Dynamic Edge Curve";
    const std::string changeName = angleCurve ? "Edit Flare Angle Curve"
                                               : "Edit Flare Dynamic Edge Curve";
    const std::string stateKey = flare->GetPath().string() + (angleCurve ? "#angle" : "#edge");
    auto expanded = std::make_shared<bool>(FlareCurveExpandedStates()[stateKey]);

    UIElement* row = InspectorUI::AddRow(parent);
    row->Overrides().Set(Style::AlignItems, AlignItems::FlexStart);
    if (Label* rowLabel = InspectorUI::AddLabel(row, label,
            "Drag keys and tangent handles. Right-click empty space to add a key."))
        rowLabel->AddClass("inspector-label-no-drag");

    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::Gap, StyleLength::Px(4.0f));

    auto graph = std::make_unique<CurveField>();
    CurveField* graphRaw = graph.get();
    CurveField::Config config;
    config.TimeMin = 0.0f;
    config.TimeMax = 1.0f;
    config.ValueMin = 0.0f;
    config.ValueMax = 1.0f;
    config.AllowTimeDrag = true;
    config.AllowAddRemove = true;
    config.AllowTangentEditing = true;
    config.MinKeys = 2;
    config.MaxKeys = Math::Curve::Capacity;
    graph->SetConfig(config);
    graph->SetKeys(FlareEditorKeys(*flare, angleCurve));
    graph->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::Height, StyleLength::Px(kFlareCurveCompactHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kFlareCurveCompactHeightPx))
        .Set(Style::MaxHeight, StyleLength::Px(kFlareCurveCompactHeightPx))
        .Set(Style::MinWidth, StyleLength::Px(160.0f));
    field->AddChild(std::move(graph));

    using OptionalEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto edit = std::make_shared<OptionalEdit>();
    const auto preview = [flare, undo, edit, angleCurve, changeName](const std::vector<Math::CurveKey>& raw)
    {
        if (!flare)
            return;
        if (undo && !edit->has_value())
            edit->emplace(undo->BeginInteractiveEdit(changeName,
                                                      MakeFlareSnapshotTarget(flare, changeName)));
        StoreFlareEditorKeys(*flare, angleCurve, NormalizeFlareEditorKeys(raw));
    };
    const auto commit = [flare, undo, edit, angleCurve, changeName, graphRaw](
                            const std::vector<Math::CurveKey>& raw)
    {
        if (!flare)
            return;
        const std::vector<Math::CurveKey> keys = NormalizeFlareEditorKeys(raw);
        graphRaw->SetKeys(keys);
        if (edit->has_value())
        {
            StoreFlareEditorKeys(*flare, angleCurve, keys);
            auto finished = std::move(edit->value());
            edit->reset();
            if (flare->Save())
                finished.Commit();
            else
            {
                finished.Cancel();
                (void)flare->Reload();
            }
            return;
        }
        CommitFlareChange(flare, undo, changeName, [angleCurve, keys](FlareAsset& asset)
        {
            StoreFlareEditorKeys(asset, angleCurve, keys);
        });
    };

    auto readout = std::make_unique<UIElement>();
    readout->AddClass("sky-scalar-curve-readout");
    UIElement* readoutRaw = readout.get();
    field->AddChild(std::move(readout));
    auto syncReadoutFn = std::make_shared<std::function<void()>>();

    FloatField* timeField = InspectorDrag::AddFloatRowWithDrag(
        readoutRaw, "Time", 0.0f,
        [graphRaw, preview](float value)
        {
            if (graphRaw->GetSelectedKey() < 0)
                return;
            graphRaw->SetSelectedKeyTime(value);
            preview(graphRaw->GetKeys());
        },
        [graphRaw, commit, syncReadoutFn](float value)
        {
            if (graphRaw->GetSelectedKey() < 0)
                return;
            graphRaw->SetSelectedKeyTime(value);
            commit(graphRaw->GetKeys());
            if (*syncReadoutFn) (*syncReadoutFn)();
        }, 0.0f, "Selected key time", 0.0f, 1.0f);
    FloatField* valueField = InspectorDrag::AddFloatRowWithDrag(
        readoutRaw, "Value", 0.0f,
        [graphRaw, preview](float value)
        {
            if (graphRaw->GetSelectedKey() < 0)
                return;
            graphRaw->SetSelectedKeyValue(value);
            preview(graphRaw->GetKeys());
        },
        [graphRaw, commit, syncReadoutFn](float value)
        {
            if (graphRaw->GetSelectedKey() < 0)
                return;
            graphRaw->SetSelectedKeyValue(value);
            commit(graphRaw->GetKeys());
            if (*syncReadoutFn) (*syncReadoutFn)();
        }, 0.0f, "Selected key value");

    const auto syncReadout = [graphRaw, timeField, valueField]()
    {
        const int selected = graphRaw->GetSelectedKey();
        const auto& keys = graphRaw->GetKeys();
        if (selected < 0 || selected >= static_cast<int>(keys.size()))
            return;
        timeField->SetValueWithoutNotify(keys[static_cast<size_t>(selected)].Time);
        valueField->SetValueWithoutNotify(keys[static_cast<size_t>(selected)].Value);
    };
    *syncReadoutFn = syncReadout;

    graphRaw->SetOnChanging([preview, syncReadout](const std::vector<Math::CurveKey>& keys)
    {
        preview(keys);
        syncReadout();
    });
    graphRaw->SetOnChanged([commit, syncReadout](const std::vector<Math::CurveKey>& keys)
    {
        commit(keys);
        syncReadout();
    });
    graphRaw->SetOnSelectionChanged([syncReadout](int) { syncReadout(); });
    graphRaw->SetOnKeyContextMenu([graphRaw, window](int keyIndex, float x, float y)
    {
        if (!window || !graphRaw)
            return;
        static std::shared_ptr<INativeContextMenu> menu;
        if (!menu)
            menu = CreateContextMenu();
        if (!menu)
            return;
        constexpr uint32_t kDeleteKey = 1;
        const bool canDelete = graphRaw->GetKeys().size() > 2;
        menu->Clear();
        menu->SetCommandHandler([graphRaw, keyIndex](uint32_t command)
        {
            if (command == kDeleteKey)
                graphRaw->DeleteKey(keyIndex);
        });
        menu->AddItem(0, "Delete Key", kDeleteKey,
                      canDelete ? MenuItemFlag_None : MenuItemFlag_Disabled);
        menu->SetItemIcon(kDeleteKey, EditorIcons::kTrash);
        menu->Show(window, static_cast<int>(x), static_cast<int>(y));
    });

    auto presetFoldout = MakeFoldout("Presets", false);
    presetFoldout->AddClass("curve-preset-foldout");
    UIElement* presetContent = presetFoldout->GetContentContainer();
    auto picker = std::make_unique<CurvePresetPicker>();
    CurvePresetPicker* pickerRaw = picker.get();
    picker->SetContextMenuWindow(window);
    picker->SetOnPick([graphRaw, commit, syncReadout](const CurvePresets::CurvePreset& preset)
    {
        const Math::DynamicCurve curve = CurvePresets::PresetAsDynamicCurve(preset);
        std::vector<Math::CurveKey> keys = NormalizeFlareEditorKeys(curve.Keys);
        graphRaw->SetKeys(keys);
        commit(keys);
        graphRaw->SetSelectedKey(0);
        syncReadout();
    });
    picker->SetOnRequestSave([graphRaw, pickerRaw]
    {
        Math::DynamicCurve curve;
        curve.Keys = graphRaw->GetKeys();
        for (Math::CurveKey& key : curve.Keys)
        {
            key.Time = std::clamp(key.Time, 0.0f, 1.0f);
            key.Value = std::clamp(key.Value, 0.0f, 1.0f);
        }
        CurvePresets::UserPresetLibrary::Get().Add(CurvePresets::NextUserPresetName(), curve);
        pickerRaw->Refresh();
    });
    presetContent->AddChild(std::move(picker));
    field->AddChild(std::move(presetFoldout));

    const auto applyExpanded = [row, field, graphRaw, expanded, stateKey](bool value)
    {
        *expanded = value;
        FlareCurveExpandedStates()[stateKey] = value;
        if (value)
        {
            row->AddClass("sky-scalar-curve-expanded");
            row->Overrides().Set(Style::FlexDir, FlexDirection::Column)
                            .Set(Style::AlignItems, AlignItems::Stretch);
            field->Overrides().Set(Style::Width, StyleLength::Percent(100.0f))
                              .Set(Style::MaxWidth, StyleLength::Percent(100.0f))
                              .Set(Style::AlignSelf, AlignItems::Stretch);
            graphRaw->Overrides().Set(Style::Width, StyleLength::Percent(100.0f))
                                 .Set(Style::Height, StyleLength::Auto())
                                 .Set(Style::MinHeight, StyleLength::Px(kFlareCurveCompactHeightPx))
                                 .Set(Style::MaxHeight, StyleLength::Auto())
                                 .Set(Style::AspectRatio, kFlareCurveExpandedAspectRatio);
        }
        else
        {
            row->RemoveClass("sky-scalar-curve-expanded");
            row->Overrides().Reset(Style::FlexDir).Set(Style::AlignItems, AlignItems::FlexStart);
            field->Overrides().Reset(Style::Width).Reset(Style::MaxWidth).Reset(Style::AlignSelf);
            graphRaw->Overrides().Reset(Style::Width)
                                 .Set(Style::Height, StyleLength::Px(kFlareCurveCompactHeightPx))
                                 .Set(Style::MinHeight, StyleLength::Px(kFlareCurveCompactHeightPx))
                                 .Set(Style::MaxHeight, StyleLength::Px(kFlareCurveCompactHeightPx))
                                 .Reset(Style::AspectRatio);
        }
    };
    applyExpanded(*expanded);
    graphRaw->SetOnCurveDoubleClick([expanded, applyExpanded] { applyExpanded(!*expanded); });
    graphRaw->SetSelectedKey(0);
}

void BuildGlobalSettings(UIElement* parent,
                         FlareAsset* flare,
                         Editor::UndoRedoService* undo,
                         const std::function<void()>& rebuild,
                         Platform::Window* window,
                         OpenColorPickerWindowFn openPicker)
{
    const LensFlare::FlareGlobals& g = flare->GetGlobals();

    auto general = MakeFoldout("General", true);
    UIElement* generalContent = general->GetContentContainer();
    AddFlareFloatValue(generalContent, flare, undo, "Scale", g.GlobalScale,
        [](FlareAsset& a, float v) { a.EditGlobals().GlobalScale = std::max(0.0f, v); }, 1.0f,
        "Global size multiplier", 0.0f);
    AddFlareFloatValue(generalContent, flare, undo, "Brightness", g.GlobalBrightness,
        [](FlareAsset& a, float v) { a.EditGlobals().GlobalBrightness = std::max(0.0f, v); }, 1.0f,
        "Global brightness multiplier", 0.0f);
    AddFlareToggle(generalContent, flare, undo, "Multiply Scale By Transform", g.MultiplyScaleByTransformScale,
        [](FlareAsset& a, bool v) { a.EditGlobals().MultiplyScaleByTransformScale = v; });
    AddFlareColorPickerRow(generalContent, "Global Tint", flare, undo, g.GlobalTint,
        [](FlareAsset& asset) { return &asset.EditGlobals().GlobalTint; }, openPicker,
        "Global HDR tint multiplied into every flare element");
    AddFlareFloatValue(generalContent, flare, undo, "Off Screen Fade Distance", g.OffScreenFadeDist,
        [](FlareAsset& a, float v) { a.EditGlobals().OffScreenFadeDist = std::max(0.0f, v); }, 0.4f, nullptr, 0.0f);
    AddFlareToggle(generalContent, flare, undo, "Never Cull", g.NeverCull,
        [](FlareAsset& a, bool v) { a.EditGlobals().NeverCull = v; });
    parent->AddChild(std::move(general));

    auto angle = MakeFoldout("Angle Falloff", false);
    UIElement* angleContent = angle->GetContentContainer();
    AddFlareToggle(angleContent, flare, undo, "Use Angle Limit", g.UseAngleLimit,
        [](FlareAsset& a, bool v) { a.EditGlobals().UseAngleLimit = v; }, rebuild);
    if (g.UseAngleLimit)
    {
        AddFlareFloatValue(angleContent, flare, undo, "Max Angle", g.MaxAngle,
            [](FlareAsset& a, float v) { a.EditGlobals().MaxAngle = std::clamp(v, 0.0f, 180.0f); }, 90.0f, nullptr, 0.0f, 180.0f);
        AddFlareToggle(angleContent, flare, undo, "Affect Scale", g.UseAngleScale,
            [](FlareAsset& a, bool v) { a.EditGlobals().UseAngleScale = v; });
        AddFlareToggle(angleContent, flare, undo, "Affect Brightness", g.UseAngleBrightness,
            [](FlareAsset& a, bool v) { a.EditGlobals().UseAngleBrightness = v; });
        AddFlareToggle(angleContent, flare, undo, "Use Curve", g.UseAngleCurve,
            [](FlareAsset& a, bool v) { a.EditGlobals().UseAngleCurve = v; }, rebuild);
        if (g.UseAngleCurve)
            BuildCurveEditor(angleContent, flare, undo, true, window);
    }
    parent->AddChild(std::move(angle));

    auto distance = MakeFoldout("Distance Falloff", false);
    UIElement* distanceContent = distance->GetContentContainer();
    AddFlareToggle(distanceContent, flare, undo, "Use Max Distance", g.UseMaxDistance,
        [](FlareAsset& a, bool v) { a.EditGlobals().UseMaxDistance = v; }, rebuild);
    if (g.UseMaxDistance)
    {
        AddFlareFloatValue(distanceContent, flare, undo, "Max Distance", g.MaxDistance,
            [](FlareAsset& a, float v) { a.EditGlobals().MaxDistance = std::max(0.0f, v); }, 150.0f, nullptr, 0.0f);
        AddFlareToggle(distanceContent, flare, undo, "Affect Scale", g.UseDistanceScale,
            [](FlareAsset& a, bool v) { a.EditGlobals().UseDistanceScale = v; });
        AddFlareToggle(distanceContent, flare, undo, "Affect Brightness", g.UseDistanceFade,
            [](FlareAsset& a, bool v) { a.EditGlobals().UseDistanceFade = v; });
    }
    parent->AddChild(std::move(distance));

    auto dynamics = MakeFoldout("Dynamic Boost", false);
    UIElement* dynamicsContent = dynamics->GetContentContainer();
    AddFlareToggle(dynamicsContent, flare, undo, "Use Edge Brightness", g.UseDynamicEdgeBoost,
        [](FlareAsset& a, bool v) { a.EditGlobals().UseDynamicEdgeBoost = v; }, rebuild);
    if (g.UseDynamicEdgeBoost)
    {
        AddFlareFloatValue(dynamicsContent, flare, undo, "Edge Brightness", g.DynamicEdgeBrightness,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeBrightness = v; }, 0.1f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Edge Range", g.DynamicEdgeRange,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeRange = std::max(0.0f, v); }, 0.3f, nullptr, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Edge Bias", g.DynamicEdgeBias,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeBias = v; }, -0.1f);
    }
    AddFlareToggle(dynamicsContent, flare, undo, "Use Edge Scale", g.UseDynamicEdgeScale,
        [](FlareAsset& a, bool v) { a.EditGlobals().UseDynamicEdgeScale = v; }, rebuild);
    if (g.UseDynamicEdgeScale)
    {
        AddFlareFloatValue(dynamicsContent, flare, undo, "Edge Scale", g.DynamicEdgeScale,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeScale = v; }, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Scale Range", g.DynamicEdgeScaleRange,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeScaleRange = std::max(0.0f, v); }, 0.3f, nullptr, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Scale Bias", g.DynamicEdgeScaleBias,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicEdgeScaleBias = v; }, 0.0f);
    }
    if (g.UseDynamicEdgeBoost || g.UseDynamicEdgeScale)
        BuildCurveEditor(dynamicsContent, flare, undo, false, window);
    AddFlareToggle(dynamicsContent, flare, undo, "Use Center Boost", g.UseDynamicCenterBoost,
        [](FlareAsset& a, bool v) { a.EditGlobals().UseDynamicCenterBoost = v; }, rebuild);
    if (g.UseDynamicCenterBoost)
    {
        AddFlareFloatValue(dynamicsContent, flare, undo, "Center Brightness", g.DynamicCenterBrightness,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicCenterBrightness = v; }, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Center Scale", g.DynamicCenterScale,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicCenterScale = v; }, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Center Range", g.DynamicCenterRange,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicCenterRange = std::max(0.0f, v); }, 0.3f, nullptr, 0.0f);
        AddFlareFloatValue(dynamicsContent, flare, undo, "Center Bias", g.DynamicCenterBias,
            [](FlareAsset& a, float v) { a.EditGlobals().DynamicCenterBias = v; }, 0.0f);
    }
    parent->AddChild(std::move(dynamics));
}

struct FlareVisibilityPaintEdit
{
    FlareAsset* Flare = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    std::function<void()> Rebuild;
    std::optional<Editor::UndoRedoService::InteractiveEdit> Edit;
    bool Active = false;
    bool Dirty = false;

    ~FlareVisibilityPaintEdit()
    {
        End();
    }

    void Begin()
    {
        if (Active)
            End();
        Active = true;
        Dirty = false;
        if (Undo && Flare)
        {
            constexpr const char* label = "Change Flare Element Visibility";
            Edit.emplace(Undo->BeginInteractiveEdit(label, MakeFlareSnapshotTarget(Flare, label)));
        }
    }

    void Apply(size_t index, bool visible)
    {
        if (!Active || !Flare || index >= Flare->EditElements().size())
            return;
        Flare->EditElements()[index].Visible = visible;
        Dirty = true;
    }

    void End()
    {
        if (!Active)
            return;
        Active = false;

        const bool saved = !Dirty || (Flare && Flare->Save());
        if (Edit.has_value())
        {
            auto finished = std::move(Edit.value());
            Edit.reset();
            if (saved)
                finished.Commit();
            else
                finished.Cancel();
        }
        else if (!saved && Flare)
        {
            (void)Flare->Reload();
        }

        Dirty = false;
        if (!saved && Rebuild)
            Rebuild();
    }
};

void BuildElementSettings(UIElement* parent,
                          FlareAsset* flare,
                          FlareAtlasAsset* atlas,
                          Editor::UndoRedoService* undo,
                          const std::function<void()>& rebuild,
                          Platform::Window* window,
                          OpenColorPickerWindowFn openPicker)
{
    const auto& elements = flare->GetElements();
    auto visibilityEdit = std::make_shared<FlareVisibilityPaintEdit>();
    visibilityEdit->Flare = flare;
    visibilityEdit->Undo = undo;
    visibilityEdit->Rebuild = rebuild;

    auto visibilityPaint = std::make_shared<InspectorDrag::ToggleDragPaintGroup>(
        [visibilityEdit](bool) { visibilityEdit->Begin(); },
        [visibilityEdit](size_t index, bool visible, ToggleBase& toggle)
        {
            visibilityEdit->Apply(index, visible);
            toggle.SetTooltip(visible ? "Visible - click or drag to hide"
                                      : "Hidden - click or drag to show");
        },
        [visibilityEdit]() { visibilityEdit->End(); });

    for (size_t i = 0; i < elements.size(); ++i)
    {
        const LensFlare::FlareElement& e = elements[i];
        const std::string title = e.SpriteName.empty()
            ? "Element " + std::to_string(i)
            : "Element " + std::to_string(i) + " - " + e.SpriteName;
        auto elementFoldout = MakeFoldout(title, i == 0);
        FlareElementPreview* headerPreviewRaw = nullptr;
        UIElement* headerSwatchRaw = nullptr;
        if (UIElement* header = elementFoldout->GetHeader())
        {
            header->Overrides()
                .Set(Style::PaddingTop, StyleLength::Px(2.0f))
                .Set(Style::PaddingRight, StyleLength::Px(6.0f))
                .Set(Style::PaddingBottom, StyleLength::Px(2.0f))
                .Set(Style::PaddingLeft, StyleLength::Px(6.0f))
                .Set(Style::Gap, StyleLength::Px(6.0f));

            auto accessories = std::make_unique<UIElement>();
            UIElement* accessoriesRaw = accessories.get();
            accessoriesRaw->Overrides()
                .Set(Style::Display, DisplayMode::Flex)
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(4.0f))
                .Set(Style::PointerEvents, true);

            auto summary = std::make_unique<UIElement>();
            UIElement* summaryRaw = summary.get();
            summaryRaw->Overrides()
                .Set(Style::Display, DisplayMode::Flex)
                .Set(Style::Visibility, !elementFoldout->IsExpanded())
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(4.0f))
                .Set(Style::PointerEvents, false);

            auto preview = std::make_unique<FlareElementPreview>();
            headerPreviewRaw = preview.get();
            uint32_t tintArgb = 0u;
            float tintIntensity = 1.0f;
            FlareColorToPickerState(e.Tint, tintArgb, tintIntensity);
            ConfigureFlareElementPreview(headerPreviewRaw, atlas, e, tintArgb);
            summaryRaw->AddChild(std::move(preview));

            auto swatch = std::make_unique<UIElement>();
            headerSwatchRaw = swatch.get();
            StyleFlareElementHeaderSwatch(headerSwatchRaw, tintArgb);
            headerSwatchRaw->SetTooltip("Tint " + FormatFlareColor(e.Tint));
            summaryRaw->AddChild(std::move(swatch));

            accessoriesRaw->AddChild(std::move(summary));

            auto visibility = visibilityPaint->CreateToggle(i, e.Visible);
            ToggleBase* visibilityRaw = visibility.get();
            visibilityRaw->AddClass("flare-element-visibility-dot");
            visibilityRaw->SetTooltip(
                e.Visible ? "Visible - click or drag to hide"
                          : "Hidden - click or drag to show");
            auto visibilityDot = std::make_unique<UIElement>();
            visibilityDot->AddClass("flare-element-visibility-dot-visual");
            visibilityRaw->AddChild(std::move(visibilityDot));
            header->InsertChild(0, std::move(visibility));

            header->AddChild(std::move(accessories));

            auto menuButton = std::make_unique<Button>();
            menuButton->AddClass("icon-button");
            menuButton->AddClass("inspector-section-header-options");
            menuButton->AddClass("flare-element-header-menu");
            menuButton->SetTooltip("Flare element options");
            menuButton->RegisterEventHandler(kEventButtonClick, [flare, undo, i, rebuild, window](UIEvent& e)
            {
                UIElement& button = *e.CurrentTarget;
                if (!window)
                    return;

                static std::shared_ptr<INativeContextMenu> menu;
                if (!menu)
                    menu = CreateContextMenu();
                if (!menu)
                    return;

                constexpr uint32_t kCmdClone = 1;
                constexpr uint32_t kCmdRemove = 2;
                constexpr uint32_t kCmdReset = 3;
                menu->Clear();
                menu->AddItem(0, "Reset", kCmdReset);
                menu->SetItemIcon(kCmdReset, EditorIcons::kReset);
                menu->AddItem(0, "Clone", kCmdClone);
                menu->SetItemIcon(kCmdClone, EditorIcons::kCopy);
                menu->AddItem(0, "Remove", kCmdRemove);
                menu->SetItemIcon(kCmdRemove, EditorIcons::kTrash);
                menu->SetCommandHandler([flare, undo, i, rebuild](uint32_t command)
                {
                    if (command == kCmdReset)
                    {
                        CommitFlareChange(flare, undo, "Reset Flare Element", [i](FlareAsset& asset)
                        {
                            auto& list = asset.EditElements();
                            if (i < list.size())
                            {
                                LensFlare::FlareElement reset{};
                                reset.SpriteName = list[i].SpriteName;
                                reset.SpriteIndex = list[i].SpriteIndex;
                                list[i] = std::move(reset);
                            }
                        });
                    }
                    else if (command == kCmdClone)
                    {
                        CommitFlareChange(flare, undo, "Clone Flare Element", [i](FlareAsset& asset)
                        {
                            auto& list = asset.EditElements();
                            if (i < list.size())
                            {
                                list.insert(
                                    list.begin() + static_cast<std::ptrdiff_t>(i + 1), list[i]);
                            }
                        });
                    }
                    else if (command == kCmdRemove)
                    {
                        CommitFlareChange(flare, undo, "Remove Flare Element", [i](FlareAsset& asset)
                        {
                            auto& list = asset.EditElements();
                            if (i < list.size())
                                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                        });
                    }
                    else
                    {
                        return;
                    }

                    if (rebuild)
                        rebuild();
                });

                const int x = static_cast<int>(button.GetLayoutX());
                const int y = static_cast<int>(button.GetLayoutY() + button.GetLayoutHeight());
                menu->Show(window, x, y);
            });
            header->AddChild(std::move(menuButton));

            elementFoldout->SetOnExpandedChanged(
                [summaryRaw](Foldout&, bool expanded)
                {
                    // Preserve the accessory width in both states so opening an
                    // element cannot reflow its header by a few pixels.
                    summaryRaw->Overrides().Set(Style::Visibility, !expanded);
                });
        }
        UIElement* content = elementFoldout->GetContentContainer();
        FlareElementPreview* expandedPreviewRaw = nullptr;

        struct ElementSpriteUiState
        {
            Foldout* ElementFoldout = nullptr;
            Dropdown* SpriteDropdown = nullptr;
            FlareElementPreview* HeaderPreview = nullptr;
            FlareElementPreview* ExpandedPreview = nullptr;
            std::vector<std::string> DropdownValues;
        };
        auto spriteUi = std::make_shared<ElementSpriteUiState>();
        spriteUi->ElementFoldout = elementFoldout.get();
        spriteUi->HeaderPreview = headerPreviewRaw;

        const auto selectSprite = [flare, undo, atlas, i, spriteUi](const std::string& value)
        {
            CommitFlareChange(flare, undo, "Change Flare Sprite", [i, value](FlareAsset& a)
            {
                if (i < a.EditElements().size())
                    a.EditElements()[i].SpriteName = value;
            });

            if (!flare || i >= flare->GetElements().size())
                return;
            const LensFlare::FlareElement& current = flare->GetElements()[i];
            if (spriteUi->SpriteDropdown)
            {
                const auto selected = std::find(
                    spriteUi->DropdownValues.begin(), spriteUi->DropdownValues.end(),
                    current.SpriteName);
                if (selected != spriteUi->DropdownValues.end())
                {
                    spriteUi->SpriteDropdown->SetSelectedIndexWithoutNotify(
                        static_cast<int>(std::distance(spriteUi->DropdownValues.begin(), selected)));
                }
            }
            if (spriteUi->ElementFoldout)
            {
                spriteUi->ElementFoldout->SetTitle(
                    current.SpriteName.empty()
                        ? "Element " + std::to_string(i)
                        : "Element " + std::to_string(i) + " - " + current.SpriteName);
            }
            uint32_t tintArgb = 0u;
            float tintIntensity = 1.0f;
            FlareColorToPickerState(current.Tint, tintArgb, tintIntensity);
            ConfigureFlareElementPreview(spriteUi->HeaderPreview, atlas, current, tintArgb);
            ConfigureFlareElementPreview(
                spriteUi->ExpandedPreview, atlas, current, tintArgb, 80.0f);
        };

        if (atlas && !atlas->GetSprites().empty())
        {
            std::vector<Dropdown::Option> options;
            options.reserve(atlas->GetSprites().size() + 1);
            int selected = 0;
            bool found = false;
            for (const auto& sprite : atlas->GetSprites())
            {
                if (sprite.Name == e.SpriteName)
                {
                    selected = static_cast<int>(options.size());
                    found = true;
                }
                options.push_back({sprite.Name, sprite.Name});
            }
            if (!found && !e.SpriteName.empty())
            {
                options.insert(options.begin(), {e.SpriteName, e.SpriteName + " (missing)"});
                selected = 0;
            }
            spriteUi->DropdownValues.reserve(options.size());
            for (const Dropdown::Option& option : options)
                spriteUi->DropdownValues.push_back(option.value);
            Dropdown* sprite = InspectorUI::AddDropdownRow(content, "Sprite", options, selected,
                "Atlas sprite used by this flare element");
            spriteUi->SpriteDropdown = sprite;
            sprite->SetOnValueChanged(selectSprite);

            uint32_t tintArgb = 0u;
            float tintIntensity = 1.0f;
            FlareColorToPickerState(e.Tint, tintArgb, tintIntensity);

            UIElement* previewRow = InspectorUI::AddRow(content);
            InspectorUI::AddLabel(
                previewRow, "Preview",
                "Drop a sprite from the Atlas Sprites palette below");
            UIElement* previewField = InspectorUI::AddFieldContainer(previewRow);
            previewField->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::JustifyContent, JustifyContent::FlexEnd);
            auto expandedPreview = std::make_unique<FlareSpriteDropPreview>();
            FlareSpriteDropPreview* dropPreview = expandedPreview.get();
            expandedPreviewRaw = dropPreview;
            spriteUi->ExpandedPreview = dropPreview;
            ConfigureFlareElementPreview(dropPreview, atlas, e, tintArgb, 80.0f);
            dropPreview->Overrides()
                .Set(Style::PointerEvents, true)
                .Set(Style::Cursor, CursorStyle::Grab);
            dropPreview->SetTooltip("Drop an atlas sprite here");
            dropPreview->SetAtlasGuid(atlas->GetGUID());
            dropPreview->SetOnSpriteDropped(selectSprite);
            previewField->AddChild(std::move(expandedPreview));

            auto atlasSprites = MakeFoldout("Atlas Sprites - drag onto Preview", false);
            UIElement* palette = atlasSprites->GetContentContainer();
            palette->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::FlexWrap, true)
                .Set(Style::Gap, StyleLength::Px(5.0f))
                .Set(Style::PaddingLeft, StyleLength::Px(0.0f));
            for (const LensFlare::AtlasSprite& atlasSprite : atlas->GetSprites())
            {
                LensFlare::FlareElement paletteElement;
                paletteElement.SpriteName = atlasSprite.Name;
                auto palettePreview = std::make_unique<FlareSpriteDragPreview>();
                FlareSpriteDragPreview* palettePreviewRaw = palettePreview.get();
                ConfigureFlareElementPreview(
                    palettePreviewRaw, atlas, paletteElement, 0xFFFFFFFFu, 34.0f);
                palettePreviewRaw->Overrides()
                    .Set(Style::PointerEvents, true)
                    .Set(Style::Cursor, CursorStyle::Grab);
                palettePreviewRaw->SetTooltip(atlasSprite.Name + " - drag onto Preview");
                palettePreviewRaw->SetDragSprite(atlas->GetGUID(), atlasSprite.Name);
                palette->AddChild(std::move(palettePreview));
            }
            content->AddChild(std::move(atlasSprites));
        }
        else
        {
            InspectorUI::AddLine(content, "Sprite: " + (e.SpriteName.empty() ? std::string("(none)") : e.SpriteName));
        }

        const auto addElementFloat = [content, flare, undo, i](const std::string& label, float value,
                                                                auto member, float defaultValue,
                                                                float minValue = -std::numeric_limits<float>::infinity(),
                                                                float maxValue = std::numeric_limits<float>::infinity())
        {
            AddFlareFloatValue(content, flare, undo, label, value,
                [i, member, minValue, maxValue](FlareAsset& a, float v)
                {
                    if (i < a.EditElements().size())
                        a.EditElements()[i].*member = std::clamp(v, minValue, maxValue);
                }, defaultValue, nullptr, minValue, maxValue);
        };
        addElementFloat("Brightness", e.Brightness, &LensFlare::FlareElement::Brightness, 1.0f, 0.0f);
        addElementFloat("Scale", e.Scale, &LensFlare::FlareElement::Scale, 1.0f, 0.0f);
        addElementFloat("Size X", e.SizeX, &LensFlare::FlareElement::SizeX, 1.0f, 0.0f);
        addElementFloat("Size Y", e.SizeY, &LensFlare::FlareElement::SizeY, 1.0f, 0.0f);
        addElementFloat("Position", e.Position, &LensFlare::FlareElement::Position, 0.0f);
        addElementFloat("Offset X", e.OffsetX, &LensFlare::FlareElement::OffsetX, 0.0f);
        addElementFloat("Offset Y", e.OffsetY, &LensFlare::FlareElement::OffsetY, 0.0f);
        addElementFloat("Anamorphic X", e.AnamorphicX, &LensFlare::FlareElement::AnamorphicX, 0.0f);
        addElementFloat("Anamorphic Y", e.AnamorphicY, &LensFlare::FlareElement::AnamorphicY, 0.0f);
        addElementFloat("Angle", e.Angle, &LensFlare::FlareElement::Angle, 0.0f);
        AddFlareToggle(content, flare, undo, "Use Star Rotation", e.UseStarRotation,
            [i](FlareAsset& a, bool v) { if (i < a.EditElements().size()) a.EditElements()[i].UseStarRotation = v; });
        AddFlareToggle(content, flare, undo, "Rotate To Flare", e.RotateToFlare,
            [i](FlareAsset& a, bool v) { if (i < a.EditElements().size()) a.EditElements()[i].RotateToFlare = v; });
        addElementFloat("Rotation Speed", e.RotationSpeed, &LensFlare::FlareElement::RotationSpeed, 0.0f);

        AddFlareColorPickerRow(content, "Tint", flare, undo, e.Tint,
            [i](FlareAsset& asset) -> LensFlare::Color*
            {
                auto& elements = asset.EditElements();
                return i < elements.size() ? &elements[i].Tint : nullptr;
            }, openPicker, "Per-element HDR tint",
            [headerPreviewRaw, expandedPreviewRaw, headerSwatchRaw](const LensFlare::Color& color)
            {
                UpdateFlareElementHeaderColor(headerPreviewRaw, headerSwatchRaw, color);
                uint32_t argb = 0u;
                float intensity = 1.0f;
                FlareColorToPickerState(color, argb, intensity);
                if (expandedPreviewRaw)
                    expandedPreviewRaw->SetTint(0xFF000000u | (argb & 0x00FFFFFFu));
            });

        struct BoostRow { const char* Label; float LensFlare::FlareElement::* Member; };
        static constexpr BoostRow boosts[] = {
            {"Override Edge Brightness", &LensFlare::FlareElement::EdgeBrightnessBoost},
            {"Override Center Brightness", &LensFlare::FlareElement::CenterBrightnessBoost},
            {"Override Edge Scale", &LensFlare::FlareElement::EdgeScaleBoost},
            {"Override Center Scale", &LensFlare::FlareElement::CenterScaleBoost},
        };
        for (const BoostRow& boost : boosts)
        {
            const bool overridden = e.*(boost.Member) != LensFlare::kInheritGlobalBoost;
            AddFlareToggle(content, flare, undo, boost.Label, overridden,
                [i, member = boost.Member](FlareAsset& a, bool v)
                {
                    if (i < a.EditElements().size())
                        a.EditElements()[i].*member = v ? 0.0f : LensFlare::kInheritGlobalBoost;
                }, rebuild);
            if (overridden)
                addElementFloat(boost.Label, e.*(boost.Member), boost.Member, 0.0f);
        }

        parent->AddChild(std::move(elementFoldout));
    }

    AddButton(parent, "Add Flare Element", [flare, undo, atlas, rebuild]
    {
        CommitFlareChange(flare, undo, "Add Flare Element", [atlas](FlareAsset& a)
        {
            LensFlare::FlareElement element;
            if (atlas && !atlas->GetSprites().empty())
                element.SpriteName = atlas->GetSprites().front().Name;
            a.EditElements().push_back(std::move(element));
        });
        if (rebuild) rebuild();
    });
}

void BuildDefinitionEditor(UIElement* parent,
                           FlareAsset* flare,
                           const InspectorContext& ctx,
                           bool showAssetPath)
{
    if (!parent || !flare)
        return;

    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const std::function<void()> rebuild = ctx.RequestInspectorRefresh;
    if (showAssetPath)
        InspectorUI::AddTextBlock(parent, flare->GetPath().string(), "inspector-asset-path");

    InspectorUI::AddTextBlock(parent, "Flare Setup", "inspector-section-subheader");
    const GUID atlasGuid = ResolveReferenceGuid(assets, *flare, flare->GetAtlasRef());
    InspectorUI::AddAssetFieldRow(parent, "Flare Atlas", atlasGuid, {AssetType::FlareAtlas},
        &assets.GetRegistry(),
        [flare, undo = ctx.Undo, rebuild](const GUID& guid)
        {
            CommitFlareChange(flare, undo, "Change Flare Atlas", [guid](FlareAsset& a)
            {
                a.SetAtlasRef(guid.IsNull() ? std::string{} : guid.ToString());
            });
            if (rebuild) rebuild();
        }, ctx.Thumbnails, "Atlas containing the sprites used by this flare");

    FlareAtlasAsset* atlas = nullptr;
    if (!atlasGuid.IsNull())
    {
        if (auto loaded = assets.GetAsset(atlasGuid))
            atlas = dynamic_cast<FlareAtlasAsset*>(loaded.get());
        else
            Editor::RunWhenAssetLoaded(assets, atlasGuid, AssetLoadPriority::Normal, parent, nullptr, rebuild, std::string());
    }

    InspectorUI::AddTextBlock(parent, "Global Settings", "inspector-section-subheader");
    BuildGlobalSettings(parent, flare, ctx.Undo, rebuild, ctx.Window,
                        ctx.OpenColorPickerWindow);

    InspectorUI::AddTextBlock(parent, "Element Settings (" +
        std::to_string(flare->GetElements().size()) + ")", "inspector-section-subheader");
    BuildElementSettings(parent, flare, atlas, ctx.Undo, rebuild, ctx.Window,
                         ctx.OpenColorPickerWindow);
}

} // namespace

void RegisterLensFlareSourceInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<Components::LensFlareSource>(
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
                return;

            // Keep the component's per-source controls (definition reference,
            // intensity, scale, tint, sun/occlusion options) using the standard
            // reflected editor, then append shared definition authoring.
            RenderDefaultComponentInspector(ctx, ECS::GetComponentTypeId<Components::LensFlareSource>());

            const auto* source = ctx.World->GetComponent<Components::LensFlareSource>(ctx.Entity);
            if (!source || source->Flare.IsNull())
            {
                InspectorUI::AddInfoCard(ctx.Parent,
                                           "Assign a Lens Flare Definition to configure its "
                                           "flare setup.");
                return;
            }

            AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
            auto loaded = assets.GetAsset(source->Flare.Guid);
            auto* flare = loaded ? dynamic_cast<FlareAsset*>(loaded.get()) : nullptr;
            if (!flare)
            {
                InspectorUI::AddLine(ctx.Parent, "Loading flare definition...");
                Editor::RunWhenAssetLoaded(assets, source->Flare.Guid, AssetLoadPriority::High,
                                           ctx.Parent, nullptr, ctx.RequestInspectorRefresh, std::string());
                return;
            }
            BuildDefinitionEditor(ctx.Parent, flare, ctx, false);
        });

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::LensFlareDefinition,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;
            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* flare = dynamic_cast<FlareAsset*>(asset);
            if (!flare)
                return;
            BuildDefinitionEditor(ctx.Parent, flare, ctx, true);
        });
}

} // namespace GameEngine
