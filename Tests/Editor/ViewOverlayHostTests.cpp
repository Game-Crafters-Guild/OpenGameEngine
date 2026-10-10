// The view overlay host: a view panel publishes its viewport, the host adds one overlay layer to it
// and presents every registered overlay there, whichever came first. The Scene View's load banner
// is the first registered overlay.

#include <gtest/gtest.h>

#include "Editor/RenderPipeline/RenderPipelineStandInNotice.h"
#include "EditorContext.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Scene/SceneLoadProgressOverlay.h"
#include "UI/UIElement.h"
#include "UI/ViewOverlayHost.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace GameEngine;
using GameEngine::Editor::ViewOverlay;
using GameEngine::Editor::ViewOverlayCamera;
using GameEngine::Editor::ViewOverlayHost;
using GameEngine::Editor::ViewOverlayView;

namespace
{

struct Presentation
{
    ViewOverlayView View = ViewOverlayView::Scene;
    UIElement* Layer = nullptr;
    SceneViewController* Camera = nullptr;
};

class RecordingOverlay final : public ViewOverlay
{
  public:
    explicit RecordingOverlay(std::vector<Presentation>& presentations, int& updates)
        : m_Presentations(presentations), m_Updates(updates)
    {
    }
    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override
    {
        m_Presentations.push_back({view, &layer, camera ? camera() : nullptr});
    }
    void Update() override { ++m_Updates; }

