// D-SDK reject on the ParallelBatchEach pointer/count shape.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Position>().ParallelBatchEach([](auto*, std::size_t) {});
}
