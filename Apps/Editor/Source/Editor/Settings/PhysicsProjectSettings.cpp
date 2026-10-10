#include "Editor/Settings/PhysicsProjectSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace GameEngine::Editor::PhysicsProjectSettings
{
namespace
{
constexpr const char* kPhysicsKey = "physics";
constexpr const char* kGravityKey = "gravity";
constexpr const char* kFixedTimeStepKey = "fixedTimeStep";
constexpr const char* kMaxSubStepsKey = "maxSubSteps";
constexpr const char* kCollisionStepsKey = "collisionSteps";
constexpr const char* kMaxBodiesKey = "maxBodies";
constexpr const char* kMaxBodyPairsKey = "maxBodyPairs";
constexpr const char* kMaxContactConstraintsKey = "maxContactConstraints";
constexpr const char* kTempAllocatorBytesKey = "tempAllocatorBytes";
constexpr const char* kNumThreadsKey = "numThreads";
constexpr const char* kLayerCollisionMatrixKey = "layerCollisionMatrix";

const nlohmann::json* FindPhysicsObject(const SettingsStore& store)
{
    const auto& root = store.Json();
    if (!root.is_object())
        return nullptr;
    const auto it = root.find(kPhysicsKey);
    if (it == root.end() || !it->is_object())
        return nullptr;
    return &(*it);
}

void ReadU32(const nlohmann::json& physics, const char* key, uint32& dst)
{
    const auto it = physics.find(key);
    if (it == physics.end() || !(it->is_number_integer() || it->is_number_unsigned()))
        return;
    dst = static_cast<uint32>(std::min<uint64_t>(it->get<uint64_t>(), 0xFFFFFFFFull));
}

void ReadI32(const nlohmann::json& physics, const char* key, int32& dst)
{
    const auto it = physics.find(key);
    if (it == physics.end() || !it->is_number_integer())
        return;
    const auto v = it->get<int64_t>();
    dst = static_cast<int32>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}
} // namespace

Physics::PhysicsWorldSettings Load(const std::filesystem::path& workspaceRoot)
{
    Physics::PhysicsWorldSettings out{};
    if (workspaceRoot.empty())
        return out;

    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    // A hand-edited file can hold any type under any key, so every read below is
    // type-guarded and a rejected key costs only its own default -- never the
    // keys read after it. The catch is a backstop for reads added later; no
    // input reaches it through the guards as they stand.
    try
    {
        const nlohmann::json* physics = FindPhysicsObject(store);
        if (physics == nullptr)
            return out;

        // Every element is checked before any is read: a partial fill would leave
        // gravity pointing somewhere the file never asked for.
        if (const auto it = physics->find(kGravityKey);
            it != physics->end() && it->is_array() && it->size() == 3 && (*it)[0].is_number() &&
            (*it)[1].is_number() && (*it)[2].is_number())
        {
            out.gravity.x = static_cast<float32>((*it)[0].get<double>());
            out.gravity.y = static_cast<float32>((*it)[1].get<double>());
            out.gravity.z = static_cast<float32>((*it)[2].get<double>());
        }

        if (const auto it = physics->find(kFixedTimeStepKey);
            it != physics->end() && it->is_number())
        {
            out.fixedTimeStep = static_cast<float32>(it->get<double>());
        }

        ReadI32(*physics, kMaxSubStepsKey, out.maxSubSteps);
        ReadI32(*physics, kCollisionStepsKey, out.collisionSteps);
        ReadI32(*physics, kNumThreadsKey, out.numThreads);
        ReadU32(*physics, kMaxBodiesKey, out.maxBodies);
        ReadU32(*physics, kMaxBodyPairsKey, out.maxBodyPairs);
        ReadU32(*physics, kMaxContactConstraintsKey, out.maxContactConstraints);
        ReadU32(*physics, kTempAllocatorBytesKey, out.tempAllocatorBytes);

        if (const auto it = physics->find(kLayerCollisionMatrixKey);
            it != physics->end() && it->is_array() && it->size() == Physics::kMaxCollisionLayers)
        {
            for (uint32 i = 0; i < Physics::kMaxCollisionLayers; ++i)
            {
                const auto& v = (*it)[i];
                if (v.is_number_integer() || v.is_number_unsigned())
                    out.layerCollisionMatrix.mask[i] = static_cast<uint32>(v.get<uint64_t>());
            }
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        Logger::Log::Warning("PhysicsProjectSettings: malformed physics block ({}) -- using defaults",
                             e.what());
    }

    return out;
}

bool SaveSimulationSettings(const std::filesystem::path& workspaceRoot,
                            const Physics::PhysicsWorldSettings& settings)
{
    if (workspaceRoot.empty())
        return false;

    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string err;
    // An unreadable file leaves the store holding a fresh empty document, so
    // saving into it would replace the whole file and take every sibling block
    // with it. A missing file loads as empty and succeeds, so refusing here does
    // not block writing the settings for the first time.
    if (!store.Load(&err))
    {
        Logger::Log::Error("PhysicsProjectSettings: not saving into unreadable project settings "
                           "({}); the file is left untouched",
                           err);
        return false;
    }

    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    auto& physics = root[kPhysicsKey];
    if (!physics.is_object())
        physics = nlohmann::json::object();

    physics[kGravityKey] =
        nlohmann::json::array({settings.gravity.x, settings.gravity.y, settings.gravity.z});
    physics[kFixedTimeStepKey] = settings.fixedTimeStep;
    physics[kMaxSubStepsKey] = settings.maxSubSteps;
    physics[kCollisionStepsKey] = settings.collisionSteps;

    if (!store.Save(&err))
    {
        Logger::Log::Error("PhysicsProjectSettings: failed to save project settings: {}", err);
        return false;
    }
    return true;
}

} // namespace GameEngine::Editor::PhysicsProjectSettings
