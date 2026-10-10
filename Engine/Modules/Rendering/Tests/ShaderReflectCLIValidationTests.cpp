#include <gtest/gtest.h>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif
#include <cstdio>
#include <string>
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"

using namespace GameEngine::Rendering;

TEST(ShaderReflectCLIValidation, FailsOnDescriptorConflict) {
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

    // Use conflict shaders (set0,binding0 UBO vs sampler)
    std::string vs = fs::absolute(Utils::ResolveShaderPath("conflict_vs.vert.spv")).string();
    std::string fs_ = fs::absolute(Utils::ResolveShaderPath("conflict_fs.frag.spv")).string();
    ASSERT_TRUE(fs::exists(vs)) << vs;
    ASSERT_TRUE(fs::exists(fs_)) << fs_;

    // Run via _popen so the tool's diagnostic output is captured: a bare nonzero
    // exit code is not proof the tool ran (a failed spawn is also nonzero, which
    // this test once green-lit for months).
    auto runCliCapture = [](const fs::path& exe, const std::string& args, std::string& output) -> int {
        const std::string cmd = "\"\"" + exe.string() + "\" " + args + " 2>&1\"";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe)
            return -1;
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe))
            output += buf;
        return _pclose(pipe);
    };

    std::string output;
    const int rc = runCliCapture(exePath,
                                 "--vs \"" + vs + "\" --fs \"" + fs_ + "\" --out conflict.shaderdesc",
                                 output);
    EXPECT_NE(rc, 0) << "CLI should fail validation on conflicts; output:\n" << output;
    EXPECT_NE(output.find("DescriptorConflict"), std::string::npos)
        << "expected the tool's conflict diagnostic (proves it actually ran); got:\n" << output;
#endif
}
