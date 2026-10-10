// Q2: Changed<T> requires T to be one of the query's component types.
#include "CompileFailCommon.h"
#include "ECS/ChangeFilter.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    GameEngine::ECS::ChangeGate gate;
    auto q = world.Query<Read<Position>>();
    q.Changed<Velocity>(gate);
    q.Each([](EntityHandle, const Position&) {});
}
