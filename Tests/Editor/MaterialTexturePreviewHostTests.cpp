// The material inspector's texture preview retries once its texture registers. The registration's
// asset event arrives on the thread that raised it (a scan worker or the file watcher), so the
// preview must never be touched there: the retry is posted to the owning UI manager's dispatcher and
// runs on the UI thread, and an event that arrives before the preview is attached is delivered once
// it is.

#include <gtest/gtest.h>

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "UI/MaterialTexturePreviewHost.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <memory>
#include <thread>

using namespace GameEngine;

namespace
{

// The asset manager the preview subscribes through; started here so the suite does not depend on
// another suite having started the engine.
EngineCore& StartedEngine()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }
    return engine;
}

// Raises the event the registration of `texture` raises, from a thread of its own.
void RaiseTextureCreatedOnAWorker(EngineCore& engine, const GUID& texture)
{
    std::thread worker([&] {
        engine.GetAssetManager().GetEventDispatcher().DispatchEvent(
            AssetEvent(AssetEventType::AssetCreated, texture, AssetType::Texture, "texture.png"));
    });
    worker.join();
}

} // namespace

TEST(MaterialTexturePreviewHostTests, AWorkerThreadEventPostsTheRetryToTheOwnersDispatcher)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();

    UIManager manager{nullptr};
    auto root = std::make_unique<UIElement>();
    auto hostOwned = std::make_unique<MaterialTexturePreviewHost>();
    MaterialTexturePreviewHost* host = hostOwned.get();
    root->AddChild(std::move(hostOwned));
    manager.SetRoot(std::move(root));
    ASSERT_NE(manager.GetDispatcher(), nullptr);
    manager.GetDispatcher()->Drain();

    const GUID unregistered = GUID::Generate();
    host->SetTexturePreviewFromGuid(registry, unregistered);
    ASSERT_TRUE(host->HasClass("texture-preview-empty"));

    RaiseTextureCreatedOnAWorker(engine, unregistered);
    EXPECT_EQ(manager.GetDispatcher()->PendingCount(), 1u) << "the retry was not posted to the owner's dispatcher";
    manager.GetDispatcher()->Drain();
    EXPECT_TRUE(host->HasClass("texture-preview-empty")) << "the texture is still unregistered";

    RaiseTextureCreatedOnAWorker(engine, GUID::Generate());
    EXPECT_EQ(manager.GetDispatcher()->PendingCount(), 0u) << "an event for another texture posted a retry";

    // A retry queued for a preview that is then destroyed runs as a no-op.
    RaiseTextureCreatedOnAWorker(engine, unregistered);
    EXPECT_EQ(manager.GetDispatcher()->PendingCount(), 1u);
    manager.SetRoot(std::make_unique<UIElement>());
    manager.GetDispatcher()->Drain();
}

TEST(MaterialTexturePreviewHostTests, AnEventBeforeThePreviewIsAttachedIsDeliveredOnceItIs)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();

    // The inspector subscribes the preview before it attaches it to the panel.
    auto hostOwned = std::make_unique<MaterialTexturePreviewHost>();
    MaterialTexturePreviewHost* host = hostOwned.get();
    const GUID unregistered = GUID::Generate();
    host->SetTexturePreviewFromGuid(registry, unregistered);
    RaiseTextureCreatedOnAWorker(engine, unregistered);

    UIManager manager{nullptr};
    ASSERT_NE(manager.GetDispatcher(), nullptr);
    manager.GetDispatcher()->Drain();
    auto root = std::make_unique<UIElement>();
    root->AddChild(std::move(hostOwned));
    manager.SetRoot(std::move(root));
    EXPECT_EQ(manager.GetDispatcher()->PendingCount(), 1u)
        << "the event that arrived before the preview had a UI manager was dropped";
    manager.GetDispatcher()->Drain();
    EXPECT_TRUE(host->HasClass("texture-preview-empty"));
    manager.SetRoot(std::make_unique<UIElement>());
    manager.GetDispatcher()->Drain();
}
