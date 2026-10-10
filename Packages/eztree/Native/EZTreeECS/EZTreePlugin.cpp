#include "EZTreeECS/EZTreePlugin.h"

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentRegistry.h"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "EZTreeECS/EZTreeService.h"
#include "EZTreeECS/Systems/RegisterEZTreeSystems.h"
#include "PluginAPI/EnginePlugin.h"

namespace GameEngine::EZTreeECS
{
namespace
{

class EZTreeEnginePlugin final : public Plugins::IEnginePlugin
{
public:
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor descriptor{
            EZTree::kPluginId.data(),
            "Tree Generator",
            EZTree::kUpstreamVersion.data(),
            true,
        };
        return descriptor;
    }

    void RegisterEngineComponents() override
    {
        ECS::ComponentRegistry::RegisterComponent<Components::EZTree>("EZTree");

        // Editor Add Component menu discoverability: record default bytes +
        // an addable creator (title/icon flow from the editor traits
        // registry). Runs again on every module reload; ComponentFactory
        // replaces by type id, so re-registration never duplicates the entry.
        static_assert(std::is_trivially_copyable_v<Components::EZTree>,
                      "EZTree default-bytes creator requires a trivially copyable component");
        const Components::EZTree defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(ECS::GetComponentTypeId<Components::EZTree>(),
                                                    &defaults, sizeof(defaults), /*addable=*/true);
    }

    void RegisterSceneSchemas() override;

    void AddSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                              Plugins::EnginePluginContext& context) override
    {
        AddEZTreeSystemsToSchedule(builder, context.RenderServices);
    }

    void OnRuntimeInitialized(Plugins::EnginePluginContext& context) override
    {
        EZTreeService::Get().Initialize(context.RenderServices);
    }

    void OnRuntimeShutdown() override
    {
        EZTreeService::Get().Shutdown();
    }
};

EZTreeEnginePlugin& GetPlugin()
{
    static EZTreeEnginePlugin plugin;
    return plugin;
}

} // namespace

void RegisterEZTreeSceneSchemas();

void EZTreeEnginePlugin::RegisterSceneSchemas()
{
    RegisterEZTreeSceneSchemas();
}

void RegisterEZTreeEnginePlugin()
{
    Plugins::EnginePluginRegistry::Get().RegisterPlugin(GetPlugin());
}

} // namespace GameEngine::EZTreeECS
