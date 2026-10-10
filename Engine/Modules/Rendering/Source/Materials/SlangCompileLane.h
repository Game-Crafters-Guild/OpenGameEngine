#pragma once

// Dev-only Slang source lane for ShaderCompileService.
//
// `.slang` stages compile through an EXTERNAL slangc invoked as a tool; the
// binary is never vendored into the repo. Two environment variables drive it:
//   GE_SHADER_SLANG_LANE=1   opt the process into the lane (default: off, and
//                            .slang sources are rejected loudly)
//   GE_SLANGC=<path>         absolute path to slangc.exe
//
// Recorded toolchain — the lane is only known to behave with these:
//   slangc   Slang v2026.12.0.1, slang-2026.12.0.1-windows-x86_64.zip,
//            sha256 d7ed90e979266ccf438a66465b95f251af5f8bd8c8fc209da16536d57089a804,
//            from github.com/shader-slang/slang/releases. Never vendored.
//   flags    -target spirv -profile spirv_1_5 -matrix-layout-column-major -O0
//            -preserve-params, entry points named `main`. Treat the set as
//            locked: it was fixed against measured SPIR-V deltas, and a
//            substitution has to be re-measured rather than assumed.
//   baseline glslc from Vulkan SDK 1.4.321.0 (the shaderc the engine embeds),
//            --target-env=vulkan1.2 --target-spv=spv1.5 -O0.
//
// The lane only produces SPIR-V words + a module-dependency list; reflection,
// ShaderMeta, .shaderpkg packaging, and PSO creation are the unchanged
// ShaderCompileService downstream.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine { namespace Rendering { namespace SlangLane {

// GE_SHADER_SLANG_LANE is set (and not "0"). Read fresh on every call so tests
// can toggle it in-process; never called from per-frame code.
bool Enabled();

// Enabled() + GE_SLANGC resolves to an existing file + a version probe
// (`slangc -v`) succeeded. On failure returns false with a message that names
// the missing piece — the lane must fail loudly, never silently fall back.
bool EnsureAvailable(std::string* outError);

// Cache-key lane discriminator: "slang:<slangc version>". Folding the external
// compiler's version into the key is what keeps the no-alias guarantee across
// toolchain updates (the engine-embedded shaderc is covered by the key-format
// version string instead, because it only changes with an engine rebuild).
// Only meaningful after EnsureAvailable() succeeded.
std::string KeyDiscriminator();

struct StageInput
{
    std::filesystem::path sourcePath;   // absolute; logical name when inlineSource is set
    std::string inlineSource;           // when non-empty, compiled via a scratch file
    std::string stageKey;               // "vs","fs","cs","gs","ms"
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> includeDirs; // import/include search roots, in order
    bool targetSpirv16 = false;         // else SPIR-V 1.5
    std::filesystem::path scratchDir;   // for the inline-source file + depfile
};

// Compile one stage via slangc at the spike's locked flag set
// (-O0 -preserve-params -matrix-layout-column-major; entry "main").
// outModuleDeps: canonical absolute paths of every imported module slangc
// reported via -depfile, with the root source itself filtered out — these feed
// the same include-hash revalidation the GLSL includer populates.
bool CompileStage(const StageInput& in,
                  std::vector<uint32_t>& outWords,
                  std::string& outDiagnostics,
                  std::vector<std::string>& outModuleDeps);

}}} // namespace GameEngine::Rendering::SlangLane
