// Contradiction on the component-only Each shape.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Read<Position>>().Each([](Position&) {});
}
