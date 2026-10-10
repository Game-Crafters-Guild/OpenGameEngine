#pragma once

#include "ECS/World.h"
#include "ECS/Entity.h"            // WorldConfig
#include "ECS/AutoRegistration.h"  // AutoComponentRegistrar
#include "ECS/ComponentConcepts.h"
#include "ECS/QueryPolicy.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <vector>

namespace GameEngine {
namespace ECS {

// Minimal fluent WorldBuilder for early configuration
class WorldBuilder {
private:
    WorldConfig config{};
    JobSystem::WorkStealingThreadPool* jobSystem = nullptr;

    QueryPolicy queryPolicy{};
    size_t reserveEntitiesCount = 0;

    struct ArchetypeReserveReq {
        DenseSignature Signature;
        size_t Count = 0;
    };
    std::vector<ArchetypeReserveReq> archetypeReserves;

public:
    WorldBuilder& SetJobSystem(JobSystem::WorkStealingThreadPool* js) {
        jobSystem = js;
        return *this;
    }

    WorldBuilder& SetQueryPolicy(const QueryPolicy& p) {
        queryPolicy = p;
        return *this;
    }

    WorldBuilder& SetConfig(const WorldConfig& cfg) {
        config = cfg;
        return *this;
    }

    // Optional convenience toggles
    WorldBuilder& EnableBulkOperations(bool enable = true) {
        config.EnableBulkOperations = enable;
        return *this;
    }

    // Pre-register component types to reduce first-use work
    template<Component... Ts>
    WorldBuilder& PreRegister() {
        // We just record; actual registration happens after World constructed
        (AutoComponentRegistrar<Ts>::EnsureRegistered(), ...);
        return *this;
    }

    WorldBuilder& ReserveEntities(size_t count) {
        reserveEntitiesCount = count;
        return *this;
    }

    template<Component... Ts>
    WorldBuilder& ReserveArchetypeCapacity(size_t count) {
        ArchetypeReserveReq req{};
        (req.Signature.Add(GetComponentTypeId<Ts>()), ...);
        req.Count = count;
        archetypeReserves.push_back(std::move(req));
        return *this;
    }

    // Construct the world and apply registrations and reserves
    std::unique_ptr<World> Build() {
        auto world = std::make_unique<World>(config, jobSystem);

        // Apply Job System and QueryPolicy
        if (jobSystem) {
            world->SetJobSystem(jobSystem);
        }
        world->SetQueryPolicy(queryPolicy);

        // Reserve entity metadata
        if (reserveEntitiesCount > 0) {
            world->ReserveEntities(reserveEntitiesCount);
        }

        // Reserve specified archetypes
        for (const auto& r : archetypeReserves) {
            auto* arch = world->GetOrCreateArchetype(r.Signature);
            if (arch) arch->Reserve(r.Count);
        }

        return world;
    }
};

} // namespace ECS
} // namespace GameEngine

