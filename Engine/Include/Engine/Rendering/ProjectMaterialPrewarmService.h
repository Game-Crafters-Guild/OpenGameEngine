#pragma once

#include "Rendering/Materials/ShaderVariantKey.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetManager;

namespace Engine::Renderer
{
class MaterialSystem;

// Main-thread pump for best-effort project material warm-up. Discovery reads
// registry records and, for models, the imported-material record a previous load
// left in the derived cache (Assets/ImportedMaterialCache.h): it never loads a
// model merely to discover its embedded materials. The project is listed once
// when it opens; after that only asset events (a .material created or edited)
// add work, so an idle editor does no discovery at all.
class ProjectMaterialPrewarmService
{
public:
    ProjectMaterialPrewarmService();
    ~ProjectMaterialPrewarmService();

    ProjectMaterialPrewarmService(const ProjectMaterialPrewarmService&) = delete;
    ProjectMaterialPrewarmService& operator=(const ProjectMaterialPrewarmService&) = delete;

    // Called by MaterialSystem once per frame with the open project's root, or
    // an empty root when there is none to warm. A new root gets a short idle
    // delay, then one registry listing once the project's directory scan has
    // finished. Background batches are serialized and repay an elapsed-time
    // budget before the next asset read or compile is admitted; `now` is the
    // frame's steady-clock time those delays are measured against. The listing
    // runs on `pool`, whose load also gates admission.
    // `resolveWorldPassKeywords` is called once per submitted batch.
    //
    // The destructor waits for a listing in flight, so destroy the service
    // before `assets` and `pool`.
    void Update(AssetManager& assets, MaterialSystem& materials, JobSystem::WorkStealingThreadPool& pool,
                const std::filesystem::path& projectRoot, std::chrono::steady_clock::time_point now,
                const std::function<Rendering::MaterialKeyword()>& resolveWorldPassKeywords);

private:
    struct State;
    std::unique_ptr<State> m_State;
};

} // namespace Engine::Renderer
} // namespace GameEngine
