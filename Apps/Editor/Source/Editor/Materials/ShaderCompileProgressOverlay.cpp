#include "Editor/Materials/ShaderCompileProgressOverlay.h"

#include "EditorContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";
// How long the final "M / M" stays up after the queue drains.
constexpr std::chrono::milliseconds kDrainedHold{400};

std::string ProgressText(uint64_t done, uint64_t total)
{
    return "Compiling shaders... " + std::to_string(done) + " / " + std::to_string(total);
}

bool SceneIsLoading(const EditorContext& context)
{
    uint64_t processed = 0;
    uint64_t total = 0;
    return context.SceneBuildProgress && context.SceneBuildProgress(processed, total) && total > 0;
}

// Done and total for the batch that started at `baseCompleted`, clamped so a baseline above the
// live counts (the material system recreated mid-batch by a device rebuild) never underflows.
std::pair<uint64_t, uint64_t> BatchCounts(uint64_t submitted, uint64_t completed, uint64_t baseCompleted)
{
    const uint64_t total = submitted >= baseCompleted ? submitted - baseCompleted : 0;
    const uint64_t done = completed >= baseCompleted ? completed - baseCompleted : 0;
    return {std::min(done, total), total};
}
} // namespace

ShaderCompileProgressOverlay::ShaderCompileProgressOverlay(const EditorContext& context)
    : m_Context(context)
{
}

void ShaderCompileProgressOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("shader-compile-progress");
    banner->AddClass(kHiddenClass);
    m_Banners.push_back(UIElement::MakeWeakRef(banner.get()));
    layer.AddChild(std::move(banner));
    m_Shown = false;
}

void ShaderCompileProgressOverlay::Update()
{
    m_Banners.erase(std::remove_if(m_Banners.begin(), m_Banners.end(),
                                   [](const UIElement::WeakRef<Label>& banner) { return banner.Get() == nullptr; }),
                    m_Banners.end());

    if (SceneIsLoading(m_Context))
    {
        // Compiles submitted meanwhile open a new batch once the scene has resolved.
        m_BatchActive = false;
        m_Draining = false;
        Hide();
        return;
    }

    uint64_t submitted = 0;
    uint64_t completed = 0;
    if (Engine::Renderer::RenderServices* services = m_Context.RenderServices)
    {
        const auto progress = services->Materials().GetPrewarmProgress();
        submitted = progress.submitted;
        completed = progress.completed;
    }

    if (submitted <= completed)
    {
        if (!m_BatchActive)
            return;
        const uint64_t total = BatchCounts(submitted, completed, m_BatchBaseCompleted).second;
        if (!m_Draining)
        {
            // The busy branch never reaches "M / M" (done == total means the queue is empty), so
            // the drain shows it, and holds it from this moment.
            m_Draining = true;
            m_DrainedAt = std::chrono::steady_clock::now();
            LogProgress(total, total);
            Show(ProgressText(total, total));
        }
        if (std::chrono::steady_clock::now() - m_DrainedAt < kDrainedHold)
            return;
        Logger::Log::Info("[ShaderWarmup] shader compile drained; overlay hidden");
        m_BatchActive = false;
        m_Draining = false;
        m_LoggedDone = ~0ull;
        m_LoggedTotal = ~0ull;
        Hide();
        return;
    }

    m_Draining = false;
    if (!m_BatchActive || m_BatchBaseCompleted > completed)
    {
        m_BatchActive = true;
        m_BatchBaseCompleted = completed;
        m_LoggedDone = ~0ull;
        m_LoggedTotal = ~0ull;
    }
    const auto [done, total] = BatchCounts(submitted, completed, m_BatchBaseCompleted);
    LogProgress(done, total);
    Show(ProgressText(done, total));
}

void ShaderCompileProgressOverlay::Show(const std::string& text)
{
    m_Shown = true;
    for (const UIElement::WeakRef<Label>& weak : m_Banners)
    {
        Label* banner = weak.Get();
        banner->SetText(text);
        banner->RemoveClass(kHiddenClass);
    }
}

void ShaderCompileProgressOverlay::Hide()
{
    if (!m_Shown)
        return;
    m_Shown = false;
    for (const UIElement::WeakRef<Label>& weak : m_Banners)
        weak.Get()->AddClass(kHiddenClass);
}

void ShaderCompileProgressOverlay::LogProgress(uint64_t done, uint64_t total)
{
    if (done == m_LoggedDone && total == m_LoggedTotal)
        return;
    m_LoggedDone = done;
    m_LoggedTotal = total;
    Logger::Log::Info("[ShaderWarmup] compiling shaders {} / {}", done, total);
}

} // namespace GameEngine::Editor
