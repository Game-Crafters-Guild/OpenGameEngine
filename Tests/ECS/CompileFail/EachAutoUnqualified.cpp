// D-SDK reject: generic/deduced parameter on an UNQUALIFIED query type.
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Position>().Each([](EntityHandle, auto&) {});
}
