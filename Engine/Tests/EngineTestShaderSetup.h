#pragma once

#include <filesystem>

namespace GameEngine::Testing
{
// The shader-path resolver EngineTestShaderSetup.cpp installs at static
// initialization in every test binary that compiles it: a relative shader path
// resolves against ENGINE_TEST_BUILD_DIR, where CompileShaderPkgs stages the
// .shaderpkg / .spv outputs. The resolver is process-global, so a test that
// installs another one (EngineCore::Initialize does) puts this one back when it
// finishes; the tests after it in the binary resolve their shaders through it.
std::filesystem::path EngineTestShaderPathResolver(const std::filesystem::path& relativePath);
} // namespace GameEngine::Testing
