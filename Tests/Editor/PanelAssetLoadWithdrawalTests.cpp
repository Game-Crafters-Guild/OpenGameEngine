// A panel destroyed while its asset load is still in flight must not be called back.
//
// Every panel that binds its .uxml/.css asynchronously registers a completion callback
// that captures the panel. Destroying an AssetLoadHandle withdraws nothing, so the panel's
// destructor has to Cancel() the handle itself; a panel that forgets is reached by the
// callback after it is freed: on the worker that completes the load through
// this->PostAction, and again inline from AssetManager::Shutdown, which fires the failure
// callback of every decode it cancels.
//
// The fixture holds every .uxml/.css decode inside the parser, so a panel can be destroyed
// while its load is provably in flight, then releases the decode and reads the owning
// manager's dispatcher: a panel the callback still reached posts its bind continuation
// there. The positive control shows exactly that post for a panel left alive, so a zero for
// the destroyed panel is the withdrawal, not a bind that never started.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "Assets/Parsers/UILayoutAssetParser.h"
#include "Assets/Parsers/UIStyleAssetParser.h"
#include "Core/Engine.h"
#include "Panels/WebPanel.h"
#include "TestTempDir.h"
#include "UI/AssetBoundDockPanel.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using GameEngine::ApplicationConfig;
using GameEngine::AssetManager;
using GameEngine::EngineCore;
using GameEngine::UILayoutAccess;
using GameEngine::UIManager;
using GameEngine::WebPanel;

namespace
{
// Holds every .uxml/.css parse inside the decode job until the test releases the gate,
// then delegates to the engine's own UI parsers so the load succeeds and nothing is
// suppressed for the next test. Registered above the built-in parsers by priority.
class HeldUiAssetParser final : public GameEngine::AssetParser
{
  public:
    GameEngine::AssetType GetAssetType() const override { return GameEngine::AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".uxml", ".css"}; }
    int GetPriority() const override { return 1000; }
    std::string GetName() const override { return "HeldUiAssetParser"; }

    GameEngine::AssetParseResult Parse(const GameEngine::AssetMetadata& metadata,
                                       AssetManager& assetManager) override
    {
        std::shared_future<void> gate;
        {
            std::lock_guard lock(m_Mutex);
            gate = m_Gate;
        }
        m_Entered.fetch_add(1, std::memory_order_release);
        if (gate.valid())
            gate.wait();

        std::string extension = metadata.Path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".css")
            return m_Style.Parse(metadata, assetManager);
        return m_Layout.Parse(metadata, assetManager);
    }

    /// Every parse entered from now on blocks until `gate` is ready.
    void Arm(std::shared_future<void> gate)
    {
        std::lock_guard lock(m_Mutex);
        m_Gate = std::move(gate);
        m_Entered.store(0, std::memory_order_release);
    }

    /// Parses that have entered (and blocked) since the last Arm.
    int Entered() const { return m_Entered.load(std::memory_order_acquire); }

  private:
    std::mutex m_Mutex;
    std::shared_future<void> m_Gate;
    std::atomic<int> m_Entered{0};
    GameEngine::UILayoutAssetParser m_Layout;
    GameEngine::UIStyleAssetParser m_Style;
};

