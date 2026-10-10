#include "Inspectors/Particles/ParticleStackReference.h"

#include "Assets/AssetManager.h"
#include "Particles/Assets/ParticleStackAsset.h"

#include <memory>

namespace GameEngine::ParticleInspectors
{

ParticleStackReference ResolveParticleStackReference(const AssetManager& assets, const GUID& stack)
{
    if (stack.IsNull())
        return ParticleStackReference::Default;
    if (const auto loaded = assets.GetAsset(stack))
        return std::dynamic_pointer_cast<Particles::ParticleStackAsset>(loaded) ? ParticleStackReference::Loaded
                                                                                : ParticleStackReference::NotAStack;
    if (!assets.GetRegistry().IsAssetRegistered(stack))
        return ParticleStackReference::Missing;
    return ParticleStackReference::Loading;
}

} // namespace GameEngine::ParticleInspectors
