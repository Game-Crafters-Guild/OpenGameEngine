#pragma once

// A temp project whose model and material imports wait at a gate the test
// releases: a cold import still in flight. The engine is started on the
// project; each held decode waits at the gate (a watchdog releases it after
// 3 s, so a frame that waits for one ends and fails its timing check) and can
// be made to fail. Shared by the resolve tests: a fixture per file derives
// from it so each file keeps its own test suite.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/RenderServices.h"
#include "EngineTestShaderSetup.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Scripting/ScriptsConfig.h"
#include "TestDeviceHelper.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace GameEngine::Testing
{

// One triangle, its buffer embedded.
inline constexpr const char* kHeldTriangleGltf =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"name":"Tri"}],)"
    R"("meshes":[{"name":"Tri","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
    R"("buffers":[{"byteLength":44,"uri":"data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}]})";

// The smallest material document that parses.
inline constexpr const char* kHeldMinimalMaterial =
    R"({"schemaVersion": 3, "materialName": "HeldMaterial", "lightingModel": "StandardPBR"})";

// The release every held decode waits for, and how many decodes reached it.
class ImportGate
{
  public:
    ImportGate() : m_Gate(m_Release.get_future().share()) {}

    void Hold()
    {
        m_Entered.fetch_add(1, std::memory_order_release);
        m_Gate.wait();
    }

    void Release()
    {
        if (!m_Released.exchange(true))
            m_Release.set_value();
    }

    bool IsReleased() const { return m_Released.load(std::memory_order_acquire); }
    int Entered() const { return m_Entered.load(std::memory_order_acquire); }

    // True once `count` decodes have reached the gate, within two seconds.
    bool WaitForEntered(int count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (Entered() < count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return Entered() >= count;
    }

    // Releases the gate after `timeout` unless the test did first.
    bool WaitForRelease(std::chrono::seconds timeout) const
    {
        return m_Gate.wait_for(timeout) == std::future_status::ready;
    }

  private:
    std::promise<void> m_Release;
    std::shared_future<void> m_Gate;
    std::atomic<bool> m_Released{false};
    std::atomic<int> m_Entered{0};
};

// A model whose decode waits at the gate. A skinned one gains a skeleton once
// it has loaded, as a rigged import does.
class HeldModelAsset final : public ModelAsset
{
  public:
    HeldModelAsset(const GUID& guid, const std::filesystem::path& path, ImportGate& gate, bool fails, bool skinned)
        : ModelAsset(guid, path), m_Gate(gate), m_Fails(fails), m_Skinned(skinned)
    {
    }

    bool Load() override
    {
        m_Gate.Hold();
        return Finish(!m_Fails && ModelAsset::Load());
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        m_Gate.Hold();
        return Finish(!m_Fails && ModelAsset::LoadFromData(data));
    }

  private:
    bool Finish(bool loaded)
    {
        if (loaded && m_Skinned)
        {
            auto& store = Engine::Renderer::SkeletonStore::Instance();
            const uint32 skeleton = store.CreateSkeleton(3);
            store.Get(skeleton)->SkinJointCount = 3;
            store.Get(skeleton)->JointNodes = {0, 1, 2};
            SetSkeletonIdForTest(skeleton);
        }
        return loaded;
    }

    ImportGate& m_Gate;
    bool m_Fails = false;
    bool m_Skinned = false;
};

class HeldMaterialAsset final : public MaterialAsset
{
  public:
    HeldMaterialAsset(const GUID& guid, const std::filesystem::path& path, ImportGate& gate, bool fails)
        : MaterialAsset(guid, path), m_Gate(gate), m_Fails(fails)
    {
    }

    bool Load() override
    {
        m_Gate.Hold();
        return !m_Fails && MaterialAsset::Load();
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        m_Gate.Hold();
        return !m_Fails && MaterialAsset::LoadFromData(data);
    }

  private:
    ImportGate& m_Gate;
    bool m_Fails = false;
};

class HeldAssetImportFixture : public testing::Test
{
  protected:
    void SetUp() override
    {
        namespace fs = std::filesystem;
        m_Root = fs::temp_directory_path() / ("held-asset-import-" + GUID::Generate().ToString());
        fs::create_directories(m_Root / "Assets");
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore& engine = EngineCore::GetInstance();
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "HeldAssetImportTest";
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        ASSERT_TRUE(engine.Initialize(config));
        m_Initialized = true;
        // Asset types come from the type registry's factory: TearDown puts the engine's own back.
        ASSERT_TRUE(Types().TryGetAssetTypeRegistration(AssetType::Model, m_ModelRegistration));
        ASSERT_TRUE(Types().TryGetAssetTypeRegistration(AssetType::Material, m_MaterialRegistration));
        Rendering::Utils::SetShaderFileLoader(nullptr);
        Rendering::Utils::SetShaderPathResolver(&Testing::EngineTestShaderPathResolver);
        m_Watchdog = std::thread([this] {
            if (!m_Gate.WaitForRelease(std::chrono::seconds(3)))
                m_Gate.Release();
        });
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
        m_ServicesInitialized = true;
    }

