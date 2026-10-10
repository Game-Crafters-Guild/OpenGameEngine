// What the shader graph panel is told to show when a graph comes into view.
// The controller reaches the panel only through its Host callbacks, so the
// verdict handed to the view (ShowDiagnostics / ClearDiagnostics) and the
// compile-refresh request are recorded here in place of the panel. No
// EditorContext and no preview host: the node preview atlas is never built.

#include <gtest/gtest.h>

#include "Graph/GraphModel.h"
#include "ShaderGraph/MaterialGraphController.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

using namespace GameEngine;

namespace {

struct RecordedShow
{
    std::vector<ShaderGraph::SgDiagnostic> Errors;
};

/** Stands in for ShaderGraphPanel on the far side of MaterialGraphController::Host. */
struct RecordingPanel
{
    std::filesystem::path CurrentPath;
    int RefreshRequests = 0;
    std::vector<RecordedShow> Shows;
    std::vector<std::size_t> Clears;

    MaterialGraphController::Host MakeHost()
    {
        MaterialGraphController::Host host;
        host.PreviewHost = []() -> MaterialGraphPreviewHost* { return nullptr; };
        host.RequestCompileRefreshIfStale = [this]() { ++RefreshRequests; };
        host.CurrentPath = [this]() { return CurrentPath; };
        host.ShowDiagnostics = [this](const std::vector<ShaderGraph::SgDiagnostic>& errors)
        { Shows.push_back({errors}); };
        host.ClearDiagnostics = [this](std::size_t onDiskErrorCount)
        { Clears.push_back(onDiskErrorCount); };
        return host;
    }
};

Graph::Model MakeBrokenGraph()
{
    // A constant with no SurfaceOutput to feed: the compiler rejects it.
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);
    Graph::Node color{};
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    model.Nodes = {color};
    return model;
}

Graph::Model MakeCleanGraph()
{
    Graph::Model model{};
    model.KindId.assign(Graph::kKindIdMaterial);

    Graph::Node color{};
    color.Id = "node_color";
    color.TypeId = "ColorConstant";
    color.Parameters["r"] = "1.0";
    color.Parameters["g"] = "0.0";
    color.Parameters["b"] = "0.0";
    color.Ports.push_back({"value", Graph::PortDirection::Out, "float3", "Color"});

    Graph::Node output{};
    output.Id = "node_output";
    output.TypeId = "SurfaceOutput";
    output.Ports.push_back({"BaseColor", Graph::PortDirection::In, "float3", "Base Color"});
    output.Ports.push_back({"Metallic", Graph::PortDirection::In, "float", "Metallic"});
    output.Ports.push_back({"Roughness", Graph::PortDirection::In, "float", "Roughness"});

    model.Nodes = {color, output};
    model.Links.push_back({"link_0", "node_color", "value", "node_output", "BaseColor"});
    return model;
}

MaterialGraphController::CompileContext ContextFor(const Graph::Model& model,
                                                   const std::filesystem::path& path)
{
    MaterialGraphController::CompileContext ctx;
    ctx.Model = &model;
    ctx.Path = path;
    return ctx;
}

/** Graph A compiled and failed, so the panel is showing A's verdict. */
void ShowFailedGraph(MaterialGraphController& controller, RecordingPanel& panel,
                     const Graph::Model& broken)
{
    panel.CurrentPath = "Materials/Graph/A.glsl";
    const MaterialGraphController::CompileOutcome outcome =
        controller.Compile(ContextFor(broken, panel.CurrentPath));
    ASSERT_FALSE(outcome.Success);
    ASSERT_EQ(panel.Shows.size(), 1u);
    ASSERT_FALSE(panel.Shows.back().Errors.empty()) << "A must have failed for its verdict to stand";
    ASSERT_TRUE(panel.Clears.empty());
}

/** The toolbar toggles operate on optional widgets; none here. */
const MaterialGraphController::ToolbarItems kNoToolbar{};

} // namespace

