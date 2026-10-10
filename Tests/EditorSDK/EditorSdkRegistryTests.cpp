// EditorSDK phase 1 registry semantics — the contracts editor-kind package
// modules rely on:
//   * RegisterPlugin REPLACES a same-id registration (module hot-reload is
//     replace-forward; the old DLL stays mapped so the swap is safe),
//   * plugins registering AFTER the editor's one-time inspector pass get
//     that pass replayed for them (packages load at project open),
//   * component traits / pick providers use replace-by-typeId registration,
//   * selection-mask collection dispatches to enabled plugins.
//
// Links EditorSDK.dll — the same registries Editor.exe and module DLLs share.

#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "InspectorRegistry.h"
#include "Picking/EditorPickProviders.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;
namespace ecs = GameEngine::ECS;

namespace
{

class TestPlugin final : public ed::IEditorPlugin
{
public:
    TestPlugin(const char* id, GameEngine::ECS::ComponentTypeId inspectorTypeId)
        : m_InspectorTypeId(inspectorTypeId)
    {
        m_Descriptor = {id, "EditorSDK Test Plugin", "1.0.0", true};
    }

    const GameEngine::Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }

    void RegisterInspectors() override
    {
        ++InspectorPassCount;
        GameEngine::InspectorRegistry::Get().RegisterComponentInspectorByTypeId(
            m_InspectorTypeId, [](const GameEngine::InspectorContext&) {});
    }

    bool ContributesSelectionMask() const override { return true; }

    void CollectSelectionMaskParts(ecs::World&, ecs::EntityHandle,
                                   std::vector<ed::SelectionMaskPart>& outParts) override
    {
        ed::SelectionMaskPart part{};
        part.MeshGpuHandleId = MaskMeshId;
        outParts.push_back(part);
    }

    int InspectorPassCount = 0;
    uint64_t MaskMeshId = 0x1111u;

private:
    GameEngine::Plugins::PluginDescriptor m_Descriptor{};
    GameEngine::ECS::ComponentTypeId m_InspectorTypeId = 0;
};

bool ContainsPlugin(const std::vector<ed::IEditorPlugin*>& plugins, const ed::IEditorPlugin* plugin)
{
    return std::find(plugins.begin(), plugins.end(), plugin) != plugins.end();
}

} // namespace

TEST(EditorPluginRegistry, SameIdRegistrationReplacesForHotReload)
{
    auto& registry = ed::EditorPluginRegistry::Get();
    static TestPlugin first("sdkTest.replace", 0xE001);
    static TestPlugin second("sdkTest.replace", 0xE001);

    registry.RegisterPlugin(first);
    const std::size_t countAfterFirst = registry.GetPlugins().size();
    registry.RegisterPlugin(second);

    const std::vector<ed::IEditorPlugin*> plugins = registry.GetPlugins();
    EXPECT_EQ(plugins.size(), countAfterFirst) << "same-id re-registration must not grow the registry";
    EXPECT_FALSE(ContainsPlugin(plugins, &first)) << "the old instance must stop being dispatched";
    EXPECT_TRUE(ContainsPlugin(plugins, &second));
}

TEST(EditorPluginRegistry, LateRegistrationReplaysInspectorPass)
{
    auto& registry = ed::EditorPluginRegistry::Get();

    // The editor's one-time startup pass runs first; a package module
    // registering afterwards (project open) must not miss it.
    registry.RegisterInspectors();

    static TestPlugin late("sdkTest.lateReplay", 0xE002);
    ASSERT_EQ(late.InspectorPassCount, 0);
    registry.RegisterPlugin(late);
    EXPECT_EQ(late.InspectorPassCount, 1) << "inspector pass must replay for late plugins";
    EXPECT_NE(GameEngine::InspectorRegistry::Get().TryGetComponentInspector(0xE002), nullptr);
}

TEST(EditorComponentTraits, RegistrationReplacesByTypeId)
{
    auto& registry = ed::EditorComponentTraitsRegistry::Get();
    const GameEngine::ECS::ComponentTypeId typeId = 0xE003;

    ed::EditorComponentTraits first;
    first.DisplayName = "First";
    registry.Register(typeId, first);

    ed::EditorComponentTraits second;
    second.DisplayName = "Second";
    second.HierarchyRowClass = "hierarchy-entity-test";
    second.IsPickInstanceRoot = true;
    registry.Register(typeId, second);

    ed::EditorComponentTraits out;
    ASSERT_TRUE(registry.TryGet(typeId, out));
    EXPECT_EQ(out.DisplayName, "Second");
    EXPECT_EQ(out.HierarchyRowClass, "hierarchy-entity-test");
    EXPECT_TRUE(out.IsPickInstanceRoot);

    EXPECT_FALSE(registry.TryGet(0xDEAD, out));
}

