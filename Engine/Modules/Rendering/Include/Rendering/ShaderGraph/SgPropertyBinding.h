#pragma once

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ShaderGraph
{

struct SgPublicPropertyBinding
{
    int ParameterSlot = 0;
    std::string FloatComponent;
    std::string VectorSwizzle;
    std::vector<std::string> MaterialPropertyKeys;
};

std::optional<SgPublicPropertyBinding> ResolvePublicPropertyBinding(std::string_view graphPropertyName,
                                                                    std::string_view graphGlslType);

std::optional<SgPublicPropertyBinding> ResolveParameterSlotMaterialBinding(int slot,
                                                                           std::string_view nodeTypeId,
                                                                           std::string_view floatComponent = {});

void ApplyPublicPropertyToMaterialDocument(const SgGraphProperty& prop,
                                           const SgPublicPropertyBinding& binding,
                                           MaterialDocument& doc);

void ApplyParameterNodeValueToMaterialDocument(const SgGraphProperty& prop,
                                               const SgPublicPropertyBinding& binding,
                                               MaterialDocument& doc);

void RemoveGraphPropertyFromMaterialDocument(const SgGraphProperty& prop,
                                             const SgPublicPropertyBinding& binding,
                                             MaterialDocument& doc);

void ApplyShaderGraphPropertiesToMaterialDocument(const std::vector<SgGraphProperty>& properties,
                                                  MaterialDocument& doc);

std::string PrettyGraphPropertyLabel(std::string_view graphPropertyName);

bool UsesDedicatedShaderGraphInspectorRow(const SgPublicPropertyBinding& binding,
                                          std::string_view graphPropertyName);

} // namespace GameEngine::ShaderGraph
