// ReflectionProbeSystem's no-probe-selected warning. Rejecting one probe is a routine
// outcome in a multi-probe scene (each probe covers its own room), so the diagnostic
// hangs off the selection OUTCOME: warn once when a frame ends with every enabled probe
// rejecting the active camera, latched until a probe is accepted again — never per probe,
// where any accepted probe clearing the latch spams a two-room scene every frame.
// None of these paths touch the device, so a bare RenderServices instance suffices
// (same seam as RenderServicesDrsModeTests).

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::Engine::Renderer::ReflectionProbeSystem;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{

// Captures every ReflectionProbe warning the system emits, by message substring
// ("ReflectionProbe" appears in the warning text). Counts and last-message are
// mutex-guarded: the logger drains on its own thread.
class ProbeWarningCapture
{
  public:
    ProbeWarningCapture()
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        m_CallbackId = m_Sink->RegisterCallback(
            [this](const Logger::LogMessage& msg)
            {
                if (msg.Level != Logger::LogLevel::Warning)
                    return;
                if (msg.Message.find("ReflectionProbe") == Logger::String::npos)
                    return;
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Messages.emplace_back(msg.Message.begin(), msg.Message.end());
            });
        Logger::Log::AddSink(std::move(sink));
    }

    ~ProbeWarningCapture() { m_Sink->UnregisterCallback(m_CallbackId); }

    int Count()
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return static_cast<int>(m_Messages.size());
    }

    std::string Last()
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Messages.empty() ? std::string() : m_Messages.back();
    }

  private:
    Logger::CallbackSink* m_Sink = nullptr;
    Logger::uint64 m_CallbackId = 0;
    std::mutex m_Mutex;
    std::vector<std::string> m_Messages;
};

Components::WorldTransform MakeTransform(float x, float y, float z, float scale)
{
    Components::WorldTransform xf{};
    xf.matrix[0] = scale;
    xf.matrix[5] = scale;
    xf.matrix[10] = scale;
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

// A probe volume centered at x, half-extent = scale / 2 on every axis.
ECS::EntityHandle AddProbe(ECS::World& world, float x, float scale, bool boxProjection)
{
    ECS::EntityHandle e = world.CreateEntity();
    Components::ReflectionProbe probe{};
    probe.BoxProjection = boxProjection;
    probe.BlendDistance = 0.0f; // hard edge: accept/reject only, no blend-zone weighting
    world.AddComponentImmediate(e, probe);
    world.AddComponentImmediate(e, MakeTransform(x, 0.0f, 0.0f, scale));
    return e;
}

ECS::EntityHandle AddCamera(ECS::World& world, float x)
{
    ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, Components::Camera{});
    world.AddComponentImmediate(e, MakeTransform(x, 0.0f, 0.0f, 1.0f));
    return e;
}

void MoveCamera(ECS::World& world, ECS::EntityHandle camera, float x)
{
    auto* xf = world.GetComponentForWrite<Components::WorldTransform>(camera);
    ASSERT_NE(xf, nullptr);
    xf->matrix[12] = x;
}

void Drive(ReflectionProbeSystem& system, ECS::World& world, int frames)
{
    for (int i = 0; i < frames; ++i)
        system.Update(world, 0.016f);
}

} // namespace

// The review's blocker scene: two rooms, one box probe each, camera inside room 1.
// Room 2's probe rejects the camera every frame — a routine outcome, not a defect —
// and room 1's probe is selected. Nothing may be logged, on any frame.
TEST(ReflectionProbeSystem, MultiProbeSceneWithAnAcceptedProbe_NeverWarns)
{
    ProbeWarningCapture capture;
    ECS::World world;
    AddProbe(world, 0.0f, 4.0f, true);  // room 1: half-extents 2, camera inside
    AddProbe(world, 10.0f, 4.0f, true); // room 2: rejects a camera at the origin
    AddCamera(world, 0.0f);

    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    Drive(system, world, 10);

    EXPECT_EQ(capture.Count(), 0)
        << "a rejected probe in a scene where another probe is selected is not a diagnostic";
}

// All probes rejected: exactly one warning for the whole episode, however long it lasts.
// A probe accepting again rearms the latch, so the next lost-reflections episode warns
// exactly once more.
TEST(ReflectionProbeSystem, AllProbesRejected_WarnsOncePerEpisodeAndRearmsOnAccept)
{
    ProbeWarningCapture capture;
    ECS::World world;
    AddProbe(world, 0.0f, 4.0f, true);
    AddProbe(world, 10.0f, 4.0f, true);
    ECS::EntityHandle camera = AddCamera(world, 5.0f); // outside both (gap between rooms)

    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    Drive(system, world, 10);
    EXPECT_EQ(capture.Count(), 1) << "a persistent lost-reflections episode logs once, not per frame";
    const std::string first = capture.Last();
    EXPECT_NE(first.find("No ReflectionProbe contributes"), std::string::npos) << first;
    EXPECT_NE(first.find("box probe"), std::string::npos)
        << "the warning names the nearest rejected probe: " << first;

    MoveCamera(world, camera, 0.0f); // inside room 1 — accepted, latch rearms
    Drive(system, world, 5);
    EXPECT_EQ(capture.Count(), 1);

    MoveCamera(world, camera, 5.0f); // outside both again — a new episode
    Drive(system, world, 10);
    EXPECT_EQ(capture.Count(), 2) << "the accept must rearm the warning for the next episode";
}

// A world with probes but no camera, or with no probes at all, records no rejection and
// must stay silent: neither is a misconfiguration the warning exists for.
TEST(ReflectionProbeSystem, NoRejectionRecorded_StaysSilent)
{
    ProbeWarningCapture capture;
    {
        ECS::World world; // camera, no probes
        AddCamera(world, 0.0f);
        RenderServices rs;
        ReflectionProbeSystem system(&rs);
        Drive(system, world, 5);
    }
    {
        ECS::World world; // probe, no camera (probe is accepted unconditionally)
        AddProbe(world, 0.0f, 4.0f, true);
        RenderServices rs;
        ReflectionProbeSystem system(&rs);
        Drive(system, world, 5);
    }
    EXPECT_EQ(capture.Count(), 0);
}

// The ellipsoid (BoxProjection = false) rejection path feeds the same selection-outcome
// warning — before the restructure it was entirely silent.
TEST(ReflectionProbeSystem, EllipsoidRejection_FeedsTheSelectionOutcomeWarning)
{
    ProbeWarningCapture capture;
    ECS::World world;
    AddProbe(world, 0.0f, 4.0f, false); // half-extents 2, camera at 5 is radially outside
    AddCamera(world, 5.0f);

    RenderServices rs;
    ReflectionProbeSystem system(&rs);
    Drive(system, world, 10);

    EXPECT_EQ(capture.Count(), 1);
    const std::string message = capture.Last();
    EXPECT_NE(message.find("ellipsoid probe"), std::string::npos) << message;
    EXPECT_NE(message.find("radially"), std::string::npos) << message;
}
