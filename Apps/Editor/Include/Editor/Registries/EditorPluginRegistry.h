#pragma once

#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"
#include "PluginAPI/EnginePlugin.h"
#include "Platform/ContextMenu.h"
#include "Rendering/Core/Handle.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
struct EditorContext;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

struct HierarchyContextMenuContext
{
    ECS::World* World = nullptr;
    ECS::EntityHandle TargetEntity{};
    bool HasTargetEntity = false;
    const EditorContext* EditorCtx = nullptr;
};

struct HierarchyCommandContext
{
    ECS::World* World = nullptr;
    ECS::EntityHandle TargetEntity{};
    bool HasTargetEntity = false;
    const EditorContext* EditorCtx = nullptr;
    EditorChangeNotifications* ChangeNotifications = nullptr;
    UndoRedoService* Undo = nullptr;

    std::function<void(ECS::EntityHandle)> SelectEntity;
    std::function<void(ECS::EntityHandle)> NotifyEntityCreated;
    std::function<void()> NotifyWorldStructureChanged;
};

// One draw the scene-view selection/hover outline mask should render for a
// plugin-owned entity (runtime-generated meshes the generic MeshRenderer path
// cannot see). The plugin resolves everything itself against Engine.dll —
// mesh registry validity, textures (RenderServices), wind — and hands the
// editor pure data; the editor owns pipelines, palettes and submission.
struct SelectionMaskPart
{
    uint64_t MeshGpuHandleId = 0; // raw MeshGPUHandle id (0 = mesh not built)
    bool AllowSkinning = false;
    Rendering::TextureHandle AlphaTexture{}; // valid handle enables alpha-tested masking
    float AlphaCutoff = 0.5f;
    float UvScale[2] = {1.0f, 1.0f};
    float UvOffset[2] = {0.0f, 0.0f};
    float WindStrength[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float WindParams[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float WindSeed = 0.0f;
};

class IEditorPlugin
{
public:
    virtual ~IEditorPlugin() = default;

    virtual const Plugins::PluginDescriptor& GetDescriptor() const = 0;

    virtual void RegisterInspectors() {}
    virtual void BuildHierarchyContextMenu(ContextMenuBuilder& /*builder*/,
                                           const HierarchyContextMenuContext& /*context*/) {}
    virtual bool HandleHierarchyCommand(uint32_t /*commandId*/,
                                        const HierarchyCommandContext& /*context*/)
    {
        return false;
    }
    // Selection/hover outline contribution for entities whose rendered meshes
    // are plugin-generated. Append zero or more parts; entities the plugin
    // does not own must be left untouched.
    // The outline dispatch runs per hovered/selected entity, so it is
    // capability-gated: a plugin that implements CollectSelectionMaskParts
    // must also return true here or it is never consulted.
    virtual bool ContributesSelectionMask() const { return false; }
    virtual void CollectSelectionMaskParts(ECS::World& /*world*/,
                                           ECS::EntityHandle /*entity*/,
                                           std::vector<SelectionMaskPart>& /*outParts*/) {}
};

class EditorPluginRegistry
{
public:
    using EnabledResolver = std::function<bool(std::string_view pluginId, bool defaultEnabled)>;

    static EditorPluginRegistry& Get();

    void RegisterPlugin(IEditorPlugin& plugin);
    std::vector<IEditorPlugin*> GetPlugins() const;
    std::vector<IEditorPlugin*> GetEnabledPlugins() const;

    void SetEnabledResolver(EnabledResolver resolver);
    bool IsEnabled(const Plugins::PluginDescriptor& descriptor) const;
    bool IsEnabled(std::string_view pluginId, bool defaultEnabled = true) const;

    // Drops the memoized per-plugin enabled states and the derived selection
    // mask contributor list. Call when the answers' source changes — project
    // open (enablement lives in the project's settings file) or a future
    // enablement toggle. Resolution is otherwise cached: the resolver reads
    // settings from disk, which must never happen on per-frame paths.
    void InvalidateEnabledCache();

    void RegisterInspectors();
    void BuildHierarchyContextMenu(ContextMenuBuilder& builder,
                                   const HierarchyContextMenuContext& context);
    bool HandleHierarchyCommand(uint32_t commandId,
                                const HierarchyCommandContext& context);
    void CollectSelectionMaskParts(ECS::World& world,
                                   ECS::EntityHandle entity,
                                   std::vector<SelectionMaskPart>& outParts);

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every plugin attributed to `moduleId` (the plugin object and all of its
    // hook code live in the module's image and pin it mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorPluginRegistry() = default;

    // Registration + the module stamp active when it was made (mirrors
    // Plugins::EnginePluginRegistry::Entry).
    struct Entry
    {
        IEditorPlugin* Plugin = nullptr;
        ECS::ModuleRegistrationStamp Module;
    };

    const std::vector<IEditorPlugin*>& MaskContributors() const;

    std::vector<Entry> m_Plugins;
    EnabledResolver m_EnabledResolver;
    // Memoized IsEnabled answers keyed by plugin id, plus the derived
    // enabled-and-contributing list the per-entity outline dispatch iterates.
    // mutable: filled lazily from const accessors on the main thread — the
    // same single-thread contract as registration.
    mutable std::map<std::string, bool, std::less<>> m_EnabledCache;
    mutable std::vector<IEditorPlugin*> m_MaskContributors;
    mutable bool m_MaskContributorsDirty = true;
    // Editor-kind package modules load after the editor's one-time inspector
    // registration pass; once that pass has run, late registrations replay it
    // for themselves. Registrations are main-thread (module loads + editor
    // startup), matching the rest of this registry.
    bool m_InspectorsRegistered = false;
};

} // namespace GameEngine::Editor