TEST(EditorComponentTraits, SectionHostIsFoundByTheSectionsItHosts)
{
    auto& registry = ed::EditorComponentTraitsRegistry::Get();
    const GameEngine::ECS::ComponentTypeId hostTypeId = 0xE010;
    const GameEngine::ECS::ComponentTypeId hostedTypeId = 0xE011;

    ed::EditorComponentTraits host;
    host.HostsInspectorSection = [hostedTypeId](GameEngine::ECS::ComponentTypeId typeId)
    { return typeId == hostedTypeId; };
    host.MissingHostBadge = "No Host";
    registry.Register(hostTypeId, host);

    GameEngine::ECS::ComponentTypeId foundHost{};
    ed::EditorComponentTraits foundTraits;
    ASSERT_TRUE(registry.TryGetSectionHost(hostedTypeId, foundHost, foundTraits));
    EXPECT_EQ(foundHost, hostTypeId);
    EXPECT_EQ(foundTraits.MissingHostBadge, "No Host");

    EXPECT_FALSE(registry.TryGetSectionHost(hostTypeId, foundHost, foundTraits));
}

TEST(EditorComponentTraits, SecondHostOfARegisteredSectionIsRefused)
{
    auto& registry = ed::EditorComponentTraitsRegistry::Get();
    const ecs::ComponentTypeId hostedTypeId =
        ecs::ComponentRegistry::RegisterBlobComponent("EditorSdkTestHostedEffect", sizeof(int));
    const ecs::ComponentTypeId firstHostTypeId = 0xE012;
    const ecs::ComponentTypeId secondHostTypeId = 0xE013;

    ed::EditorComponentTraits host;
    host.HostsInspectorSection = [hostedTypeId](ecs::ComponentTypeId typeId) { return typeId == hostedTypeId; };
    registry.Register(firstHostTypeId, host);
    registry.Register(secondHostTypeId, host);

    ed::EditorComponentTraits out;
    EXPECT_FALSE(registry.TryGet(secondHostTypeId, out));
    ecs::ComponentTypeId foundHost{};
    ASSERT_TRUE(registry.TryGetSectionHost(hostedTypeId, foundHost, out));
    EXPECT_EQ(foundHost, firstHostTypeId);

    // Re-registering the first host replaces it rather than colliding with itself.
    registry.Register(firstHostTypeId, host);
    EXPECT_TRUE(registry.TryGet(firstHostTypeId, out));

    registry.Register(firstHostTypeId, ed::EditorComponentTraits{});
    ecs::ComponentRegistry::UnregisterComponentForTests(hostedTypeId);
}

TEST(EditorPickProviders, RegistrationReplacesAndSnapshotIterates)
{
    auto& registry = ed::Picking::EditorPickProviderRegistry::Get();
    const GameEngine::ECS::ComponentTypeId typeId = 0xE004;

    ed::Picking::EditorPickProvider first;
    first.Resolve = [](ecs::World&, ecs::EntityHandle, ed::Picking::SyntheticMeshPick& out) {
        out.MeshGpuHandleId = 1u;
        return true;
    };
    registry.Register(typeId, first);

    ed::Picking::EditorPickProvider second;
    second.Resolve = [](ecs::World&, ecs::EntityHandle, ed::Picking::SyntheticMeshPick& out) {
        out.MeshGpuHandleId = 2u;
        return true;
    };
    registry.Register(typeId, second);

    // The snapshot carries the REPLACED provider (2), never both.
    ecs::World world;
    bool sawReplacement = false;
    for (const ed::Picking::EditorPickProvider& provider : registry.Snapshot())
    {
        ed::Picking::SyntheticMeshPick pick;
        if (provider.Resolve && provider.Resolve(world, ecs::EntityHandle{}, pick) &&
            pick.MeshGpuHandleId == 2u)
            sawReplacement = true;
        EXPECT_NE(pick.MeshGpuHandleId, 1u) << "replaced provider must not survive";
    }
    EXPECT_TRUE(sawReplacement);
}

TEST(EditorPluginRegistry, SelectionMaskPartsDispatchToEnabledPlugins)
{
    auto& registry = ed::EditorPluginRegistry::Get();
    static TestPlugin masker("sdkTest.mask", 0xE005);
    masker.MaskMeshId = 0xABCDu;
    registry.RegisterPlugin(masker);

    ecs::World world;
    std::vector<ed::SelectionMaskPart> parts;
    registry.CollectSelectionMaskParts(world, ecs::EntityHandle{}, parts);

    const bool found = std::any_of(parts.begin(), parts.end(), [](const ed::SelectionMaskPart& p) {
        return p.MeshGpuHandleId == 0xABCDu;
    });
    EXPECT_TRUE(found);
}

