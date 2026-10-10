// M4a lattice hole: Read<T> + concrete mutable parameter must be a
// contradiction static_assert BEFORE any plain-refs fallback is selected.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Read<Position>>().Each([](EntityHandle, Position&) {});
}
