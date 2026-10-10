#include "Assets/DownloadPillOverlay.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenService.h"
#include "ECS/Entity.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{
int PercentOf(float fraction)
{
    return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}
} // namespace

DownloadPillOverlay::DownloadPillOverlay(const PolyhavenDownloadManager& downloads)
    : m_Downloads(downloads)
{
}

void DownloadPillOverlay::SetHost(UIElement* host, const UIElement* stackBanner)
{
    m_Host = host;
    m_StackBanner = stackBanner;
}

void DownloadPillOverlay::Build(const std::string& id, Widgets& out)
{
    auto pill = std::make_unique<UIElement>();
    pill->SetId(id);
    pill->AddClass("download-pill");
    pill->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PointerEvents, false)
        .Set(Style::Opacity, 0.0f);

    auto image = std::make_unique<UIElement>();
    image->SetId(id + "Image");
    image->AddClass("download-pill-image");
    out.Image = image.get();
    pill->AddChild(std::move(image));

    auto text = std::make_unique<Label>();
    text->AddClass("download-pill-label");
    text->SetText("");
    out.Text = text.get();
    pill->AddChild(std::move(text));

    auto track = std::make_unique<UIElement>();
    track->AddClass("download-pill-track");
    auto fill = std::make_unique<UIElement>();
    fill->AddClass("download-pill-fill");
    out.Fill = fill.get();
    track->AddChild(std::move(fill));
    pill->AddChild(std::move(track));

    out.Pill = pill.get();
    out.ImageSlug.clear();
    out.LastPercent = -1000;
    out.LastRow = -1;
    out.LastTopY = -1.0f;
    out.LastLeftX = -1.0f;
    m_Host->AddChild(std::move(pill));
}

void DownloadPillOverlay::ApplyState(Widgets& w, const std::string& name, float hostX, float hostY,
                                     bool hasFraction, float fraction)
{
    if (!w.Pill)
        return;

    std::string text = name.empty() ? std::string("Downloading") : ("Downloading " + name);
    if (hasFraction)
        text += "  " + std::to_string(PercentOf(fraction)) + "%";
    if (w.Text)
        w.Text->SetText(text);

    if (w.Fill)
    {
        // Indeterminate until the file count is known: a short teaser fill.
        const float f = hasFraction ? std::clamp(fraction, 0.0f, 1.0f) : 0.12f;
        w.Fill->Overrides().Set(Style::Width, StyleLength::Percent(f * 100.0f));
    }

    // hostX/Y are the pill's absolute left/top within the host.
    w.Pill->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(hostX))
        .Set(Style::PositionTop, StyleLength::Px(hostY))
        .Set(Style::Opacity, 1.0f);
}

void DownloadPillOverlay::ApplyImage(Widgets& w, const std::string& slug)
{
    if (!w.Image || slug == w.ImageSlug)
        return;
    w.ImageSlug = slug;

    std::error_code ec;
    const std::filesystem::path thumb = PolyhavenService::GetCacheDir() / (slug + ".png");
    if (!slug.empty() && std::filesystem::exists(thumb, ec))
    {
        w.Image->RemoveClass("download-pill-image-empty");
        UI::Layout::SetBackgroundPath(*w.Image, thumb.string());
    }
    else
    {
        // No cached thumbnail — collapse the image so the pill is text plus bar only.
        w.Image->AddClass("download-pill-image-empty");
    }
}

void DownloadPillOverlay::Hide(Widgets& w)
{
    if (w.Pill)
        w.Pill->Overrides().Set(Style::Opacity, 0.0f);
    // Force the preview image to re-apply when the pill next shows a slug.
    w.ImageSlug.clear();
    w.LastPercent = -1000;
    w.LastRow = -1;
    w.LastTopY = -1.0f;
    w.LastLeftX = -1.0f;
}

void DownloadPillOverlay::ShowDragPill(const std::string& slug, const std::string& name,
                                       float hostX, float hostY)
{
    m_DragPillShown = true;
    if (!m_DragPill.Pill)
    {
        if (!m_Host)
            return;
        Build("SceneViewDownloadPill", m_DragPill);
    }
    ApplyImage(m_DragPill, slug);

    float fraction = 0.0f;
    const bool hasFraction = m_Downloads.GetDownloadProgress(slug, fraction);
    // The drag pill trails the cursor by a small offset so it never sits under it.
    constexpr float kCursorOffsetPx = 16.0f;
    ApplyState(m_DragPill, name, hostX + kCursorOffsetPx, hostY + kCursorOffsetPx, hasFraction, fraction);
}

