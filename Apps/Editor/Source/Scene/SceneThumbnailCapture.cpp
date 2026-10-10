#include "Scene/SceneThumbnailCapture.h"

#include "Editor/EditorPaths.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Logger/Logger.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

void SceneThumbnailCapture::RequestCapture(const std::filesystem::path& scenePath)
{
    m_Pending = true;
    m_PendingPath = scenePath;
}

void SceneThumbnailCapture::BeginReadbackRG(Rendering::RenderGraph::RGFrame& frame,
                                            uint32_t sceneViewId,
                                            Engine::Renderer::RenderServices* rs)
{
    if (!WantsCapture() || !rs || !rs->GetDevice() || sceneViewId == 0)
        return;
    const auto out = rs->GetPipelineOutputRG(frame, sceneViewId);
    if (!out.IsValid())
        return;
    m_Ticket = Rendering::RequestTextureReadbackRG(rs->GetDevice(), frame, out.Out,
                                                   "Editor.SceneThumbnail.Readback");
    if (m_Ticket)
    {
        // The pipeline's own stamp for what it wrote into FinalColor this frame,
        // carried to the PNG write rather than re-derived from the readback format.
        m_InFlightSpace = rs->GetPipelineOutputSpaceRG();
        m_InFlightPath = m_PendingPath;
        m_PendingPath.clear();
        m_Pending = false;
    }
}

void SceneThumbnailCapture::Poll(const ReadyCallback& onReady)
{
    // Stage 1: poll the GPU readback; when it resolves, kick off the background
    // PNG write.
    if (m_Ticket)
    {
        // A cancelled ticket (declaring frame abandoned) never resolves —
        // re-arm the pending request instead of wedging the capture.
        if (m_Ticket->IsConsumed())
        {
            m_Ticket = nullptr;
            m_InFlightSpace.reset();
            if (m_PendingPath.empty() && !m_InFlightPath.empty())
                m_PendingPath = std::move(m_InFlightPath);
            else
                m_InFlightPath.clear();
            m_Pending = !m_PendingPath.empty();
        }
        else
        {
            Rendering::ViewReadbackResult result;
            if (m_Ticket->TryGet(result))
            {
                m_Ticket = nullptr;
                if (m_InFlightSpace)
                    CommitResult(result, *m_InFlightSpace);
                else
                    Logger::Log::Warning(
                        "SceneThumbnailCapture: readback resolved without a source-space stamp — "
                        "thumbnail skipped");
                m_InFlightSpace.reset();
                m_InFlightPath.clear();
                m_Pending = !m_PendingPath.empty();
            }
        }
    }

    // Stage 2: poll the background PNG write's ready flag.
    std::string cachePath;
    std::string scenePath;
    {
        std::lock_guard<std::mutex> lock(m_Ready->Mutex);
        if (m_Ready->Flag)
        {
            cachePath = std::move(m_Ready->CachePath);
            scenePath = std::move(m_Ready->ScenePath);
            m_Ready->Flag = false;
        }
    }
    if (!cachePath.empty() && onReady)
        onReady(cachePath, scenePath);
}

void SceneThumbnailCapture::CommitResult(const Rendering::ViewReadbackResult& result,
                                         UI::UITextureSpace sourceSpace)
{
    const std::filesystem::path capturePath = !m_InFlightPath.empty() ? m_InFlightPath : m_PendingPath;
    if (capturePath.empty() || result.pixels.empty())
        return;

    const EditorProjectPaths projectPaths = GetCurrentEditorProjectPaths();
    if (projectPaths.thumbnailsRoot.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(projectPaths.thumbnailsRoot, ec);
    if (ec)
        return;

    // Name the file by hashing the scene path — same convention as VideoThumbnailHandler.
    const std::size_t h = std::hash<std::string>{}(capturePath.string());
    const std::filesystem::path cachePath = projectPaths.thumbnailsRoot /
        ("scenethumb_" + std::to_string(h) + ".png");

    const std::string cachePathStr = cachePath.generic_string();
    // Fixed-name project-card thumbnail: the picker reads this with a single
    // exists() (no directory walk), so it shows on web and always reflects the
    // latest load/save capture.
    const std::string cardPathStr = (projectPaths.thumbnailsRoot / "card.png").generic_string();
    const std::string scenePathStr = capturePath.string();
    const Rendering::TextureFormat fmt = result.format;
    const uint32_t w = result.width;
    const uint32_t h2 = result.height;
    std::vector<uint8_t> pixels = result.pixels;

    EngineCore::GetInstance().GetJobSystem().EnqueueWork([ready = m_Ready, pixels = std::move(pixels), cachePathStr, cardPathStr, scenePathStr, fmt, w, h2,
                 sourceSpace]() mutable
    {
        // Normalize the readback to sRGB 8-bit RGBA via the shared converter, under
        // the space the pipeline stamped when the copy was declared.
        Rendering::ViewReadbackResult rb;
        rb.width = w;
        rb.height = h2;
        rb.format = fmt;
        rb.pixels = std::move(pixels);
        std::vector<uint8_t> rgba8 = Rendering::ReadbackToRgba8Srgb(rb, sourceSpace);
        if (rgba8.empty())
        {
            Logger::Log::Warning("SceneThumbnailCapture: unhandled pixel format {}",
                                 static_cast<int>(fmt));
            return;
        }
        const uint8_t* pngSrc = rgba8.data();

        if (!stbi_write_png(cachePathStr.c_str(),
                            static_cast<int>(w),
                            static_cast<int>(h2),
                            4,
                            pngSrc,
                            static_cast<int>(w * 4)))
        {
            Logger::Log::Warning("SceneThumbnailCapture: failed to write '{}'", cachePathStr);
            return;
        }

        Logger::Log::Info("Editor: wrote scene thumbnail '{}'", cachePathStr);

        // Also update the fixed-name card thumbnail the picker reads without a walk.
        stbi_write_png(cardPathStr.c_str(), static_cast<int>(w), static_cast<int>(h2), 4,
                       pngSrc, static_cast<int>(w * 4));

        std::lock_guard<std::mutex> lock(ready->Mutex);
        ready->CachePath = cachePathStr;
        ready->ScenePath = scenePathStr;
        ready->Flag = true;
    }, JobSystem::JobPriority::Background);
}

} // namespace GameEngine::Editor