bool WaitUntil(const std::function<bool()>& condition,
               std::chrono::seconds budget = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Receives the deferred bind the panel posts from OnPostLayout. UIElement::PostAction
// prefers the thread-local UI context's dispatcher, so installing one of these through
// UiContextScope intercepts the main-thread posts; the worker that completes a load has no
// such context and posts to the owning manager's dispatcher instead, which is the one the
// tests read.
class CountingDispatcher final : public GameEngine::UI::IUiDispatcher
{
  public:
    bool Post(std::function<void()> fn) override
    {
        ++m_PostCount;
        m_Queue.push_back(std::move(fn));
        return true;
    }

    void Drain() override
    {
        std::vector<std::function<void()>> batch;
        batch.swap(m_Queue);
        for (auto& fn : batch)
            if (fn)
                fn();
    }

    size_t PendingCount() const override { return m_Queue.size(); }
    size_t PostCount() const { return m_PostCount; }

  private:
    std::vector<std::function<void()>> m_Queue;
    size_t m_PostCount = 0;
};

// AssetBoundDockPanel's constructor is protected; this names the fixture's editor source.
// Each test binds its own layout file: a layout one test loaded is resident for the next,
// whose load would then resolve inline instead of entering the held decode.
template <const char* Stem>
class ProbeAssetBoundPanel final : public GameEngine::Editor::AssetBoundDockPanel
{
  public:
    ProbeAssetBoundPanel()
        : AssetBoundDockPanel("Probe", std::string(GameEngine::kAssetSourceAliasEditor),
                              std::filesystem::path("UI/panels") / (std::string(Stem) + ".uxml"),
                              std::filesystem::path("UI/panels") / (std::string(Stem) + ".css"))
    {
    }
};

constexpr char kLivePanelStem[] = "ProbePanelLive";
constexpr char kDestroyedPanelStem[] = "ProbePanelDestroyed";

class PanelAssetLoadWithdrawalTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }

        // The panels resolve their assets through the editor source alias; the headless
        // engine mounts none, so the suite mounts a temporary one holding the files.
        s_Root = GameEngine::TestUtils::MakeUniqueTempDirectory("panel_asset_load_withdrawal");
        const auto panels = s_Root / "UI" / "panels";
        std::filesystem::create_directories(panels);
        for (const char* name : {"WebPanel", kLivePanelStem, kDestroyedPanelStem})
        {
            std::ofstream layout(panels / (std::string(name) + ".uxml"), std::ios::binary);
            layout << "<?xml version=\"1.0\"?>\n<UI id=\"root\"><Button id=\"b\" text=\"Test\"/></UI>\n";
            std::ofstream style(panels / (std::string(name) + ".css"), std::ios::binary);
            style << "button { color: red; }\n";
        }

        AssetManager& assets = engine.GetAssetManager();
        GameEngine::AssetSourceDesc source{};
        source.Alias = std::string(GameEngine::kAssetSourceAliasEditor);
        source.Root = s_Root;
        ASSERT_TRUE(assets.RegisterSource(source)) << "the editor source alias was already mounted";
        assets.WaitForStartupScan(GameEngine::kAssetSourceAliasEditor);

        s_Parser = std::make_shared<HeldUiAssetParser>();
        ASSERT_TRUE(assets.GetParserRegistry().RegisterParser(s_Parser, s_Parser->GetPriority()));
    }

    static void TearDownTestSuite()
    {
        AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
        if (s_Parser)
            assets.GetParserRegistry().UnregisterParser(s_Parser);
        s_Parser.reset();
        std::error_code ec;
        std::filesystem::remove_all(s_Root, ec);
    }

    static AssetManager& Assets() { return EngineCore::GetInstance().GetAssetManager(); }

    // Arms the parser, mounts a panel under its own UIManager, and drives the first laid-out
    // frame so the panel schedules its bind and the bind starts its load.
    template <typename PanelT>
    struct ArmedPanel
    {
        std::promise<void> Release;
        CountingDispatcher Deferred;
        GameEngine::UI::UiContextScope Scope{&Deferred, nullptr};
        UIManager Manager{nullptr};
        PanelT* Panel = nullptr;

        explicit ArmedPanel(HeldUiAssetParser& parser)
        {
            parser.Arm(Release.get_future().share());
            auto owned = std::make_unique<PanelT>();
            Panel = owned.get();
            Manager.SetRoot(std::move(owned));
            UILayoutAccess::SetLastLayoutRect(*Panel, 0.0f, 0.0f, 320.0f, 240.0f);
        }

        void StartLoad(HeldUiAssetParser& parser)
        {
            Panel->OnPostLayout();
            ASSERT_EQ(Deferred.PostCount(), 1u) << "the layout pass scheduled no bind, so this test proves nothing";
            Deferred.Drain();
            ASSERT_TRUE(WaitUntil([&] { return parser.Entered() >= 1; })) << "the bind never reached its decode";
            ASSERT_GT(Assets().GetInFlightLoadCount(), 0u) << "the load is not in flight";
        }
    };

    template <typename PanelT>
    void DestroyingThePanelWithdrawsItsLoad()
    {
        ArmedPanel<PanelT> armed(*s_Parser);
        armed.StartLoad(*s_Parser);
        if (HasFatalFailure())
            return;

        armed.Manager.SetRoot(nullptr); // destroys the panel while its load is held
        armed.Release.set_value();
        ASSERT_TRUE(WaitUntil([] { return Assets().GetInFlightLoadCount() == 0; }))
            << "the held load never completed";

        ASSERT_NE(armed.Manager.GetDispatcher(), nullptr);
        EXPECT_EQ(armed.Manager.GetDispatcher()->PendingCount(), 0u)
            << "the completion callback reached the destroyed panel and posted its continuation";
        EXPECT_EQ(armed.Deferred.PendingCount(), 0u);
    }

    static inline std::filesystem::path s_Root;
    static inline std::shared_ptr<HeldUiAssetParser> s_Parser;
};
} // namespace

// Positive control for the instrument: a panel left alive is reached by its callback and
// posts the bind continuation to its manager. This is the post the withdrawal tests expect
// not to see.
TEST_F(PanelAssetLoadWithdrawalTests, LivePanelReceivesItsContinuation)
{
    ArmedPanel<ProbeAssetBoundPanel<kLivePanelStem>> armed(*s_Parser);
    armed.StartLoad(*s_Parser);
    if (HasFatalFailure())
        return;

    armed.Release.set_value();
    ASSERT_TRUE(WaitUntil([] { return Assets().GetInFlightLoadCount() == 0; }));

    ASSERT_NE(armed.Manager.GetDispatcher(), nullptr);
    EXPECT_EQ(armed.Manager.GetDispatcher()->PendingCount(), 1u)
        << "the live panel's callback posted nothing: the instrument would not see a dangling callback either";

    // Run the chain to its end while the panel is alive: the layout continuation starts the
    // stylesheet load, whose continuation settles the bind. A continuation left queued
    // would run against a destroyed panel when the manager drains on destruction.
    for (int step = 0; step < 4 && armed.Manager.GetDispatcher()->PendingCount() > 0; ++step)
    {
        armed.Manager.GetDispatcher()->Drain();
        ASSERT_TRUE(WaitUntil([] { return Assets().GetInFlightLoadCount() == 0; }));
    }
    EXPECT_EQ(armed.Manager.GetDispatcher()->PendingCount(), 0u);
}

TEST_F(PanelAssetLoadWithdrawalTests, AssetBoundDockPanelDestroyedMidLoadIsNotCalledBack)
{
    DestroyingThePanelWithdrawsItsLoad<ProbeAssetBoundPanel<kDestroyedPanelStem>>();
}

TEST_F(PanelAssetLoadWithdrawalTests, WebPanelDestroyedMidLoadIsNotCalledBack)
{
    DestroyingThePanelWithdrawsItsLoad<WebPanel>();
}