void DownloadPillOverlay::HideDragPill()
{
    m_DragPillShown = false;
    Hide(m_DragPill);
}

void DownloadPillOverlay::SetDisplayName(const std::string& slug, const std::string& name)
{
    m_DisplayNames[slug] = name;
}

void DownloadPillOverlay::Update(const ECS::World* world)
{
    // The drag owns the overlay while its pill is shown.
    if (m_DragPillShown)
        return;

    // Pills visualize every in-flight model-placeholder download, whether it was
    // dropped on the scene view or on the hierarchy panel: one pill per download,
    // stacked from the top of the host in download-start order.
    std::vector<std::pair<std::string, ECS::EntityHandle>> active;
    if (world)
        m_Downloads.GetActivePlaceholderDownloads(world, active);

    // Retire pills whose download is no longer active (its model landed, or the
    // world went away).
    for (auto it = m_PostDropPills.begin(); it != m_PostDropPills.end();)
    {
        const bool stillActive = std::any_of(active.begin(), active.end(),
                                             [&](const auto& e) { return e.first == it->first; });
        if (stillActive)
        {
            ++it;
            continue;
        }
        if (UIElement* pill = it->second.Pill)
            if (UIElement* parent = pill->GetParent())
                parent->RemoveChild(pill);
        m_DisplayNames.erase(it->first);
        it = m_PostDropPills.erase(it);
    }

    if (active.empty() || !m_Host)
        return;

    // Stack anchor: horizontally centred on the host, starting below the
    // "Compiling shaders…" banner, which shares the centre (PositionTop 14px).
    // The banner's laid-out height is read from the element itself so the
    // offset tracks its CSS and needs no visibility flag; when the banner is
    // hidden the stack simply starts a banner-height lower. Each pill is
    // centred on its own laid-out width; before its first layout that width
    // reads 0, so a typical width stands in for that one frame and the re-flow
    // below corrects it as soon as the real width is known.
    constexpr float kPillStrideY = 40.0f;  // ~34px pill (24px image + 5px padding x2) plus a 6px gap
    constexpr float kPillFallbackWidth = 232.0f;
    constexpr float kCompileBannerTop = 14.0f;
    constexpr float kCompileBannerGap = 22.0f;
    constexpr float kStackTopNoBanner = 16.0f;
    const float hostWidth = m_Host->GetLayoutWidth();
    const float stackTopY = m_StackBanner
                                ? kCompileBannerTop + m_StackBanner->GetLayoutHeight() + kCompileBannerGap
                                : kStackTopNoBanner;

    int row = 0;
    for (const auto& entry : active)
    {
        const std::string& slug = entry.first;
        auto [it, inserted] = m_PostDropPills.try_emplace(slug);
        Widgets& w = it->second;
        if (inserted)
            Build("SceneViewDownloadPill_" + slug, w);

        const auto nameIt = m_DisplayNames.find(slug);
        const std::string& name = nameIt != m_DisplayNames.end() ? nameIt->second : slug;

        ApplyImage(w, slug);
        float fraction = 0.0f;
        const bool hasFraction = m_Downloads.GetDownloadProgress(slug, fraction);
        const int percent = hasFraction ? PercentOf(fraction) : -1;
        const float pillWidth = w.Pill ? w.Pill->GetLayoutWidth() : 0.0f;
        const float leftX = (hostWidth - (pillWidth > 0.0f ? pillWidth : kPillFallbackWidth)) * 0.5f;
        // Re-apply only when the percent, the stack row, the stack origin, or
        // the centred edge changes, so an idle stack costs nothing per frame
        // while a finished download above (or the banner appearing, or a label
        // growing a digit) re-flows the pills.
        if (inserted || percent != w.LastPercent || row != w.LastRow || stackTopY != w.LastTopY ||
            leftX != w.LastLeftX)
        {
            ApplyState(w, name, leftX, stackTopY + static_cast<float>(row) * kPillStrideY,
                       hasFraction, fraction);
            w.LastPercent = percent;
            w.LastRow = row;
            w.LastTopY = stackTopY;
            w.LastLeftX = leftX;
        }
        ++row;
    }
}

} // namespace GameEngine
