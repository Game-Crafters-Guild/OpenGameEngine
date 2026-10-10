#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "Rendering/Materials/ShaderMeta.h"

namespace GameEngine { namespace Rendering {

enum class ShaderStageKind { Vertex, Fragment, Compute, Geometry, Mesh, TessControl, TessEval };

struct ReflectionOptions { bool IncludeBuiltins = false; };

struct StageReflectionResult {
    ShaderStageKind Stage;
    StageMeta Meta;                         // inputs/outputs, entry point, sizes
    std::vector<DescriptorSetMeta> Sets;        // descriptor sets discovered in this stage
    std::vector<PushConstantRangeMeta> Pcr;     // push constant ranges in this stage
    std::vector<SpecConstantMeta> SpecConstants;// specialization constants in this stage
};

// Reflect a single SPIR-V module for one stage
// words: pointer to SPIR-V words (uint32)
// Returns false on error; outError contains a short message.
bool ReflectSpirv(ShaderStageKind stage, const uint32_t* words, size_t wordCount,
                  const ReflectionOptions& opts,
                  StageReflectionResult& out,
                  std::string* outError = nullptr);

// Merge stage-wise reflection results into a single ShaderMeta
ShaderMeta MergeStages(const std::vector<StageReflectionResult>& stages);

}} // namespace GameEngine::Rendering

