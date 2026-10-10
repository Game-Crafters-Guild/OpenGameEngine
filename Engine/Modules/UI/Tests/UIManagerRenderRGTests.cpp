// Slice 8b — declare-level pins for UIManager::RenderRG (the RenderGraph UI arm).
// Headless: no swapchain, no draws executed — these tests pin the DECLARED
// shape (pass, reads, attachment ops) and the per-frame publish validity
// rules, which is exactly the layer a real-GPU run cannot isolate.
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "RGPassQuery.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;
namespace RGQuery = GameEngine::Testing::RGQuery;

namespace
{

// Pools-by-convention fixture (mirrors Engine/Tests/RenderPipelineDeclareTests.cpp).
struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

const RenderGraph::RGAttachmentRec* FindColorAttachment(const RenderGraph::RGFrame& frame, RenderGraph::RGPassId pass)
{
    for (const auto& rec : frame.Attachments())
        if (rec.Pass == pass && !rec.IsDepth && rec.Slot == 0)
            return &rec;
    return nullptr;
}

TextureDesc MakeTargetDesc(const char* name)
{
    TextureDesc td{};
    td.width = 256;
    td.height = 128;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
               static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = name;
    return td;
}

struct UiFixture
{
    std::unique_ptr<UIManager> Ui;
    bool Ok = false;

    UiFixture(IDevice* dev, bool withExternalBinding)
    {
        UIRegistration::RegisterBuiltInControls();
        const std::string xml = withExternalBinding
            ? R"(<uielement id='root'><uielement id='panel' /><uielement id='viewport' /></uielement>)"
            : R"(<uielement id='root'><uielement id='panel' /></uielement>)";
        std::unique_ptr<UIElement> root;
        if (!XMLParser::ParseLayoutFromString(xml, root))
            return;
        Ui = std::make_unique<UIManager>(dev);
        Ui->SetRoot(std::move(root));

        const auto tmpDir = std::filesystem::temp_directory_path();
        const auto css = tmpDir / (withExternalBinding ? "ui_renderrg_ext.css" : "ui_renderrg.css");
        {
            std::ofstream f(css);
            f << R"(
#root { display: flex; width: 256px; height: 128px; }
#panel { width: 64px; height: 64px; background-color: #336699; }
)";
            if (withExternalBinding)
                f << "#viewport { width: 128px; height: 64px; background-image: engine(test_src); }\n";
        }
        if (!Ui->AttachStyleFromFile(css.string()))
            return;
        Ok = true;
    }
};

} // namespace

TEST(UIManagerRenderRGTests, DeclaresPassReadsAndClearAttachment)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UiFixture fx(dev.get(), /*withExternalBinding=*/true);
        ASSERT_TRUE(fx.Ok);

        // Register the RenderGraph external name BEFORE layout so primitive gen
        // resolves it through the publish path.
        fx.Ui->SetExternalTextureRG("test_src", 128, 64, UI::UITextureSpace::SrgbAuthored(),
                                     Rendering::TextureFormat::RGBA8_SRGB);
        fx.Ui->Update(0.0f, /*interactive=*/false);

        FramePools pools(dev.get());
        RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);

        const RenderGraph::RGTexture src = frame.CreateTexture("Test.Src", MakeTargetDesc("Test.Src"));
        const RenderGraph::RGTexture target =
            frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
        ASSERT_TRUE(src.IsValid());
        ASSERT_TRUE(target.IsValid());

        fx.Ui->PublishExternalTextureRG("test_src", frame, src);
        fx.Ui->RenderRG(frame, target, UI::UITargetSpace::LinearSdr());

        // (1) Exactly one UI pass.
        EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"}), 1u);
        const RenderGraph::RGPassId pass = RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"});
        ASSERT_NE(pass, RenderGraph::kInvalidId);

        // (2) The published binding became a declared sampled read — the
        //     barrier that orders the producer before the UI samples it.
        EXPECT_TRUE(frame.Graph().HasReadAccess(pass, src.Id))
            << "RenderRG must declare a read of the published texture";

        // (3) The SSBO ring imports are declared reads (resource names are
        //     the import keys).
        uint32_t ssboReads = 0;
        for (size_t r = 0; r < frame.Graph().ResourceCount(); ++r)
        {
            const std::string name = frame.Graph().ResourceName(static_cast<RenderGraph::RGResourceId>(r));
            if (name == "UISdfPrims" || name == "UISdfClips" || name == "UISdfDrawOrder")
                if (frame.Graph().HasReadAccess(pass, static_cast<RenderGraph::RGResourceId>(r)))
                    ++ssboReads;
        }
        EXPECT_EQ(ssboReads, 3u) << "the three SDF SSBO ring buffers must be declared reads";

        // (4) Slot-0 color attachment: the target, Clear (the DEFAULT load-op —
        //     the UI owns/clears the target, the editor chrome contract), Store,
        //     opaque black. (The Load path is covered by CompositesWithLoadOp.)
        const RenderGraph::RGAttachmentRec* att = FindColorAttachment(frame, pass);
        ASSERT_NE(att, nullptr);
        EXPECT_EQ(att->Tex, target.Id);
        EXPECT_EQ(att->Ops.Load, RenderGraph::RGLoadOp::Clear);
        EXPECT_EQ(att->Ops.Store, RenderGraph::RGStoreOp::Store);
        EXPECT_FLOAT_EQ(att->Ops.Clear.Color[3], 1.0f);

        frame.Execute();
        dev->WaitForIdle();
    }
    dev->Shutdown();
}

