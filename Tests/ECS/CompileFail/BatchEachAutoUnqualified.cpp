// D-SDK reject on the pointer/count shape.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Position>().BatchEach([](auto*, std::size_t) {});
}
