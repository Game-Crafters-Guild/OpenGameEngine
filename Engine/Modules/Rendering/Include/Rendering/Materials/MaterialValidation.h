#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include "Rendering/Materials/ShaderMetaValidation.h"

namespace GameEngine { namespace Rendering {

struct MaterialValidationInput
{
    std::vector<std::string> propertyNames;
    std::vector<std::string> textureNames;
    // Optional explicit bindings (uniformName -> propertyKey).
    std::unordered_map<std::string, std::string> bindings;
};

// Validates a material's declared properties/textures against reflected ShaderMeta.
// Returns ValidationReport with:
// - unknown properties -> warning
// - missing texture bindings (by name) -> error
ValidationReport ValidateMaterialAgainstMeta(const ShaderMeta& meta,
                                            const MaterialValidationInput& input);

}} // namespace GameEngine::Rendering

