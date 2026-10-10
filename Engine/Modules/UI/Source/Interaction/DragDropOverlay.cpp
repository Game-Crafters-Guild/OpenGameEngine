#include "UI/Interaction/DragDropOverlay.h"

#include "UI/Controls/Label.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DragDropGhostRendererRegistry.h"
#include "UI/Interaction/Payload.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace GameEngine::UI::Interaction
{
static const char* GhostIconClassFor(DragGhostIconKind k)
{
    switch (k)
    {
        case DragGhostIconKind::AssetFile: return "dnd-ghost-icon-asset-file";
        case DragGhostIconKind::AssetFolder: return "dnd-ghost-icon-asset-folder";
        case DragGhostIconKind::Entity: return "dnd-ghost-icon-entity";
        case DragGhostIconKind::None:
        default: return "";
    }
}

static float ClampToViewport(float x, float w, float viewportW)
{
    if (viewportW <= 0.0f || w <= 0.0f)
        return x;
    if (x < 0.0f)
        return 0.0f;
    const float maxX = viewportW - w;
    if (x > maxX)
        return std::max(0.0f, maxX);
    return x;
}

void DragDropOverlay::ResetIfDetached()
{
    if (m_Ghost && m_Ghost->GetParent() == nullptr)
    {
        m_Ghost = nullptr;
        m_GhostIcon = nullptr;
        m_GhostLabel = nullptr;
        m_GhostBadge = nullptr;
        m_LastGhostText.clear();
        m_LastGhostIconKind = DragGhostIconKind::None;
        m_LastGhostThumbnailEngineName.clear();
        m_LastGhostVisible = false;
        m_LastGhostX = 0;
        m_LastGhostY = 0;
    }
    if (m_Tooltip && m_Tooltip->GetParent() == nullptr)
    {
        m_Tooltip = nullptr;
        m_TooltipLabel = nullptr;
        m_LastTooltipText.clear();
        m_LastTooltipVisible = false;
        m_LastTooltipX = 0;
        m_LastTooltipY = 0;
    }
}

void DragDropOverlay::EnsureGhost(UIElement* root)
{
    if (!root)
        return;

    if (!m_Ghost)
    {
        if (UIElement* el = root->FindById("ui-dnd-ghost"))
        {
            m_Ghost = el;
            m_GhostIcon = m_Ghost->FindById("ui-dnd-ghost-icon");
            m_GhostLabel = dynamic_cast<Label*>(m_Ghost->FindById("ui-dnd-ghost-text"));
            m_GhostBadge = dynamic_cast<Label*>(m_Ghost->FindById("ui-dnd-ghost-badge"));
        }
    }

    // If a legacy/partial ghost exists in the tree, upgrade it in-place.
    if (m_Ghost)
    {
        if (!m_GhostIcon)
        {
            auto icon = std::make_unique<UIElement>();
            icon->SetId("ui-dnd-ghost-icon");
            icon->AddClass("dnd-ghost-icon");
            m_GhostIcon = icon.get();
            m_Ghost->AddChild(std::move(icon));
        }
        if (!m_GhostLabel)
        {
            auto lbl = std::make_unique<Label>();
            lbl->SetId("ui-dnd-ghost-text");
            lbl->AddClass("dnd-ghost-text");
            m_GhostLabel = lbl.get();
            m_Ghost->AddChild(std::move(lbl));
        }
        if (!m_GhostBadge)
        {
            auto badge = std::make_unique<Label>();
            badge->SetId("ui-dnd-ghost-badge");
            badge->AddClass("dnd-ghost-badge");
            badge->AddClass("hidden");
            m_GhostBadge = badge.get();
            m_Ghost->AddChild(std::move(badge));
        }
    }

    if (!m_Ghost)
    {
        auto ghost = std::make_unique<UIElement>();
        ghost->SetId("ui-dnd-ghost");
        ghost->AddClass("drag-ghost");
        ghost->AddClass("dnd-ghost");

        auto icon = std::make_unique<UIElement>();
        icon->SetId("ui-dnd-ghost-icon");
        icon->AddClass("dnd-ghost-icon");
        m_GhostIcon = icon.get();
        ghost->AddChild(std::move(icon));

        auto lbl = std::make_unique<Label>();
        lbl->SetId("ui-dnd-ghost-text");
        lbl->AddClass("dnd-ghost-text");
        m_GhostLabel = lbl.get();
        ghost->AddChild(std::move(lbl));

        auto badge = std::make_unique<Label>();
        badge->SetId("ui-dnd-ghost-badge");
        badge->AddClass("dnd-ghost-badge");
        badge->AddClass("hidden");
        m_GhostBadge = badge.get();
        ghost->AddChild(std::move(badge));

        m_Ghost = ghost.get();
        root->AddChild(std::move(ghost));
    }
    if (m_Ghost)
    {
        m_Ghost->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9400);
        if (!m_LastGhostVisible)
            m_Ghost->Overrides().Set(Style::Display, DisplayMode::None);
    }
}

