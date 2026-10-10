#include "Engine/Rendering/DDGIEligibleView.h"

#include "Engine/Rendering/ViewRegistry.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
namespace
{
// Same movement threshold the idle gate uses: squared world-space distance a
// camera may drift without counting as being driven.
constexpr float kFollowMotionEpsilonSq = 1.0e-7f;
}  // namespace

bool IsDDGIEligibleView(const Rendering::ViewDesc& view, uint64 worldId)
{
    if (view.cameraId == 0 || view.ActiveRenderLayerMask() == 0u)
        return false;
    if (view.purpose != Rendering::ViewPurpose::Game && view.purpose != Rendering::ViewPurpose::EditorScene)
        return false;
    return view.worldId == 0 || worldId == 0 || view.worldId == worldId;
}

bool DDGIFollowCameraTracker::Update(const ViewRegistry& views, uint64 worldId,
                                     Mathematics::Vector3& outPositionWS)
{
    ++m_Serial;

    bool found = false;
    bool foundIsGame = false;
    Mathematics::Vector3 fallback{};
    bool followedAlive = false;
    Mathematics::Vector3 followedPos{};

    for (const Rendering::ViewDesc& view : views.GetViews())
    {
        if (!IsDDGIEligibleView(view, worldId))
            continue;
        const Rendering::CameraData* cam = views.FindCameraData(view.cameraId);
        if (cam == nullptr)
            continue;

        const Mathematics::Vector3 pos(cam->cameraPos[0], cam->cameraPos[1], cam->cameraPos[2]);
        const bool isGame = view.purpose == Rendering::ViewPurpose::Game;
        if (!found || (isGame && !foundIsGame))
        {
            fallback = pos;
            found = true;
            foundIsGame = isGame;
        }

        auto it = std::find_if(m_Samples.begin(), m_Samples.end(),
                               [&](const ViewSample& s) { return s.ViewId == view.id; });
        if (it == m_Samples.end())
        {
            // First sighting is a baseline, never motion — opening a viewport
            // must not steal the follow target.
            m_Samples.push_back({view.id, pos, m_Serial});
        }
        else
        {
            const Mathematics::Vector3 delta = pos - it->PositionWS;
            if (Mathematics::Vector3::Dot(delta, delta) > kFollowMotionEpsilonSq)
                m_FollowedViewId = view.id;
            it->PositionWS = pos;
            it->SeenSerial = m_Serial;
        }

        if (view.id == m_FollowedViewId)
        {
            followedAlive = true;
            followedPos = pos;
        }
    }

    // Views that stopped being sampled are forgotten; if the followed view is
    // among them, the next movement of any eligible camera elects a new one.
    m_Samples.erase(std::remove_if(m_Samples.begin(), m_Samples.end(),
                                   [&](const ViewSample& s) { return s.SeenSerial != m_Serial; }),
                    m_Samples.end());
    if (!followedAlive)
        m_FollowedViewId = 0;

    if (followedAlive)
    {
        outPositionWS = followedPos;
        return true;
    }
    if (found)
    {
        outPositionWS = fallback;
        return true;
    }
    return false;
}

}  // namespace GameEngine::Engine::Renderer
