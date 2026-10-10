// D-SDK reject: deduced pointer parameter on an Optional<> slot (intent must
// be concrete const T*/T*).
#include "CompileFailCommon.h"

void Case(CompileFailCase::World& world)
{
    using namespace CompileFailCase;
    world.Query<Position, Optional<Velocity>>().Each(
        [](EntityHandle, Position&, auto*) {});
}