void DragDropOverlay::EnsureTooltip(UIElement* root)
{
    if (!root)
        return;

    if (!m_Tooltip)
    {
        if (UIElement* el = root->FindById("ui-dnd-tooltip"))
        {
            m_Tooltip = el;
            m_TooltipLabel = dynamic_cast<Label*>(m_Tooltip->FindById("ui-dnd-tooltip-text"));
        }
    }

    // Upgrade legacy/partial tooltip.
    if (m_Tooltip && !m_TooltipLabel)
    {
        auto text = std::make_unique<Label>();
        text->SetId("ui-dnd-tooltip-text");
        text->AddClass("dnd-tooltip-text");
        m_TooltipLabel = text.get();
        m_Tooltip->AddChild(std::move(text));
    }

    if (!m_Tooltip)
    {
        auto tip = std::make_unique<UIElement>();
        tip->SetId("ui-dnd-tooltip");
        tip->AddClass("dnd-tooltip");

        auto text = std::make_unique<Label>();
        text->SetId("ui-dnd-tooltip-text");
        text->AddClass("dnd-tooltip-text");
        m_TooltipLabel = text.get();
        tip->AddChild(std::move(text));

        m_Tooltip = tip.get();
        root->AddChild(std::move(tip));
    }
    if (m_Tooltip)
    {
        m_Tooltip->Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::PointerEvents, false)
            .Set(Style::ZIndex, 9500);
        if (!m_LastTooltipVisible)
            m_Tooltip->Overrides().Set(Style::Display, DisplayMode::None);
    }
}

