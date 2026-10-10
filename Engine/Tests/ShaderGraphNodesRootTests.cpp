// GameEngineRendering is linked standalone by its own test executables, so the Rendering module
// cannot call PathUtils and ShaderGraph::GetEngineGraphNodesRoot() carries its own copy of the
// install-assets layout rule. This pins that copy to the owner: both must name the same staged
// directory. The suite stages the node tree beside itself (ge_stage_shader_graph_nodes), so the
// comparison is against a directory that exists.
//
// COVERAGE: a test executable is never an app bundle, so this exercises the copy's flat half
// (<exe>/Assets) only. Its bundle half — Contents/Resources/Assets — runs only inside the Editor
// and Player bundles and has no test; the follow-up that hands the Rendering module its root from
// the host deletes the copy and this pin with it.
#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"

#include <filesystem>

TEST(ShaderGraphNodesRoot, ResolvesToTheInstallAssetsRoot)
{
    const std::filesystem::path expected =
        (GameEngine::PathUtils::GetInstallAssetsRoot() / "Shaders" / "Graph" / "Nodes").lexically_normal();
    ASSERT_TRUE(std::filesystem::is_directory(expected))
        << "shader-graph node helpers not staged at " << expected.string();

    EXPECT_EQ(GameEngine::ShaderGraph::GetEngineGraphNodesRoot().lexically_normal(), expected);
}
