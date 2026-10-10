#pragma once

#include "Types/Types.h"

namespace GameEngine {

class AssetManager;

struct ProjectLodReimportResult {
    uint32 Regenerated = 0;
    uint32 Skipped = 0; // opted out, missing metadata, or failed to load
};

// The "Reimport LODs (project)" batch: regenerate the LOD chain of every Model
// asset whose resolved LOD settings opt in (Generate), and dispatch
// AssetReloaded for each so the GPU registry re-uploads the new LOD tables. A
// visible command rather than a silent auto-mutation, so artists see the batch
// and can check silhouettes. Blocks the calling thread on each model's load.
ProjectLodReimportResult ReimportProjectLods(AssetManager& assets);

} // namespace GameEngine