void DragDropOverlay::Update(UIElement* root, const DragDropManager& dnd, float mouseX, float mouseY)
{
    ResetIfDetached();
    if (!root)
        return;

    const float viewportW = root->GetLayoutWidth();
    const float viewportH = root->GetLayoutHeight();

    // Ghost
    {
        const DragPayload& p = dnd.GetPayload();
        const bool suppressedByTarget = dnd.GetCurrentFeedback().SuppressGhost;
        const bool showGhost = !p.DisplayLabel.empty() && !suppressedByTarget;

        if (showGhost)
        {
            EnsureGhost(root);
            if (m_GhostLabel && m_LastGhostText != p.DisplayLabel)
            {
                m_LastGhostText = p.DisplayLabel;
                m_GhostLabel->SetText(m_LastGhostText);
            }
            if (m_GhostIcon)
            {
                if (m_LastGhostIconKind != p.GhostIconKind)
                {
                    const char* prev = GhostIconClassFor(m_LastGhostIconKind);
                    if (prev && *prev)
                        m_GhostIcon->RemoveClass(prev);
                    if (m_Ghost && m_LastGhostIconKind == DragGhostIconKind::Entity)
                        m_Ghost->RemoveClass("dnd-ghost-entity-row");
                    m_LastGhostIconKind = p.GhostIconKind;
                    const char* now = GhostIconClassFor(m_LastGhostIconKind);
                    if (now && *now)
                        m_GhostIcon->AddClass(now);
                    if (m_Ghost && m_LastGhostIconKind == DragGhostIconKind::Entity)
                        m_Ghost->AddClass("dnd-ghost-entity-row");
                }
                if (m_LastGhostThumbnailEngineName != p.GhostThumbnailEngineName)
                {
                    m_LastGhostThumbnailEngineName = p.GhostThumbnailEngineName;
                    if (!p.GhostThumbnailEngineName.empty())
                    {
                        m_GhostIcon->AddClass("dnd-ghost-icon-thumbnail");
                        if (m_Ghost)
                            m_Ghost->AddClass("dnd-ghost-has-thumbnail");
                        constexpr const char* kEnginePrefix = "engine:";
                        constexpr const char* kFilePrefix = "file:";
                        constexpr const char* kEditorPrefix = "editor:";
                        if (p.GhostThumbnailEngineName.size() > 7 &&
                            p.GhostThumbnailEngineName.compare(0, 7, kEnginePrefix) == 0)
                        {
                            m_GhostIcon->Styles()
                                .SetBackgroundResourceName(p.GhostThumbnailEngineName.substr(7))
                                .SetBackgroundSizeContain();
                        }
                        else if (p.GhostThumbnailEngineName.size() > 5 &&
                                 p.GhostThumbnailEngineName.compare(0, 5, kFilePrefix) == 0)
                        {
                            UI::Layout::SetBackgroundPath(*m_GhostIcon, p.GhostThumbnailEngineName.substr(5));
                        }
                        else if (p.GhostThumbnailEngineName.size() > 7 &&
                                 p.GhostThumbnailEngineName.compare(0, 7, kEditorPrefix) == 0)
                        {
                            BackgroundImageSource source{};
                            source.Kind = BackgroundImageSource::SourceKind::Path;
                            source.Value = p.GhostThumbnailEngineName.substr(7);
                            source.SourceAlias = "editor";
                            m_GhostIcon->Overrides()
                                .Set(Style::BackgroundImage, source)
                                .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
                                .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Contain})
                                .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true})
                                .Reset(Style::BackgroundTint);
                            m_GhostIcon->MarkDirty(UIElement::VisualDirty);
                        }
                        else
                        {
                            std::string path = p.GhostThumbnailEngineName;
                            for (char& c : path)
                                if (c == '\\')
                                    c = '/';
                            size_t n = 0;
                            while ((n = path.find('"', n)) != std::string::npos)
                            {
                                path.insert(n, "\\");
                                n += 2;
                            }
                            m_GhostIcon->Styles()
                                .SetBackgroundResourceName(path)
                                .SetBackgroundSizeContain()
                                .SetBackgroundPositionPercent(0.5f, 0.5f)
                                .SetBackgroundRepeat(BackgroundRepeat::NoRepeat);
                        }
                    }
                    else
                    {
                        m_GhostIcon->RemoveClass("dnd-ghost-icon-thumbnail");
                        if (m_Ghost)
                            m_Ghost->RemoveClass("dnd-ghost-has-thumbnail");
                        m_GhostIcon->Styles().ResetBackgroundImage().ResetBackgroundSize();
                    }
                }
            }

            // Optional app-provided renderer hook (per payload type).
            if (m_Ghost && m_GhostIcon && m_GhostLabel)
            {
                if (const DragGhostRendererEntry* r = FindDragGhostRenderer(p.TypeId))
                {
                    if (r->fn)
                        r->fn(p, dnd.GetCurrentMods(), *m_Ghost, *m_GhostIcon, *m_GhostLabel, m_GhostBadge, r->userData);
                }
            }

            if (m_Ghost)
            {
                const float ox = 18.0f; // to the right of cursor
                float x = mouseX + ox;
                // Align ghost vertically to the mouse tip (centered on cursor Y).
                const float ghostH = m_Ghost->GetLayoutHeight();
                const float h = (ghostH > 0.0f) ? ghostH : 30.0f; // fallback when layout not yet run
                float y = mouseY - h * 0.5f;
                const int xi = (int)std::lround(ClampToViewport(x, 1.0f, viewportW));
                const int yi = (int)std::lround(ClampToViewport(y, 1.0f, viewportH));
                if (!m_LastGhostVisible || xi != m_LastGhostX || yi != m_LastGhostY)
                {
                    m_Ghost->Overrides()
                        .Set(Style::Display, DisplayMode::Flex)
                        .Set(Style::PositionLeft, StyleLength::Px((float)xi))
                        .Set(Style::PositionTop, StyleLength::Px((float)yi));
                    m_LastGhostX = xi;
                    m_LastGhostY = yi;
                }
                m_LastGhostVisible = true;
            }
        }
        else if (m_Ghost)
        {
            if (m_LastGhostVisible)
            {
                m_Ghost->Overrides().Set(Style::Display, DisplayMode::None);
                m_LastGhostVisible = false;
            }
        }
    }

    // Tooltip (shows DropFeedback.reason near the cursor when invalid).
    {
        const auto& fb = dnd.GetCurrentFeedback();
        const bool show = (!fb.Allowed && !fb.Reason.empty());

        if (show)
        {
            EnsureTooltip(root);
            if (m_TooltipLabel && m_LastTooltipText != fb.Reason)
            {
                m_LastTooltipText = fb.Reason;
                m_TooltipLabel->SetText(m_LastTooltipText);
            }
            if (m_Tooltip)
            {
                const float oy = 18.0f;
                float x = mouseX - 40.0f;
                float y = mouseY - oy - 28.0f; // above cursor (approx; auto-sized by layout)
                if (y < 0.0f)
                    y = mouseY + oy; // fallback below cursor if near top edge

                const int xi = (int)std::lround(ClampToViewport(x, 1.0f, viewportW));
                const int yi = (int)std::lround(ClampToViewport(y, 1.0f, viewportH));
                if (!m_LastTooltipVisible || xi != m_LastTooltipX || yi != m_LastTooltipY)
                {
                    m_Tooltip->Overrides()
                        .Set(Style::Display, DisplayMode::Block)
                        .Set(Style::PositionLeft, StyleLength::Px((float)xi))
                        .Set(Style::PositionTop, StyleLength::Px((float)yi));
                    m_LastTooltipX = xi;
                    m_LastTooltipY = yi;
                }
                m_LastTooltipVisible = true;
            }
        }
        else if (m_Tooltip)
        {
            if (m_LastTooltipVisible)
            {
                m_Tooltip->Overrides().Set(Style::Display, DisplayMode::None);
                m_LastTooltipVisible = false;
            }
        }
    }
}

void DragDropOverlay::Hide()
{
    ResetIfDetached();
    if (m_Ghost)
    {
        m_Ghost->Overrides().Set(Style::Display, DisplayMode::None);
    }
    if (m_Tooltip)
    {
        m_Tooltip->Overrides().Set(Style::Display, DisplayMode::None);
    }
    m_LastGhostVisible = false;
    m_LastTooltipVisible = false;
}
} // namespace GameEngine::UI::Interaction
