// Contradiction on the entity-first Parallel shape.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Read<Position>>().Parallel([](EntityHandle, Position&) {});
}
