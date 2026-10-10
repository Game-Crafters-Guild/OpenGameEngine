#include "PhysicsECS/Systems/PhysicsEventsSystem.h"

#include "PhysicsECS/PhysicsWorldService.h"

#include "PhysicsECS/Components/PhysicsEventBuffers.h"

#include "ECS/Query.h"
#include "Logger/Logger.h"

namespace GameEngine::PhysicsECS
{
namespace
{
static GameEngine::Components::PhysicsContactEventsBuffer* g_contactBuf = nullptr;
static GameEngine::Components::PhysicsTriggerEventsBuffer* g_triggerBuf = nullptr;

static Physics::PhysicsWorld::ListenerId g_bufferContactListenerId = 0;
static Physics::PhysicsWorld::ListenerId g_bufferTriggerListenerId = 0;

static ECS::EntityHandle BufferEntityFromUserData(uint64 ud)
{
    // Convention: if userData is an ECS entity id, it fits in uint32.
    return ECS::EntityHandle(static_cast<uint32>(ud));
}
} // namespace

void PhysicsEventsSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* pw = PhysicsWorldService::TryGet();
    if (!pw)
        return;

    // Opt-in: only route into buffers if the world already has them.
    // (We avoid buffering all collisions by default.)
    g_contactBuf = nullptr;
    g_triggerBuf = nullptr;

    // Find singleton buffers entity (do not create).
    ECS::EntityHandle buffersEntity{};
    GameEngine::Components::PhysicsContactEventsBuffer* contact = nullptr;
    GameEngine::Components::PhysicsTriggerEventsBuffer* trigger = nullptr;

    world.Query<ECS::Write<GameEngine::Components::PhysicsContactEventsBuffer>,
                ECS::Write<GameEngine::Components::PhysicsTriggerEventsBuffer>>()
        .Each([&](ECS::EntityHandle e,
                  GameEngine::Components::PhysicsContactEventsBuffer& c,
                  GameEngine::Components::PhysicsTriggerEventsBuffer& t)
              {
                  if (!buffersEntity.IsValid())
                  {
                      buffersEntity = e;
                      contact = &c;
                      trigger = &t;
                  }
              });

    const bool wantBuffers = (buffersEntity.IsValid() && contact && trigger);

    if (wantBuffers)
    {
        // Clear for this frame.
        contact->Clear();
        trigger->Clear();

        // Point global routing to this world's buffers (PhysicsWorldService is currently a singleton).
        g_contactBuf = contact;
        g_triggerBuf = trigger;
    }

    // Install/remove listeners based on whether buffers exist.
    if (wantBuffers && g_bufferContactListenerId == 0)
    {
        g_bufferContactListenerId = PhysicsWorldService::Get().AddContactListener([](const Physics::ContactEvent& e)
                                                    {
                                                        if (!g_contactBuf)
                                                            return;

                                                        GameEngine::Components::PhysicsContactEvent out{};
                                                        out.userDataA = e.userDataA;
                                                        out.userDataB = e.userDataB;
                                                        out.entityA = BufferEntityFromUserData(e.userDataA);
                                                        out.entityB = BufferEntityFromUserData(e.userDataB);
                                                        out.state = e.state;
                                                        out.point = e.point;
                                                        out.normal = e.normal;
                                                        out.impulse = e.impulse;
                                                        if (!g_contactBuf->Push(out))
                                                        {
                                                            static bool warned = false;
                                                            if (!warned)
                                                            {
                                                                warned = true;
                                                                Logger::Log::Warning("[PhysicsECS] Contact event buffer overflow; dropping events");
                                                            }
                                                        }
                                                    });
    }
    else if (!wantBuffers && g_bufferContactListenerId != 0)
    {
        PhysicsWorldService::Get().RemoveContactListener(g_bufferContactListenerId);
        g_bufferContactListenerId = 0;
    }

    if (wantBuffers && g_bufferTriggerListenerId == 0)
    {
        g_bufferTriggerListenerId = PhysicsWorldService::Get().AddTriggerListener([](const Physics::TriggerEvent& e)
                                                    {
                                                        if (!g_triggerBuf)
                                                            return;

                                                        GameEngine::Components::PhysicsTriggerEvent out{};
                                                        out.triggerUserData = e.triggerUserData;
                                                        out.otherUserData = e.otherUserData;
                                                        out.trigger = BufferEntityFromUserData(e.triggerUserData);
                                                        out.other = BufferEntityFromUserData(e.otherUserData);
                                                        out.isEntering = e.isEntering;
                                                        if (!g_triggerBuf->Push(out))
                                                        {
                                                            static bool warned = false;
                                                            if (!warned)
                                                            {
                                                                warned = true;
                                                                Logger::Log::Warning("[PhysicsECS] Trigger event buffer overflow; dropping events");
                                                            }
                                                        }
                                                    });
    }
    else if (!wantBuffers && g_bufferTriggerListenerId != 0)
    {
        PhysicsWorldService::Get().RemoveTriggerListener(g_bufferTriggerListenerId);
        g_bufferTriggerListenerId = 0;
    }
}
} // namespace GameEngine::PhysicsECS