TEST(UIManagerRenderRGTests, CompositesWithLoadOp)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UiFixture fx(dev.get(), /*withExternalBinding=*/false);
        ASSERT_TRUE(fx.Ok);
        fx.Ui->Update(0.0f, /*interactive=*/false);

        FramePools pools(dev.get());
        RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);

        const RenderGraph::RGTexture target =
            frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
        ASSERT_TRUE(target.IsValid());

        // Pass Load: the UI composites OVER the target's existing content (a game
        // HUD over the scene's FinalColor) instead of clearing it.
        fx.Ui->RenderRG(frame, target, UI::UITargetSpace::LinearSdr(), RenderGraph::RGLoadOp::Load);

        const RenderGraph::RGPassId pass = RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"});
        ASSERT_NE(pass, RenderGraph::kInvalidId);
        const RenderGraph::RGAttachmentRec* att = FindColorAttachment(frame, pass);
        ASSERT_NE(att, nullptr);
        EXPECT_EQ(att->Tex, target.Id);
        EXPECT_EQ(att->Ops.Load, RenderGraph::RGLoadOp::Load)
            << "RenderRG(..., Load) must attach the target with Load so the scene survives";
        EXPECT_EQ(att->Ops.Store, RenderGraph::RGStoreOp::Store);

        frame.Execute();
        dev->WaitForIdle();
    }
    dev->Shutdown();
}

TEST(UIManagerRenderRGTests, TargetSpaceDrivesResolvedOutputState)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UiFixture fx(dev.get(), /*withExternalBinding=*/false);
        ASSERT_TRUE(fx.Ok);
        fx.Ui->Update(0.0f, /*interactive=*/false);

        FramePools pools(dev.get());

        // The resolved output state follows the DECLARED target space, not any
        // device/display property (#784): the same manager on the same device
        // stamps 0 for an SDR declaration and 2 for a PQ declaration.
        {
            RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(1);
            const RenderGraph::RGTexture target =
                frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
            ASSERT_TRUE(target.IsValid());
            fx.Ui->RenderRG(frame, target, UI::UITargetSpace::LinearSdr());
            EXPECT_EQ(fx.Ui->GetLastResolvedOutputEncoding(), 0);
            frame.Execute();
            dev->WaitForIdle();
        }
        {
            RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(2);
            const RenderGraph::RGTexture target =
                frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
            ASSERT_TRUE(target.IsValid());
            fx.Ui->RenderRG(frame, target, UI::UITargetSpace::HdrPq());
            EXPECT_EQ(fx.Ui->GetLastResolvedOutputEncoding(), 2);
            EXPECT_FALSE(fx.Ui->GetLastTextSubpixelActive())
                << "an HDR-declared target must resolve the grayscale pipeline";
            frame.Execute();
            dev->WaitForIdle();
        }
    }
    dev->Shutdown();
}

TEST(UIManagerRenderRGTests, StalePublishIsNeverConsumed)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UiFixture fx(dev.get(), /*withExternalBinding=*/true);
        ASSERT_TRUE(fx.Ok);
        fx.Ui->SetExternalTextureRG("test_src", 128, 64, UI::UITextureSpace::SrgbAuthored(),
                                     Rendering::TextureFormat::RGBA8_SRGB);
        fx.Ui->Update(0.0f, /*interactive=*/false);

        FramePools pools(dev.get());
        RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // Frame 1: publish + declare (consumed).
        frame.BeginFrame(1);
        RenderGraph::RGTexture src1 = frame.CreateTexture("Test.Src", MakeTargetDesc("Test.Src"));
        RenderGraph::RGTexture target1 =
            frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
        fx.Ui->PublishExternalTextureRG("test_src", frame, src1);
        fx.Ui->RenderRG(frame, target1, UI::UITargetSpace::LinearSdr());
        const RenderGraph::RGPassId pass1 = RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"});
        ASSERT_NE(pass1, RenderGraph::kInvalidId);
        EXPECT_TRUE(frame.Graph().HasReadAccess(pass1, src1.Id));
        frame.Execute();
        dev->WaitForIdle();

        // Frame 2: NO republish — the frame-1 publish has a stale frameIndex
        // stamp and must be skipped (no read of any frame-1 id; the binding
        // simply doesn't declare).
        frame.BeginFrame(2);
        RenderGraph::RGTexture target2 =
            frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
        fx.Ui->RenderRG(frame, target2, UI::UITargetSpace::LinearSdr());
        const RenderGraph::RGPassId pass2 = RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"});
        ASSERT_NE(pass2, RenderGraph::kInvalidId);
        // Frame-2 ids are dense and REUSED (frame-1's src id aliases frame-2's
        // first resource), so a name scan alone can't catch a deleted
        // FrameIndex compare. The real pin: pass 2 reads NOTHING except the 3
        // SSBO imports — any other read means the stale publish was consumed
        // through an aliased id.
        for (size_t r = 0; r < frame.Graph().ResourceCount(); ++r)
        {
            const auto rid = static_cast<RenderGraph::RGResourceId>(r);
            const std::string name = frame.Graph().ResourceName(rid);
            if (name == "Test.Src")
                FAIL() << "a stale publish must never materialize the previous frame's texture";
            if (frame.Graph().HasReadAccess(pass2, rid))
                EXPECT_TRUE(name == "UISdfPrims" || name == "UISdfClips" ||
                            name == "UISdfDrawOrder")
                    << "unexpected UI-pass read of '" << name
                    << "' — a stale publish leaked through an aliased frame-local id";
        }
        frame.Execute();
        dev->WaitForIdle();
    }
    dev->Shutdown();
}

