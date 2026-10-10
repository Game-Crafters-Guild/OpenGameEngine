#pragma once

#include <string>

namespace GameEngine {

inline constexpr const char* kDefaultBuildRenderPipelinePath = "RenderPipelines/ForwardPlus.rendergraph";

std::string LoadBuildGlobalRenderPipeline();
void SaveBuildGlobalRenderPipeline(const std::string& path);

bool LoadBuildPlatformUseGlobalRenderPipeline(const std::string& platformName);
void SaveBuildPlatformUseGlobalRenderPipeline(const std::string& platformName, bool useGlobal);

std::string LoadBuildPlatformRenderPipeline(const std::string& platformName);
void SaveBuildPlatformRenderPipeline(const std::string& platformName, const std::string& path);

std::string ResolveBuildGlobalRenderPipeline();
std::string ResolveBuildRenderPipelineForPlatform(const std::string& platformName);

} // namespace GameEngine