    void TearDown() override
    {
        m_Gate.Release();
        if (m_Watchdog.joinable())
            m_Watchdog.join();
        EngineCore& engine = EngineCore::GetInstance();
        if (m_Initialized)
            engine.DisableRenderingLoop();
        if (m_ServicesInitialized)
            m_Services.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
        if (m_Initialized)
        {
            if (m_ModelOverridden)
            {
                Types().UnregisterAssetType(AssetType::Model);
                Types().RegisterAssetType(m_ModelRegistration);
            }
            if (m_MaterialOverridden)
            {
                Types().UnregisterAssetType(AssetType::Material);
                Types().RegisterAssetType(m_MaterialRegistration);
            }
            engine.Shutdown();
        }
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    static AssetTypeRegistry& Types()
    {
        return EngineCore::GetInstance().GetAssetManager().GetRegistry().GetTypeRegistry();
    }

    static AssetRegistry& Registry() { return EngineCore::GetInstance().GetAssetManager().GetRegistry(); }

    // Makes every model asset a held model (failing its load when `fails`,
    // gaining a skeleton when `skinned`) and registers a triangle glTF under
    // `fileName`; returns its GUID.
    GUID RegisterHeldModel(bool fails, bool skinned = false, const char* fileName = "hold_triangle.gltf")
    {
        if (!m_ModelOverridden)
        {
            Types().UnregisterAssetType(AssetType::Model);
            AssetTypeRegistration held = m_ModelRegistration;
            held.factory = [this, fails, skinned](const AssetMetadata& metadata) -> SharedPtr<Asset> {
                return std::make_shared<HeldModelAsset>(metadata.Guid, metadata.Path, m_Gate, fails, skinned);
            };
            EXPECT_TRUE(Types().RegisterAssetType(held));
            m_ModelOverridden = true;
        }
        return RegisterFile(fileName, kHeldTriangleGltf);
    }

    // Makes every material asset a held material and registers a minimal one
    // under `fileName`; returns its GUID.
    GUID RegisterHeldMaterial(bool fails, const char* fileName = "hold.material")
    {
        if (!m_MaterialOverridden)
        {
            Types().UnregisterAssetType(AssetType::Material);
            AssetTypeRegistration held = m_MaterialRegistration;
            held.factory = [this, fails](const AssetMetadata& metadata) -> SharedPtr<Asset> {
                return std::make_shared<HeldMaterialAsset>(metadata.Guid, metadata.Path, m_Gate, fails);
            };
            EXPECT_TRUE(Types().RegisterAssetType(held));
            m_MaterialOverridden = true;
        }
        return RegisterFile(fileName, kHeldMinimalMaterial);
    }

    GUID RegisterFile(const char* fileName, const char* contents)
    {
        const std::filesystem::path path = m_Root / "Assets" / fileName;
        {
            std::ofstream out(path, std::ios::binary);
            out << contents;
        }
        EXPECT_TRUE(Registry().RegisterAsset(path));
        return Registry().GetAssetGUID(path);
    }

    // The engine-managed rendering loop over this fixture's render services, the
    // frame path the Player runs; its world is the engine's primary world.
    ECS::World* EnableRenderingLoop()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.EnableRenderingLoop(&m_Services))
            return nullptr;
        return engine.GetPrimaryWorld();
    }

    // One frame of the engine's per-frame work this test exercises: the asset
    // manager's main-thread update, then the rendering loop. Returns its wall time.
    std::chrono::steady_clock::duration StepFrame()
    {
        const auto start = std::chrono::steady_clock::now();
        EngineCore::GetInstance().GetAssetManager().Update();
        EngineCore::GetInstance().StepRenderingLoop(1.0f / 60.0f);
        return std::chrono::steady_clock::now() - start;
    }

    std::filesystem::path m_Root;
    bool m_Initialized = false;
    AssetTypeRegistration m_ModelRegistration;
    AssetTypeRegistration m_MaterialRegistration;
    bool m_ModelOverridden = false;
    bool m_MaterialOverridden = false;
    ImportGate m_Gate;
    std::thread m_Watchdog;
    std::unique_ptr<Rendering::IDevice> m_Device;
    Engine::Renderer::RenderServices m_Services;
    bool m_ServicesInitialized = false;
};

// A frame that waited for an import: the gate's watchdog releases after 3 s,
// so a frame over this bound waited for a load.
inline constexpr std::chrono::milliseconds kAFrameThatWaited{1000};

} // namespace GameEngine::Testing
