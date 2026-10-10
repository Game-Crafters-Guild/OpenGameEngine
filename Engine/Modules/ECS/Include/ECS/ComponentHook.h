#pragma once

#include <cstdint>

namespace GameEngine::ECS
{

enum class ComponentHookKind : std::uint8_t { Add, Set, Remove };

// Identifies one registration, including replacement by the same callback.
// Discarding a token does not unregister: RegisterOn* remains persistent.
class ComponentHookToken
{
  public:
    ComponentHookToken() = default;
    explicit operator bool() const { return m_RegistrationId != 0; }
    bool operator==(const ComponentHookToken&) const = default;

  private:
    friend class World;
    ComponentHookToken(std::uint64_t worldId, std::uint64_t component,
                       ComponentHookKind kind, std::uint64_t registrationId)
        : m_WorldId(worldId), m_Component(component), m_Kind(kind), m_RegistrationId(registrationId) {}

    std::uint64_t m_WorldId = 0;
    std::uint64_t m_Component = 0;
    ComponentHookKind m_Kind = ComponentHookKind::Remove;
    std::uint64_t m_RegistrationId = 0;
};

} // namespace GameEngine::ECS
