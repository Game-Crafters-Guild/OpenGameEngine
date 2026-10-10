#pragma once

#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "Logger/Logger.h"

#include <array>
#include <type_traits>

// ECS RequiredComponents support.
//
// This provides a lightweight, opt-in way for component types to declare
// other components that should be present on an entity when the component is added.
// The World API auto-adds these components with default values.

namespace GameEngine::ECS
{
namespace Detail
{
template<typename... Ts>
struct TypeList
{
};

template<typename T>
struct TypeTag
{
    using type = T;
};

// Specialize this trait per component type to declare requirements.
// Example:
// template<> struct RequiredComponents<MyComponent> { using type = TypeList<Transform, Parent>; };
template<typename T>
struct RequiredComponents
{
    using type = TypeList<>;
};

// Recursion guard to avoid infinite cycles.
struct RequiredAddGuard
{
    static constexpr size_t kMaxDepth = 16;
    static inline thread_local size_t s_Depth = 0;
    static inline thread_local std::array<ComponentTypeId, kMaxDepth> s_Stack = {};

    ComponentTypeId id = 0;
    bool active = false;

    explicit RequiredAddGuard(ComponentTypeId typeId)
        : id(typeId)
    {
        // Already in stack => cycle
        for (size_t i = 0; i < s_Depth; ++i)
        {
            if (s_Stack[i] == id)
                return;
        }
        if (s_Depth >= kMaxDepth)
            return;

        s_Stack[s_Depth++] = id;
        active = true;
    }

    ~RequiredAddGuard()
    {
        if (!active)
            return;
        // pop (LIFO)
        if (s_Depth > 0 && s_Stack[s_Depth - 1] == id)
        {
            --s_Depth;
        }
        else
        {
            // Best-effort: scan and remove.
            for (size_t i = 0; i < s_Depth; ++i)
            {
                if (s_Stack[i] == id)
                {
                    for (size_t j = i + 1; j < s_Depth; ++j)
                        s_Stack[j - 1] = s_Stack[j];
                    --s_Depth;
                    break;
                }
            }
        }
    }
};

template<typename T, typename... Seen>
void EnsureRequiredComponents(class World& world, EntityHandle entity);

template<typename Req, typename... Seen>
void EnsureOne(class World& world, EntityHandle entity)
{
    // Guard compile-time cycles.
    if constexpr ((std::is_same_v<Req, Seen> || ...))
    {
        return;
    }

    // Guard runtime cycles too.
    RequiredAddGuard guard(GetComponentTypeId<Req>());
    if (!guard.active)
        return;

    if (!world.template HasComponent<Req>(entity))
    {
        Logger::Log::Debug("[ECS] Auto-adding required component {} to entity {}", typeid(Req).name(), entity.id);
        world.template AddComponentImmediate<Req>(entity, Req{});
    }

    EnsureRequiredComponents<Req, Seen..., Req>(world, entity);
}

template<typename... Seen, typename... Reqs>
void EnsureList(TypeList<Reqs...>, class World& world, EntityHandle entity)
{
    (EnsureOne<Reqs, Seen...>(world, entity), ...);
}

template<typename T, typename... Seen>
void EnsureRequiredComponents(class World& world, EntityHandle entity)
{
    using List = typename RequiredComponents<T>::type;
    EnsureList<Seen...>(List{}, world, entity);
}

} // namespace Detail

} // namespace GameEngine::ECS

