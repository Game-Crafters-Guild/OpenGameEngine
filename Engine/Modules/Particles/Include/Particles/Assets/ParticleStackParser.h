#pragma once

#include "Assets/ParserRegistry.h"

namespace GameEngine::Particles
{

/// Parser for particle stacks (.particlestack). Creates the asset, which reads the file itself; the
/// build cooks the staged copy into the binary form a packaged game loads.
class ParticleStackParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::ParticleStack; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".particlestack"}; }
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;
    std::string GetName() const override { return "ParticleStackParser"; }
    int GetPriority() const override { return 100; }
    /// A stack references no assets: its spawn rules name the emitter's sub-emitter slots.
    bool ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata, DepEdgeSink& sink) const override;
    bool CookForPackage(const std::filesystem::path& stagedFile, std::string& error) const override;
};

} // namespace GameEngine::Particles
