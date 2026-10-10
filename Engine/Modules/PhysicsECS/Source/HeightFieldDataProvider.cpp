#include "PhysicsECS/HeightFieldDataProvider.h"

namespace GameEngine::PhysicsECS
{

static HeightFieldDataProvider s_Provider = nullptr;

void SetHeightFieldDataProvider(HeightFieldDataProvider provider)
{
    s_Provider = provider;
}

HeightFieldDataProvider GetHeightFieldDataProvider()
{
    return s_Provider;
}

} // namespace GameEngine::PhysicsECS