  private:
    std::vector<Presentation>& m_Presentations;
    int& m_Updates;
};

std::vector<UIElement*> LayersOf(const UIElement& viewport)
{
    std::vector<UIElement*> layers;
    for (const auto& child : viewport.GetChildren())
    {
        if (child && child->HasClass("view-overlay-layer"))
            layers.push_back(child.get());
    }
    return layers;
}

UIElement* OnlyChild(const UIElement& layer)
{
    return layer.GetChildren().size() == 1 ? layer.GetChildren().front().get() : nullptr;
}

UIElement* ChildWithClass(const UIElement& layer, const char* className)
{
    for (const auto& child : layer.GetChildren())
    {
        if (child && child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

} // namespace

TEST(ViewOverlayHostTests, AnOverlayIsPresentedOnEveryPublishedViewportOnce)
{
    ViewOverlayHost host;
    auto sceneViewport = std::make_unique<UIElement>();
    UIElement gameViewport;
    host.PublishViewport(ViewOverlayView::Scene, *sceneViewport, {});

    std::vector<Presentation> first;
    int firstUpdates = 0;
    host.Register(std::make_unique<RecordingOverlay>(first, firstUpdates));
    ASSERT_EQ(LayersOf(*sceneViewport).size(), 1u);
    ASSERT_EQ(first.size(), 1u) << "an overlay registered after a viewport was published was not presented on it";
    EXPECT_EQ(first[0].View, ViewOverlayView::Scene);
    EXPECT_EQ(first[0].Layer, LayersOf(*sceneViewport)[0]);

    host.PublishViewport(ViewOverlayView::Game, gameViewport, {});
    host.PublishViewport(ViewOverlayView::Scene, *sceneViewport, {});
    EXPECT_EQ(LayersOf(*sceneViewport).size(), 1u) << "publishing again added a second layer";
    ASSERT_EQ(LayersOf(gameViewport).size(), 1u);
    ASSERT_EQ(first.size(), 2u) << "publishing again presented the overlay again";
    EXPECT_EQ(first[1].View, ViewOverlayView::Game);
    EXPECT_EQ(first[1].Layer, LayersOf(gameViewport)[0]);

    host.Update();
    EXPECT_EQ(firstUpdates, 1);

    // A destroyed viewport is not presented on again.
    sceneViewport.reset();
    std::vector<Presentation> second;
    int secondUpdates = 0;
    host.Register(std::make_unique<RecordingOverlay>(second, secondUpdates));
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].View, ViewOverlayView::Game);
}

// A positional overlay projects with the camera of the viewport its layer sits on: each layer
// hands its overlays that viewport's camera, whichever was registered first.
TEST(ViewOverlayHostTests, EachLayerHandsItsOverlaysItsViewportsCamera)
{
    // Stand-ins: the host only passes the controllers along, it never calls them.
    std::array<std::byte, 2> controllers{};
    auto* perspective = reinterpret_cast<SceneViewController*>(&controllers[0]);
    auto* front = reinterpret_cast<SceneViewController*>(&controllers[1]);

    ViewOverlayHost host;
    UIElement perspectiveViewport;
    UIElement frontViewport;
    host.PublishViewport(ViewOverlayView::Scene, perspectiveViewport, [perspective]() { return perspective; });

    std::vector<Presentation> presentations;
    int updates = 0;
    host.Register(std::make_unique<RecordingOverlay>(presentations, updates));
    host.PublishViewport(ViewOverlayView::Scene, frontViewport, [front]() { return front; });

    ASSERT_EQ(presentations.size(), 2u);
    EXPECT_EQ(presentations[0].Layer, LayersOf(perspectiveViewport)[0]);
    EXPECT_EQ(presentations[0].Camera, perspective);
    EXPECT_EQ(presentations[1].Layer, LayersOf(frontViewport)[0]);
    EXPECT_EQ(presentations[1].Camera, front);
}

TEST(ViewOverlayHostTests, TheSceneLoadBannerShowsTheProgressOnTheSceneViewOnly)
{
    uint64_t processed = 3;
    uint64_t total = 10;
    EditorContext context;
    context.SceneBuildProgress = [&](uint64_t& outProcessed, uint64_t& outTotal)
    {
        outProcessed = processed;
        outTotal = total;
        return total > 0;
    };
    ViewOverlayHost host;
    UIElement sceneViewport;
    UIElement gameViewport;
    host.Register(std::make_unique<Editor::SceneLoadProgressOverlay>(context));
    host.PublishViewport(ViewOverlayView::Scene, sceneViewport, {});
    host.PublishViewport(ViewOverlayView::Game, gameViewport, {});
    ASSERT_EQ(LayersOf(gameViewport).size(), 1u);
    EXPECT_TRUE(LayersOf(gameViewport)[0]->GetChildren().empty()) << "the load banner was put on the Game View";
    ASSERT_EQ(LayersOf(sceneViewport).size(), 1u);
    UIElement* banner = OnlyChild(*LayersOf(sceneViewport)[0]);
    ASSERT_NE(banner, nullptr);
    EXPECT_TRUE(banner->HasClass("hidden")) << "the banner shows before any update";

    host.Update();
    EXPECT_FALSE(banner->HasClass("hidden"));
    EXPECT_EQ(banner->GetTextContent(), "Loading scene... 3 / 10");

    processed = 7;
    host.Update();
    EXPECT_EQ(banner->GetTextContent(), "Loading scene... 7 / 10");

    total = 0;
    host.Update();
    EXPECT_TRUE(banner->HasClass("hidden")) << "the banner stayed up after the load";
}

// The refused-pipeline notice is on both views and hidden while no stand-in draws; its text names the
// requested pipeline, says what the stand-in leaves out, and gives the reason.
TEST(ViewOverlayHostTests, ThePipelineStandInNoticeIsOnBothViewsAndNamesTheRequestTheStandInAndTheReason)
{
    EditorContext context;
    ViewOverlayHost host;
    UIElement sceneViewport;
    UIElement gameViewport;
    host.Register(std::make_unique<Editor::RenderPipelineStandInNotice>(context));
    host.PublishViewport(ViewOverlayView::Scene, sceneViewport, {});
    host.PublishViewport(ViewOverlayView::Game, gameViewport, {});
    host.Update();
    for (UIElement* viewport : {&sceneViewport, &gameViewport})
    {
        ASSERT_EQ(LayersOf(*viewport).size(), 1u);
        UIElement* notice = ChildWithClass(*LayersOf(*viewport)[0], "render-pipeline-stand-in-notice");
        ASSERT_NE(notice, nullptr) << "the notice is missing from a view";
        EXPECT_TRUE(notice->HasClass("hidden")) << "the notice shows with no stand-in drawing";
        UIElement* waitNote = ChildWithClass(*LayersOf(*viewport)[0], "render-pipeline-waiting-note");
        ASSERT_NE(waitNote, nullptr) << "the waiting note is missing from a view's overlay layer";
        EXPECT_TRUE(waitNote->HasClass("hidden")) << "the waiting note shows with nothing waiting";
    }

    Engine::Renderer::PipelineStandIn standIn;
    standIn.RequestedPath = "RenderPipelines/ForwardPlus.rendergraph";
    standIn.StandInName = "ForwardPlus";
    standIn.Reason = "Unknown pass type 'MissionFog': no loaded scripts register it.";
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::Describe(standIn),
              "'RenderPipelines/ForwardPlus.rendergraph' can't be used, so this view draws the engine's default "
              "pipeline instead. Unknown pass type 'MissionFog': no loaded scripts register it.");
    standIn.StandInName.clear();
    standIn.StandInFailure = "The engine's pipeline file is missing (RenderPipelines/ForwardPlus.rendergraph).";
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::Describe(standIn),
              "'RenderPipelines/ForwardPlus.rendergraph' can't be used, and the engine's default pipeline can't "
              "stand in for it, so this view draws nothing. Unknown pass type 'MissionFog': no loaded scripts "
              "register it. The engine's pipeline file is missing (RenderPipelines/ForwardPlus.rendergraph).");

    // While the request waits for scripts, the note says what draws meanwhile and counts the seconds.
    standIn.WaitingForModules = true;
    standIn.PendingPasses = {"MissionFog"};
    standIn.StandInName.clear();
    standIn.StandInFailure.clear();
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::DescribeWait(standIn, 12),
              "'RenderPipelines/ForwardPlus.rendergraph' uses a pass from the project's scripts ('MissionFog'), "
              "which are still building (12 s so far). This view draws the rest of the pipeline until then; that "
              "pass joins once the scripts are built.");
    standIn.WaitingReaders = {"FogComposite"};
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::DescribeWait(standIn, 12),
              "'RenderPipelines/ForwardPlus.rendergraph' uses a pass from the project's scripts ('MissionFog'), "
              "which are still building (12 s so far); 'FogComposite' reads its output and waits with it. This view "
              "draws the rest of the pipeline until then; they join once the scripts are built.");
    standIn.PendingPasses = {"MissionFog", "MissionRain"};
    standIn.WaitingReaders.clear();
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::DescribeWait(standIn, 12),
              "'RenderPipelines/ForwardPlus.rendergraph' uses passes from the project's scripts ('MissionFog', "
              "'MissionRain'), which are still building (12 s so far). This view draws the rest of the pipeline "
              "until then; they join once the scripts are built.");
    standIn.PendingPasses = {"MissionFog"};
    standIn.StandInName = "ForwardPlus";
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::DescribeWait(standIn, 12),
              "'RenderPipelines/ForwardPlus.rendergraph' uses a pass from the project's scripts ('MissionFog'), "
              "which are still building (12 s so far). The pipeline's output comes from that pass, so this view "
              "draws the engine's default pipeline until then; the project's pipeline replaces it once its scripts "
              "are built.");
    standIn.StandInName.clear();
    standIn.LastVersionDraws = true;
    EXPECT_EQ(Editor::RenderPipelineStandInNotice::DescribeWait(standIn, 12),
              "'RenderPipelines/ForwardPlus.rendergraph' uses a pass from the project's scripts ('MissionFog'), "
              "which are still building (12 s so far). The pipeline's output comes from that pass, so this view "
              "keeps the pipeline's last version until then.");
}
