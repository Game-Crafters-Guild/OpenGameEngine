#pragma once

// Derived-cache record of a model's imported materials: the ImportedMaterialData
// array a load of the model produced, as plain data, at
// <cache root>/ModelMaterials/<model guid>.json. Consumers that need a model's
// materials without importing the model (the project shader warm-up) read it;
// the next load of the model rewrites it. Like every derived artifact it is safe
// to delete and never tracked: the authoritative asset database is not touched.

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <filesystem>
#include <optional>

namespace GameEngine {

class AssetRegistry;
struct ImportedMaterialData;

// The record's location for the model at `modelPath`. nullopt when the owning
// source takes no derived records (AssetRegistry::AcceptsDerivedRecords: a
// packaged game or a published package) or has no derived-cache root.
std::optional<std::filesystem::path> ImportedMaterialCacheFile(const AssetRegistry& registry,
                                                               const std::filesystem::path& modelPath,
                                                               const GUID& modelGuid);

enum class ImportedMaterialCacheWrite : uint8 {
    Written,   // the record changed (or did not exist) and was replaced
    Unchanged, // the record already holds these materials; nothing was written
    Failed,
};

// Replaces the record only when its contents differ, so reloading an unchanged
// model performs no write.
ImportedMaterialCacheWrite WriteImportedMaterialCache(const std::filesystem::path& file,
                                                      const Vector<ImportedMaterialData>& materials);

// Deletes the record of a model that was deleted. Resolves the cache root from
// `modelPath`, since a destroyed model's GUID has already left the registry.
void RemoveImportedMaterialCache(const AssetRegistry& registry, const std::filesystem::path& modelPath,
                                 const GUID& modelGuid);

// nullopt when the record is missing, unreadable, or from another format version.
std::optional<Vector<ImportedMaterialData>> ReadImportedMaterialCache(const std::filesystem::path& file);

} // namespace GameEngine
