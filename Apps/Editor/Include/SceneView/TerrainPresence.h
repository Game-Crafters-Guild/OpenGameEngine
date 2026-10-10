#pragma once

#include <cstddef>
#include <cstdint>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// Whether a world holds a terrain, recounted only when the world or its structure changed,
// so the terrain brush's tool strip entry can ask every frame.
class TerrainPresence
{
public:
    bool HasTerrain(ECS::World* world);

private:
    struct Key
    {
        const ECS::World* World = nullptr;
        uint64_t WorldId = 0;
        uint64_t ResetGeneration = 0;
        std::size_t StructureVersion = 0;
        bool operator==(const Key&) const = default;
    };

    Key m_Key;
    bool m_Valid = false;
    bool m_HasTerrain = false;
};

} // namespace GameEngine::Editor