TEST(EditorPluginRegistry, SelectionMaskPartsSkipInactiveEntities)
{
    auto& registry = ed::EditorPluginRegistry::Get();
    static TestPlugin masker("sdkTest.maskDisabled", 0xE006);
    masker.MaskMeshId = 0xD15Bu;
    registry.RegisterPlugin(masker);

    // Disabled entities render nothing, so the dispatch must produce no mask
    // parts at all — plugins are never consulted.
    ecs::World world;
    const ecs::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, ecs::Disabled{});

    std::vector<ed::SelectionMaskPart> parts;
    registry.CollectSelectionMaskParts(world, entity, parts);
    EXPECT_TRUE(parts.empty()) << "disabled entity must contribute no selection mask parts";

    // Re-enabling restores the contribution.
    world.RemoveComponentImmediate<ecs::Disabled>(entity);
    registry.CollectSelectionMaskParts(world, entity, parts);
    const bool found = std::any_of(parts.begin(), parts.end(), [](const ed::SelectionMaskPart& p) {
        return p.MeshGpuHandleId == 0xD15Bu;
    });
    EXPECT_TRUE(found);

    // Under a switched-off parent the entity carries the tag the hierarchy pass
    // writes, and contributes nothing either.
    world.AddComponentImmediate(entity, ecs::DisabledInHierarchy{});
    parts.clear();
    registry.CollectSelectionMaskParts(world, entity, parts);
    EXPECT_TRUE(parts.empty()) << "an entity inactive through its parent must contribute no selection mask parts";
}

namespace
{

// Overrides the mask hook WITHOUT declaring the capability — the registry
// must never consult it. Encodes the ContributesSelectionMask contract.
class NonContributingPlugin final : public ed::IEditorPlugin
{
public:
    explicit NonContributingPlugin(const char* id) { m_Descriptor = {id, "No Capability", "1.0.0", true}; }

    const GameEngine::Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }

    void CollectSelectionMaskParts(ecs::World&, ecs::EntityHandle,
                                   std::vector<ed::SelectionMaskPart>& outParts) override
    {
        ++CollectCalls;
        ed::SelectionMaskPart part{};
        part.MeshGpuHandleId = 0xBAD0u;
        outParts.push_back(part);
    }

    int CollectCalls = 0;

private:
    GameEngine::Plugins::PluginDescriptor m_Descriptor{};
};

} // namespace

TEST(EditorPluginRegistry, SelectionMaskSkipsPluginsWithoutCapability)
{
    auto& registry = ed::EditorPluginRegistry::Get();
    static NonContributingPlugin bystander("sdkTest.noCapability");
    registry.RegisterPlugin(bystander);

    ecs::World world;
    std::vector<ed::SelectionMaskPart> parts;
    registry.CollectSelectionMaskParts(world, ecs::EntityHandle{}, parts);

    EXPECT_EQ(bystander.CollectCalls, 0) << "no ContributesSelectionMask => never dispatched";
    EXPECT_TRUE(std::none_of(parts.begin(), parts.end(), [](const ed::SelectionMaskPart& p) {
        return p.MeshGpuHandleId == 0xBAD0u;
    }));
}

TEST(EditorPluginRegistry, EnabledResolutionIsCachedUntilInvalidated)
{
    auto& registry = ed::EditorPluginRegistry::Get();
    static TestPlugin masker("sdkTest.cachedEnable", 0xE007);
    registry.RegisterPlugin(masker);

    // Counting resolver stands in for the real one, which reads the project
    // settings file from disk — the cost this cache exists to bound.
    static std::map<std::string, int, std::less<>> s_ResolveCounts;
    s_ResolveCounts.clear();
    registry.SetEnabledResolver([](std::string_view id, bool defaultEnabled) {
        ++s_ResolveCounts[std::string(id)];
        return defaultEnabled;
    });

    ecs::World world;
    std::vector<ed::SelectionMaskPart> parts;
    for (int i = 0; i < 64; ++i)
        registry.CollectSelectionMaskParts(world, ecs::EntityHandle{}, parts);
    EXPECT_EQ(s_ResolveCounts["sdkTest.cachedEnable"], 1)
        << "64 per-entity dispatches must resolve enablement exactly once";

    registry.InvalidateEnabledCache();
    registry.CollectSelectionMaskParts(world, ecs::EntityHandle{}, parts);
    EXPECT_EQ(s_ResolveCounts["sdkTest.cachedEnable"], 2)
        << "invalidation must re-resolve on the next dispatch";

    // Restore the default resolver state so later tests see default-enabled
    // semantics (the registry is process-global).
    registry.SetEnabledResolver(nullptr);
}
