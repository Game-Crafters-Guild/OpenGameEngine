#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif
#include "TestUtils.h"

using nlohmann::json;
using namespace GameEngine::Rendering;

TEST(ShaderReflectCLI, EmitsPushConstantIdsInShaderDesc) {
#if !defined(_WIN32)
    GTEST_SKIP() << "CLI tool tests are Windows-only";
#elif !RENDERING_ENABLE_SPIRV_REFLECTION
    GTEST_SKIP() << "Reflection disabled";
#else
    namespace fs = std::filesystem;

    // Locate the ShaderReflect CLI relative to THIS test binary, never the cwd:
    // tests stage to bin/<config>/Tests/, tools to the canonical
    // bin/<config>/Tools/ (see cmake/OutputLayout.cmake).
    auto locateShaderReflect = []() -> fs::path {
        char self[MAX_PATH]{};
        ::GetModuleFileNameA(nullptr, self, MAX_PATH);
        return fs::path(self).parent_path().parent_path() / "Tools" / "ShaderReflect.exe";
    };
    const fs::path exePath = locateShaderReflect();
    ASSERT_TRUE(fs::exists(exePath)) << "ShaderReflect.exe not staged relative to test binary: " << exePath.string();
    std::string exe = exePath.string();

    // Input SPIR-V (compute shader with push constants). The blob is produced by
    // the CompileShaders dependency into the build output shader dir; a miss here
    // is a build regression, not an environment condition.
    fs::path csPath;
    ASSERT_TRUE(GameEngine::Rendering::Tests::ResolveShaderPath("pc_compute.comp.spv", csPath))
        << "pc_compute.comp.spv not found in CompileShaders output";
    std::string csAbs = fs::absolute(csPath).string();

    // Run CLI to emit .shaderdesc JSON
    const char* outPath = "pc_compute_cli.shaderdesc";
    auto runCli = [](const std::string& exePath, const std::vector<std::string>& args) -> int {
        std::vector<const char*> argv; argv.reserve(args.size() + 2);
        argv.push_back(exePath.c_str()); for (auto& a : args) argv.push_back(a.c_str()); argv.push_back(nullptr);
        return static_cast<int>(_spawnv(_P_WAIT, exePath.c_str(), argv.data()));
    };
    int rc = runCli(exe, {"--cs", csAbs, "--out", outPath});
    ASSERT_EQ(rc, 0);

    // Parse and validate. The reflection JSON carries pushConstants at top level
    // (the old "meta" envelope is gone along with .shaderdesc sidecars).
    std::ifstream f(outPath); ASSERT_TRUE(f.good()); json j; f >> j;
    ASSERT_TRUE(j.contains("pushConstants"));
    auto a = j["pushConstants"];
    ASSERT_FALSE(a.empty()) << "pc_compute declares a push-constant block; reflection emitted none";
    // Expect ids present and contiguous starting at 0
    std::vector<uint32_t> ids;
    for (auto& pc : a) {
        ASSERT_TRUE(pc.contains("id"));
        ids.push_back(pc.value("id", 9999u));
    }
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i], static_cast<uint32_t>(i));
    }
#endif
}

