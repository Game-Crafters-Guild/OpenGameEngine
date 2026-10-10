#include "Particles/Assets/ParticleStackParser.h"

#include "AssetCore/Asset.h"
#include "Particles/Assets/ParticleStackAsset.h"

#include <memory>

namespace GameEngine::Particles
{

AssetParseResult ParticleStackParser::Parse(const AssetMetadata& metadata, AssetManager&)
{
    return AssetParseResult(std::make_shared<ParticleStackAsset>(metadata.Guid, metadata.Path));
}

bool ParticleStackParser::ExtractDependencies(const GUID&, const AssetMetadata&, DepEdgeSink&) const
{
    return true;
}

bool ParticleStackParser::CookForPackage(const std::filesystem::path& stagedFile, std::string& error) const
{
    return ParticleStackAsset::CookFile(stagedFile, error);
}

} // namespace GameEngine::Particles
