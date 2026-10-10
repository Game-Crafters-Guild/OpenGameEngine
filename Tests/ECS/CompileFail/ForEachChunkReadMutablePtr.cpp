// Contradiction on the ChunkCallAdapter pointer shape (with count).
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Read<Position>>().ForEachChunk([](Position*, std::size_t) {});
}
