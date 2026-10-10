// POSITIVE CONTROL — this file must keep compiling. If it breaks, the
// compile-fail harness is testing a broken include set, and every WILL_FAIL
// case above is passing vacuously.
#include "CompileFailCommon.h"
#include "ECS/ChangeFilter.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    GameEngine::ECS::ChangeGate gate;
    auto q = world.Query<Read<Position>, Write<Velocity>>();
    q.Changed<Position>(gate);
    q.Each([](EntityHandle, const Position&, Velocity&) {});
    world.Query<Read<Position>>().Each([](EntityHandle, auto&) {}); // qualified deduced = OK
    world.Query<Position>().BatchEach([](const Position*, std::size_t) {});
    GameEngine::ECS::Entity(&world, world.CreateEntity()).SetEnabled<Position>(false); // a toggleable type = OK
}