TEST(UIManagerRenderRGTests, EmptyUiStillDeclaresClearOnlyPass)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        // Root present but zero drawable primitives (no styles attached): the
        // pass must still declare with ClearOps — the UI pass is the
        // backbuffer/FinalLinear clearer even on empty layouts.
        UIRegistration::RegisterBuiltInControls();
        std::unique_ptr<UIElement> root;
        ASSERT_TRUE(
            XMLParser::ParseLayoutFromString(R"(<uielement id='root'></uielement>)", root));
        UIManager ui(dev.get());
        ui.SetRoot(std::move(root));
        ui.Update(0.0f, /*interactive=*/false);

        FramePools pools(dev.get());
        RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);
        const RenderGraph::RGTexture target =
            frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
        ui.RenderRG(frame, target, UI::UITargetSpace::LinearSdr());

        const RenderGraph::RGPassId pass = RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"UI Overlay"});
        ASSERT_NE(pass, RenderGraph::kInvalidId) << "zero primitives must still declare the clear";
        const RenderGraph::RGAttachmentRec* att = FindColorAttachment(frame, pass);
        ASSERT_NE(att, nullptr);
        EXPECT_EQ(att->Ops.Load, RenderGraph::RGLoadOp::Clear);

        frame.Execute();
        dev->WaitForIdle();
    }
    dev->Shutdown();
}

// Drain regression: a visual-only re-emit must seed opacity from the ancestor
// product, not the baked primitive value (which already includes self opacity
// and would compound per drain: 0.5 -> 0.25 -> 0.125).
TEST(UIManagerRenderRGTests, DrainDoesNotCompoundElementOpacity)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";
    {
        UIRegistration::RegisterBuiltInControls();
        std::unique_ptr<UIElement> root;
        ASSERT_TRUE(XMLParser::ParseLayoutFromString(
            R"(<uielement id='root'><uielement id='panel' /></uielement>)", root));
        auto ui = std::make_unique<UIManager>(dev.get());
        ui->SetRoot(std::move(root));

        const auto css = std::filesystem::temp_directory_path() / "ui_drain_opacity.css";
        {
            std::ofstream f(css);
            f << "#root { display: flex; width: 256px; height: 128px; }\n"
                 "#panel { width: 64px; height: 64px; background-color: #336699; opacity: 0.5; }\n";
        }
        ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));

        FramePools pools(dev.get());
        uint64_t frameIdx = 0;
        auto pump = [&]() {
            ui->Update(0.016f, /*interactive=*/false);
            RenderGraph::RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(++frameIdx);
            const RenderGraph::RGTexture target =
                frame.CreateTexture("Test.Target", MakeTargetDesc("Test.Target"));
            ui->RenderRG(frame, target, UI::UITargetSpace::LinearSdr());
            frame.Execute();
            dev->WaitForIdle();
        };
        // Settle: first frames run full regen + layout solves.
        for (int i = 0; i < 3; ++i)
            pump();

        UIElement* panel = ui->GetRootElement()->FindById("panel");
        ASSERT_NE(panel, nullptr);
        const UI::UIPrimitive* p = ui->PeekPrimitiveForTesting(*panel, 0);
        ASSERT_NE(p, nullptr);
        EXPECT_NEAR(p->Opacity, 0.5f, 1e-3f) << "baked opacity after full regen";

        // Two visual-only drains: opacity must stay at the element's own value.
        for (int i = 0; i < 2; ++i)
        {
            panel->MarkDirty(UIElement::VisualDirty);
            pump();
            p = ui->PeekPrimitiveForTesting(*panel, 0);
            ASSERT_NE(p, nullptr);
            EXPECT_NEAR(p->Opacity, 0.5f, 1e-3f) << "after drain " << (i + 1);
        }
    }
    dev->Shutdown();
}
