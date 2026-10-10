// A component that declares NotToggleable has no on/off state: switching it
// through Entity::SetEnabled<T> must not compile (ComponentFlags.h).
#include "CompileFailCommon.h"

namespace
{
struct Placement
{
    float X = 0.0f;
    static constexpr bool NotToggleable = true;
};
} // namespace

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    const EntityHandle entity = world.CreateEntity();
    GameEngine::ECS::Entity(&world, entity).SetEnabled<Placement>(false);
}
