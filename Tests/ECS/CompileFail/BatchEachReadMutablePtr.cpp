// Contradiction on the BatchEach pointer shape.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Read<Position>>().BatchEach([](Position*, std::size_t) {});
}
