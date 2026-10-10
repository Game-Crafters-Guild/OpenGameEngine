#include "Engine/Rendering/DDGICameraIdleGate.h"

#include "Engine/Rendering/DDGIEligibleView.h"
#include "Engine/Rendering/ViewRegistry.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
namespace
{
// Quiet window before a rested field resumes solving. 200 ms is the reference
// implementation's GI_IDLE_MS verbatim (js/gi_probes.js).
constexpr float kRestDebounceMs = 200.0f;

// Squared world-space distance a camera may drift without counting as motion —
// the reference's own `cam.position.distanceToSquared(prev) > 1e-7` threshold.
constexpr float kPositionEpsilonSq = 1.0e-7f;

// Squared Frobenius distance between two view bases, the matrix-space
// equivalent of the reference's `|quat.dot(prev)| < 0.99999995` (which is a
// rotation of ~6.3e-4 rad; a basis separated by angle t has squared Frobenius
// distance ~2*t^2, so ~8e-7 — rounded to 1e-6 here).
constexpr float kBasisEpsilonSq = 1.0e-6f;

}  // namespace

bool DDGICameraIdleGate::UpdateAndIsMoving(const ViewRegistry& views, uint64 worldId, float deltaTimeSeconds)
{
    ++m_Serial;
    bool moved = false;

    for (const Rendering::ViewDesc& view : views.GetViews())
    {
        if (!IsDDGIEligibleView(view, worldId))
            continue;
        const Rendering::CameraData* cam = views.FindCameraData(view.cameraId);
        if (cam == nullptr)
            continue;

        ViewPose pose{};
        pose.ViewId = view.id;
        pose.PositionWS = Mathematics::Vector3(cam->cameraPos[0], cam->cameraPos[1], cam->cameraPos[2]);
        // Column-major view matrix: columns 0..2 carry the rotation basis,
        // column 3 the translation (which cameraPos above already covers).
        for (int col = 0; col < 3; ++col)
            pose.Basis[col] = glm::vec3(cam->view[col * 4 + 0], cam->view[col * 4 + 1],
                                        cam->view[col * 4 + 2]);
        pose.SeenSerial = m_Serial;

        auto it = std::find_if(m_Poses.begin(), m_Poses.end(),
                               [&](const ViewPose& p) { return p.ViewId == pose.ViewId; });
        if (it == m_Poses.end())
        {
            // First sighting is a baseline, never motion — otherwise opening a
            // viewport would arm the gate for no reason.
            m_Poses.push_back(pose);
            continue;
        }

        const Mathematics::Vector3 posDelta = pose.PositionWS - it->PositionWS;
        const float posDeltaSq = Mathematics::Vector3::Dot(posDelta, posDelta);
        float basisDeltaSq = 0.0f;
        for (int col = 0; col < 3; ++col)
        {
            const glm::vec3 d = pose.Basis[col] - it->Basis[col];
            basisDeltaSq += glm::dot(d, d);
        }
        if (posDeltaSq > kPositionEpsilonSq || basisDeltaSq > kBasisEpsilonSq)
            moved = true;
        *it = pose;
    }

    const uint64 serial = m_Serial;
    m_Poses.erase(std::remove_if(m_Poses.begin(), m_Poses.end(),
                                 [serial](const ViewPose& p) { return p.SeenSerial != serial; }),
                  m_Poses.end());

    m_RestMs = moved ? 0.0f : m_RestMs + std::max(deltaTimeSeconds, 0.0f) * 1000.0f;
    return m_RestMs < kRestDebounceMs;
}

}  // namespace GameEngine::Engine::Renderer