TEST(MaterialGraphControllerTests, OpeningAnotherGraphWithNoRefreshClearsThePreviousVerdict)
{
    RecordingPanel panel;
    MaterialGraphController controller(panel.MakeHost());
    controller.SetAutoCompileEnabled(false);
    controller.TogglePreview(kNoToolbar);
    ASSERT_FALSE(controller.PreviewEnabled());
    panel.RefreshRequests = 0;

    const Graph::Model broken = MakeBrokenGraph();
    ShowFailedGraph(controller, panel, broken);

    /* Auto-compile and the preview are off, so no compile will run for B: the
       panel would keep A's label and strip until the user pressed Compile. */
    const Graph::Model clean = MakeCleanGraph();
    panel.CurrentPath = "Materials/Graph/B.glsl";
    controller.OnModelShown(clean);

    ASSERT_EQ(panel.Clears.size(), 1u) << "the view must reflect the file now shown";
    /* No RenderServices in this process, so the Shader Errors mirror holds no
       rows for B: the on-disk hint count is zero. */
    EXPECT_EQ(panel.Clears.front(), 0u);
    EXPECT_EQ(panel.Shows.size(), 1u) << "nothing compiled, so no new verdict";
    EXPECT_EQ(panel.RefreshRequests, 1)
        << "the request still goes out so the pip reads stale for the uncompiled graph";
}

TEST(MaterialGraphControllerTests, OpeningAnotherGraphWithAutoCompileLeavesTheVerdictToTheCompile)
{
    RecordingPanel panel;
    MaterialGraphController controller(panel.MakeHost());
    ASSERT_TRUE(controller.AutoCompileEnabled());
    controller.TogglePreview(kNoToolbar);
    panel.RefreshRequests = 0;

    const Graph::Model broken = MakeBrokenGraph();
    ShowFailedGraph(controller, panel, broken);

    const Graph::Model clean = MakeCleanGraph();
    panel.CurrentPath = "Materials/Graph/B.glsl";
    controller.OnModelShown(clean);

    EXPECT_EQ(panel.RefreshRequests, 1);
    EXPECT_TRUE(panel.Clears.empty()) << "a compile is coming; it replaces the verdict";

    // The refresh the panel schedules compiles B and reports B's own verdict.
    const MaterialGraphController::CompileOutcome outcome =
        controller.Compile(ContextFor(clean, panel.CurrentPath));
    ASSERT_TRUE(outcome.Success);
    ASSERT_EQ(panel.Shows.size(), 2u);
    EXPECT_TRUE(panel.Shows.back().Errors.empty());
    EXPECT_TRUE(panel.Clears.empty());
}

TEST(MaterialGraphControllerTests, PreviewOnWithAHostStillWantsTheRefresh)
{
    RecordingPanel panel;
    MaterialGraphController controller(panel.MakeHost());
    controller.SetAutoCompileEnabled(false);
    ASSERT_TRUE(controller.PreviewEnabled());
    // The panel's gate: a visible preview compiles the graph it shows.
    EXPECT_TRUE(controller.WantsCompileRefresh(/*previewHostPresent=*/true));
    EXPECT_FALSE(controller.WantsCompileRefresh(/*previewHostPresent=*/false));
}

TEST(MaterialGraphControllerTests, EmptyModelShownReadsUpToDateAndClearsTheView)
{
    RecordingPanel panel;
    MaterialGraphController controller(panel.MakeHost());
    controller.SetCompileUpToDate(false);

    const Graph::Model empty{};
    panel.CurrentPath = "Materials/Graph/Empty.glsl";
    controller.OnModelShown(empty);

    EXPECT_TRUE(controller.CompileUpToDate()) << "nothing to compile, so nothing is stale";
    ASSERT_EQ(panel.Clears.size(), 1u);
    EXPECT_EQ(panel.Clears.front(), 0u);
    EXPECT_TRUE(panel.Shows.empty());
    EXPECT_EQ(panel.RefreshRequests, 0);
}

TEST(MaterialGraphControllerTests, CompilingAnEmptyModelReadsUpToDate)
{
    RecordingPanel panel;
    MaterialGraphController controller(panel.MakeHost());
    controller.SetCompileUpToDate(false);

    const Graph::Model empty{};
    const MaterialGraphController::CompileOutcome outcome =
        controller.Compile(ContextFor(empty, "Materials/Graph/Empty.glsl"));

    EXPECT_FALSE(outcome.Success) << "nothing compiled";
    EXPECT_TRUE(controller.CompileUpToDate()) << "a Compile click on an empty graph has nothing to redo";
    ASSERT_EQ(panel.Clears.size(), 1u);
    EXPECT_TRUE(panel.Shows.empty());
}
