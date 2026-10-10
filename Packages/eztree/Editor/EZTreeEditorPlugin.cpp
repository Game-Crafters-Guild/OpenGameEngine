// Editor half of the Tree Generator, shipped as the eztree package's native
// Editor-kind module (EditorSDK phase 1). Links the Engine + EditorSDK import
// libs; everything it registers goes through the shared editor registries:
// the component inspector (InspectorRegistry), hierarchy menu + create
// command (EditorPluginRegistry), component traits (names/icons/pick-root/
// pivot/enabled), the scene-view pick provider, and selection-mask parts.

#include "EZTreeEditorPlugin.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "EZTreeStatsPanel.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WindVolumeResolver.h"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "EZTreeECS/EZTreeDefaultTextures.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Picking/EditorPickProviders.h"
#include "PluginAPI/EnginePlugin.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/UIManager.h"
#include "UI/UIManagerRef.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace GameEngine::EZTreeEditor
{
namespace
{

constexpr uint32 kCreateEZTreeCommand = 0x6501u;
constexpr uint32 kShowTreeStatsCommand = 0x6502u;
constexpr const char* kTreeGeneratorIcon = "editor:Icons/leaf.svg";

Components::Name MakeName(const char* text)
{
    Components::Name n{};
    std::strncpy(n.value, text, sizeof(n.value) - 1u);
    return n;
}

Components::HierarchyOrder NextOrder(ECS::World* world)
{
    Components::HierarchyOrder order{};
    if (!world)
        return order;
    std::vector<ECS::EntityHandle> alive;
    world->GetAliveEntitiesSnapshot(alive);
    for (const auto& e : alive)
    {
        if (const auto* existing = world->GetComponent<Components::HierarchyOrder>(e))
            order.order = std::max(order.order, existing->order + 1);
    }
    return order;
}

GUID GuidFromArray(const std::array<uint8, 16>& bytes)
{
    GUID::Data data{};
    std::copy(bytes.begin(), bytes.end(), data.begin());
    return GUID(data);
}

void WriteGuidToArray(const GUID& guid, std::array<uint8, 16>& out)
{
    std::copy(guid.GetData().begin(), guid.GetData().end(), out.begin());
}

bool IsNullGuidArray(const std::array<uint8, 16>& bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](uint8 b) { return b == 0; });
}

// Default materials/textures live in the eztree package mount (their GUIDs
// stay the pre-extraction editor originals via the package's stored-identity
// manifest). Null when the package is not mounted — callers already treat a
// null GUID as "no default available".
GUID ResolvePackageAsset(const std::filesystem::path& relativePath)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    GUID guid = assets.ResolveAssetGuid(relativePath, EZTree::kPackageAlias);
    if (!guid.IsNull())
        return guid;
    return assets.ResolveAssetGuid(relativePath, kAssetSourceAliasEditor);
}

GUID ResolveDefaultBarkTexture(EZTree::BarkType type, const char* suffix)
{
    return ResolvePackageAsset(EZTreeECS::DefaultBarkTextureRelativePath(type, suffix));
}

GUID ResolveDefaultLeafTexture(EZTree::LeafType type, const char* suffix = "color")
{
    return ResolvePackageAsset(EZTreeECS::DefaultLeafTextureRelativePath(type, suffix));
}

bool IsKnownDefaultBarkTexture(const GUID& guid, const char* suffix)
{
    if (guid.IsNull())
        return false;

    constexpr std::array barkTypes{
        EZTree::BarkType::Oak,
        EZTree::BarkType::Birch,
        EZTree::BarkType::Pine,
        EZTree::BarkType::Willow,
    };
    return std::any_of(barkTypes.begin(), barkTypes.end(),
        [&guid, suffix](EZTree::BarkType type)
        {
            return guid == ResolveDefaultBarkTexture(type, suffix);
        });
}

bool IsKnownDefaultLeafTexture(const GUID& guid)
{
    if (guid.IsNull())
        return false;

    constexpr std::array leafTypes{
        EZTree::LeafType::Oak,
        EZTree::LeafType::Ash,
        EZTree::LeafType::Aspen,
        EZTree::LeafType::Pine,
    };
    return std::any_of(leafTypes.begin(), leafTypes.end(),
        [&guid](EZTree::LeafType type)
        {
            return guid == ResolveDefaultLeafTexture(type);
        });
}

bool AssignGuid(std::array<uint8, 16>& dst, const GUID& guid)
{
    if (guid.IsNull() || GuidFromArray(dst) == guid)
        return false;

    WriteGuidToArray(guid, dst);
    return true;
}

bool AssignGuidIfEmpty(std::array<uint8, 16>& dst, const GUID& guid)
{
    if (!IsNullGuidArray(dst))
        return false;

    return AssignGuid(dst, guid);
}

bool AssignDefaultBarkTexture(std::array<uint8, 16>& dst, EZTree::BarkType type, const char* suffix)
{
    const GUID current = GuidFromArray(dst);
    if (!current.IsNull() && !IsKnownDefaultBarkTexture(current, suffix))
        return false;

    return AssignGuid(dst, ResolveDefaultBarkTexture(type, suffix));
}

bool AssignDefaultLeafTexture(std::array<uint8, 16>& dst, EZTree::LeafType type)
{
    const GUID current = GuidFromArray(dst);
    if (!current.IsNull() && !IsKnownDefaultLeafTexture(current))
        return false;

    return AssignGuid(dst, ResolveDefaultLeafTexture(type));
}

GUID ResolveOptionalTrellisTexture()
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const Vector<GUID> textures = registry.GetAssetsByType(AssetType::Texture);
    for (const GUID& guid : textures)
    {
        AssetMetadata md{};
        if (!registry.TryGetAssetMetadata(guid, md))
            continue;
        const std::string stem = md.Path.stem().string();
        if (stem == "trellis" || stem == "wood_planks")
            return guid;
    }

    return GUID::Null();
}

bool ApplyDefaultTreeAssets(Components::EZTree& tree)
{
    bool changed = false;

    changed |= AssignGuidIfEmpty(tree.Options.barkMaterialGuid,
                                 ResolvePackageAsset(std::filesystem::path("Materials") / "EZTree" / "EZTree_Bark.material"));
    changed |= AssignGuidIfEmpty(tree.Options.leafMaterialGuid,
                                 ResolvePackageAsset(std::filesystem::path("Materials") / "EZTree" / "EZTree_Leaves.material"));
    changed |= AssignGuidIfEmpty(tree.Options.trellisMaterialGuid,
                                 ResolvePackageAsset(std::filesystem::path("Materials") / "EZTree" / "EZTree_Trellis.material"));

    changed |= AssignDefaultBarkTexture(tree.Options.barkColorTextureGuid, tree.Options.bark.type, "color");
    changed |= AssignDefaultBarkTexture(tree.Options.barkNormalTextureGuid, tree.Options.bark.type, "normal");
    changed |= AssignDefaultBarkTexture(tree.Options.barkRoughnessTextureGuid, tree.Options.bark.type, "roughness");
    changed |= AssignDefaultBarkTexture(tree.Options.barkAoTextureGuid, tree.Options.bark.type, "ao");
    changed |= AssignDefaultLeafTexture(tree.Options.leafColorTextureGuid, tree.Options.leaves.type);

    changed |= AssignGuidIfEmpty(tree.Options.trellisTextureGuid, ResolveOptionalTrellisTexture());
    return changed;
}

class CreateEZTreeCommand final : public Editor::IEditorCommand
{
public:
    CreateEZTreeCommand(ECS::World* world,
                        ECS::EntityHandle parent,
                        Components::HierarchyOrder order,
                        std::function<void(ECS::EntityHandle)> select,
                        std::function<void(ECS::EntityHandle)> created,
                        std::function<void()> structureChanged)
        : m_World(world)
        , m_Parent(parent)
        , m_Order(order)
        , m_Select(std::move(select))
        , m_Created(std::move(created))
        , m_StructureChanged(std::move(structureChanged))
    {
    }

    const char* GetName() const override { return "Create Tree Generator"; }

    void Do() override
    {
        if (!m_World)
            return;
        if (!m_Entity.IsValid())
            m_Entity = m_World->CreateEntity();
        m_World->AddComponentImmediate(m_Entity, Components::Transform{});
        m_World->AddComponentImmediate(m_Entity, MakeName("Tree Generator"));
        m_World->AddComponentImmediate(m_Entity, m_Order);
        Components::EZTree tree{};
        ApplyDefaultTreeAssets(tree);
        m_World->AddComponentImmediate(m_Entity, tree);
        if (m_Parent.IsValid() && m_World->IsValid(m_Parent))
            m_World->AddComponentImmediate(m_Entity, Components::Parent{m_Parent});
        if (m_Created)
            m_Created(m_Entity);
        if (m_Select)
            m_Select(m_Entity);
        if (m_StructureChanged)
            m_StructureChanged();
    }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        if (m_StructureChanged)
            m_StructureChanged();
    }

private:
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    ECS::EntityHandle m_Parent{};
    Components::HierarchyOrder m_Order{};
    std::function<void(ECS::EntityHandle)> m_Select;
    std::function<void(ECS::EntityHandle)> m_Created;
    std::function<void()> m_StructureChanged;
};

void MarkDirty(const InspectorContext& ctx,
               Components::EZTree& tree,
               Editor::EditorChangeNotifications::ChangeKind kind = Editor::EditorChangeNotifications::ChangeKind::Commit)
{
    tree.RuntimeOptionsHash = 0;
    tree.RuntimeMaterialHash = 0;
    tree.RuntimeMeshHandleId = 0;
    if (ctx.World)
        ctx.World->AddComponentImmediate(ctx.Entity, tree);
    if (ctx.ChangeNotifications)
        ctx.ChangeNotifications->NotifyComponentChange<Components::EZTree>(
            ctx.World, ctx.Entity, kind);
}

void WriteDirtyTreeComponent(const InspectorContext& ctx, Components::EZTree& tree)
{
    tree.RuntimeOptionsHash = 0;
    tree.RuntimeMaterialHash = 0;
    tree.RuntimeMeshHandleId = 0;
    if (ctx.World)
        ctx.World->AddComponentImmediate(ctx.Entity, tree);
}

std::string MakeTreeUndoName(const std::string& label)
{
    return "Change Tree Generator " + label;
}

void PreserveCurrentRuntimeState(Components::EZTree& restored, const Components::EZTree& current)
{
    restored.RuntimeOptionsHash = 0;
    restored.RuntimeMaterialHash = 0;
    restored.RuntimeMeshHandleId = 0;
    restored.RuntimeBranchMeshHandleId = current.RuntimeBranchMeshHandleId;
    restored.RuntimeLeafMeshHandleId = current.RuntimeLeafMeshHandleId;
    restored.RuntimeTrellisMeshHandleId = current.RuntimeTrellisMeshHandleId;
    restored.RuntimeBranchInstanceIndex = current.RuntimeBranchInstanceIndex;
    restored.RuntimeLeafInstanceIndex = current.RuntimeLeafInstanceIndex;
    restored.RuntimeTrellisInstanceIndex = current.RuntimeTrellisInstanceIndex;
    restored.RuntimeMeshGuid = current.RuntimeMeshGuid;
}

Editor::UndoRedoService::SnapshotTarget MakeTreeSnapshotTarget(const InspectorContext& ctx, const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    target.Capture = [ctx](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        auto* tree = ctx.World ? ctx.World->GetComponent<Components::EZTree>(ctx.Entity) : nullptr;
        if (!tree)
            return false;

        out.resize(sizeof(Components::EZTree));
        std::memcpy(out.data(), tree, sizeof(Components::EZTree));
        return true;
    };

    target.Apply = [ctx](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot) -> bool
    {
        if (snapshot.size() != sizeof(Components::EZTree) || !ctx.World || !ctx.Entity.IsValid() || !ctx.World->IsValid(ctx.Entity))
            return false;

        Components::EZTree restored{};
        std::memcpy(&restored, snapshot.data(), sizeof(Components::EZTree));
        if (const auto* current = ctx.World->GetComponent<Components::EZTree>(ctx.Entity))
            PreserveCurrentRuntimeState(restored, *current);
        else
        {
            restored.RuntimeOptionsHash = 0;
            restored.RuntimeMaterialHash = 0;
            restored.RuntimeMeshHandleId = 0;
        }

        ctx.World->AddComponentImmediate(ctx.Entity, restored);
        return true;
    };

    target.Notify = [ctx](Editor::EditorChangeNotifications::ChangeKind kind)
    {
        if (ctx.ChangeNotifications)
            ctx.ChangeNotifications->NotifyComponentChange<Components::EZTree>(ctx.World, ctx.Entity, kind);
    };

    return target;
}

using TreeInteractiveEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;

void EnsureTreeInteractiveEdit(const InspectorContext& ctx, const std::string& undoName, TreeInteractiveEdit& edit)
{
    if (edit.has_value() || !ctx.Undo)
        return;

    auto activeEdit = ctx.Undo->BeginInteractiveEdit(undoName, MakeTreeSnapshotTarget(ctx, undoName));
    if (activeEdit)
        edit.emplace(std::move(activeEdit));
}

template <typename Fn, typename Value>
void ApplyInspectorValue(const InspectorContext& ctx,
                         Components::EZTree& tree,
                         const Fn& fn,
                         Value value,
                         Editor::EditorChangeNotifications::ChangeKind kind,
                         TreeInteractiveEdit* edit = nullptr)
{
    fn(tree, value);
    if (edit && edit->has_value() && edit->value())
    {
        if (kind == Editor::EditorChangeNotifications::ChangeKind::Preview)
        {
            edit->value().Preview([ctx, &tree]()
            {
                WriteDirtyTreeComponent(ctx, tree);
            });
        }
        else
        {
            WriteDirtyTreeComponent(ctx, tree);
            edit->value().Commit();
            edit->reset();
        }
        return;
    }

    MarkDirty(ctx, tree, kind);
}

template <typename Fn>
void ApplyInspectorCommit(const InspectorContext& ctx,
                          Components::EZTree& tree,
                          const std::string& label,
                          const Fn& fn)
{
    TreeInteractiveEdit edit;
    const std::string undoName = MakeTreeUndoName(label);
    EnsureTreeInteractiveEdit(ctx, undoName, edit);

    fn(tree);
    if (edit && edit.value())
    {
        WriteDirtyTreeComponent(ctx, tree);
        edit->Commit();
        return;
    }

    MarkDirty(ctx, tree);
}

template <typename Fn>
void AddFloat(UIElement* parent, const InspectorContext& ctx, Components::EZTree& tree,
              const std::string& label, float value, Fn&& fn, float reset = 0.0f)
{
    auto* treePtr = &tree;
    auto handler = std::forward<Fn>(fn);
    auto edit = std::make_shared<TreeInteractiveEdit>();
    const std::string undoName = MakeTreeUndoName(label);
    InspectorDrag::AddFloatRowWithDrag(parent, label, value,
        [ctx, treePtr, fn = handler, edit, undoName](float v)
        {
            EnsureTreeInteractiveEdit(ctx, undoName, *edit);
            ApplyInspectorValue(ctx, *treePtr, fn, v, Editor::EditorChangeNotifications::ChangeKind::Preview, edit.get());
        },
        [ctx, treePtr, fn = handler, edit, undoName](float v)
        {
            EnsureTreeInteractiveEdit(ctx, undoName, *edit);
            ApplyInspectorValue(ctx, *treePtr, fn, v, Editor::EditorChangeNotifications::ChangeKind::Commit, edit.get());
        },
        reset);
}

template <typename Fn>
void AddInt(UIElement* parent, const InspectorContext& ctx, Components::EZTree& tree,
            const std::string& label, int value, Fn&& fn, int reset = 0)
{
    auto* treePtr = &tree;
    auto handler = std::forward<Fn>(fn);
    auto edit = std::make_shared<TreeInteractiveEdit>();
    const std::string undoName = MakeTreeUndoName(label);
    InspectorDrag::AddIntRowWithDrag(parent, label, value,
        [ctx, treePtr, fn = handler, edit, undoName](int v)
        {
            EnsureTreeInteractiveEdit(ctx, undoName, *edit);
            ApplyInspectorValue(ctx, *treePtr, fn, v, Editor::EditorChangeNotifications::ChangeKind::Preview, edit.get());
        },
        [ctx, treePtr, fn = handler, edit, undoName](int v)
        {
            EnsureTreeInteractiveEdit(ctx, undoName, *edit);
            ApplyInspectorValue(ctx, *treePtr, fn, v, Editor::EditorChangeNotifications::ChangeKind::Commit, edit.get());
        },
        reset);
}

template <typename Fn>
void AddToggle(UIElement* parent, const InspectorContext& ctx, Components::EZTree& tree,
               const std::string& label, bool value, Fn&& fn)
{
    auto* treePtr = &tree;
    InspectorDrag::AddToggleRow(parent, label, value,
        [ctx, treePtr, label, fn = std::forward<Fn>(fn)](bool v)
        {
            ApplyInspectorCommit(ctx, *treePtr, label, [fn, v](Components::EZTree& t) { fn(t, v); });
        });
}

template <typename Fn>
void AddDropdown(UIElement* parent,
                 const InspectorContext& ctx,
                 Components::EZTree& tree,
                 const std::string& label,
                 const std::vector<Dropdown::Option>& options,
                 int selected,
                 Fn&& fn)
{
    auto* dd = InspectorUI::AddDropdownRow(parent, label, options, selected);
    auto* treePtr = &tree;
    dd->SetOnValueChanged([ctx, treePtr, label, fn = std::forward<Fn>(fn)](const std::string& value)
    {
        ApplyInspectorCommit(ctx, *treePtr, label, [fn, value](Components::EZTree& t) { fn(t, value); });
    });
}

template <typename Fn>
void AddAssetSlot(UIElement* parent,
                  const InspectorContext& ctx,
                  Components::EZTree& tree,
                  const std::string& label,
                  const std::array<uint8, 16>& current,
                  const std::vector<AssetType>& acceptedTypes,
                  Fn&& fn)
{
    auto* treePtr = &tree;
    InspectorUI::AddAssetFieldRow(
        parent,
        label,
        GuidFromArray(current),
        acceptedTypes,
        &EngineCore::GetInstance().GetAssetManager().GetRegistry(),
        [ctx, treePtr, label, fn = std::forward<Fn>(fn)](const GUID& guid)
        {
            ApplyInspectorCommit(ctx, *treePtr, label, [fn, guid](Components::EZTree& t) { fn(t, guid); });
        },
        ctx.Thumbnails);
}

Foldout* AddFoldout(UIElement* parent, const std::string& title, bool expanded = false)
{
    auto foldout = std::make_unique<Foldout>();
    foldout->SetTitle(title);
    foldout->AddClass("rp-foldout");
    foldout->SetExpanded(expanded);
    auto* raw = foldout.get();
    parent->AddChild(std::move(foldout));
    return raw;
}

// ---------------------------------------------------------------------------
// Selection-mask wind (moved verbatim from the editor's SceneViewOverlaysRG
// when this half left Editor.exe): resolve the tree's authored wind + wind
// volumes at the entity, expressed in the mesh's local axes for the mask
// vertex shader.
// ---------------------------------------------------------------------------

struct TreeMaskWind
{
    float Strength[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float Params[4] = {0.0f, 1.0f, 1.0f, 0.0f};
};

void NormalizeAxis3(float v[3])
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-6f)
    {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

Engine::Renderer::ResolvedWind BuildTreeMaskBaseWindWorld(const EZTree::TreeOptions& options,
                                                          const Components::WorldTransform& worldTransform)
{
    Engine::Renderer::ResolvedWind wind{};
    wind.GustFrequency = options.wind.frequency;
    wind.GustScale = options.wind.scale;
    if (!options.wind.enabled)
        return wind;

    const float* m = worldTransform.matrix;
    wind.VelocityX = m[0] * options.wind.strength.x + m[4] * options.wind.strength.y + m[8] * options.wind.strength.z;
    wind.VelocityY = m[1] * options.wind.strength.x + m[5] * options.wind.strength.y + m[9] * options.wind.strength.z;
    wind.VelocityZ = m[2] * options.wind.strength.x + m[6] * options.wind.strength.y + m[10] * options.wind.strength.z;
    wind.Weight = 1.0f;
    return wind;
}

Engine::Renderer::ResolvedWind TreeMaskWorldWindToLocal(const Engine::Renderer::ResolvedWind& worldWind,
                                                        const Components::WorldTransform& worldTransform)
{
    const float* m = worldTransform.matrix;
    float axisX[3]{m[0], m[1], m[2]};
    float axisY[3]{m[4], m[5], m[6]};
    float axisZ[3]{m[8], m[9], m[10]};
    NormalizeAxis3(axisX);
    NormalizeAxis3(axisY);
    NormalizeAxis3(axisZ);

    Engine::Renderer::ResolvedWind localWind = worldWind;
    localWind.VelocityX = worldWind.VelocityX * axisX[0] + worldWind.VelocityY * axisX[1] + worldWind.VelocityZ * axisX[2];
    localWind.VelocityY = worldWind.VelocityX * axisY[0] + worldWind.VelocityY * axisY[1] + worldWind.VelocityZ * axisY[2];
    localWind.VelocityZ = worldWind.VelocityX * axisZ[0] + worldWind.VelocityY * axisZ[1] + worldWind.VelocityZ * axisZ[2];
    return localWind;
}

TreeMaskWind BuildTreeMaskWind(ECS::World& world,
                               const Components::EZTree& tree,
                               const Components::WorldTransform& worldTransform,
                               const Components::LocalBounds* bounds,
                               float strengthScale)
{
    TreeMaskWind out{};
    const float windHeight = bounds ? std::max(0.25f, bounds->Box.halfExtents.y * 2.0f) : 20.0f;
    const float windBaseY = bounds ? (bounds->Box.center.y - bounds->Box.halfExtents.y) : 0.0f;

    Engine::Renderer::ResolvedWind worldWind = BuildTreeMaskBaseWindWorld(tree.Options, worldTransform);
    if (tree.Options.wind.enabled)
    {
        worldWind = Engine::Renderer::WindVolumeResolver::ResolveAt(world,
                                                                    worldTransform.matrix[12],
                                                                    worldTransform.matrix[13],
                                                                    worldTransform.matrix[14],
                                                                    worldWind);
    }
    const Engine::Renderer::ResolvedWind localWind = TreeMaskWorldWindToLocal(worldWind, worldTransform);

    out.Strength[0] = localWind.VelocityX * strengthScale;
    out.Strength[1] = localWind.VelocityY * strengthScale;
    out.Strength[2] = localWind.VelocityZ * strengthScale;
    out.Strength[3] = tree.Options.wind.enabled ? 1.0f : 0.0f;
    out.Params[0] = std::max(localWind.GustFrequency, 0.0f);
    out.Params[1] = std::max(0.001f, localWind.GustScale);
    out.Params[2] = windHeight;
    out.Params[3] = windBaseY;
    return out;
}

// A mask part only outlines when its mesh actually resolves in the GPU mesh
// registry — mirrors the editor-side draw validity check so the "no tree part
// built yet, outline the combined mesh" fallback still works.
bool IsMaskMeshDrawable(Engine::Renderer::RenderServices* rs, uint64 meshGpuHandleId)
{
    if (!rs || meshGpuHandleId == 0u)
        return false;
    auto& meshReg = rs->GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* entry = meshReg.Find(Rendering::MeshGPUHandle(meshGpuHandleId));
    if (!entry || entry->indexCount == 0)
        return false;
    Rendering::MeshGPUEntryBindings bindings{};
    return meshReg.TryGetDrawableBindings(*entry, bindings);
}

// Stats line that re-reads the live component. Regeneration runs async in the
// extraction system after an options commit, so a snapshot taken while the
// inspector section is built goes stale the moment the new mesh lands. Same
// throttled change-detected pull the Tree Stats panel uses (Label::SetText
// no-ops on identical text, so steady state costs a string build per poll).
class TreeStatsLine final : public Label
{
public:
    TreeStatsLine(ECS::World* world, ECS::EntityHandle entity)
        : m_World(world)
        , m_Entity(entity)
    {
        AddClass("inspector-text");
        RefreshText();
    }

    ~TreeStatsLine() override
    {
        if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
            manager->UnregisterPeriodicRefresh(m_RefreshToken);
    }

private:
    void OnPostLayout() override
    {
        Label::OnPostLayout();
        // Register once on the manager's low-frequency refresh tick so the live
        // vertex/triangle counts keep updating after an async regeneration even
        // while the editor is input-idle — OnPostLayout only fires on heavy
        // passes. SetText no-ops on identical text, so an unchanged tick costs
        // one string build.
        if (m_RefreshToken == 0)
        {
            if (UIManager* manager = GetOwnerManager())
            {
                m_RefreshManager = UIManagerRef(manager);
                m_RefreshToken = manager->RegisterPeriodicRefresh([this]() { RefreshText(); });
            }
        }
    }

    void RefreshText()
    {
        if (!m_World || !m_World->IsValid(m_Entity))
            return;
        const auto* tree = m_World->GetComponent<Components::EZTree>(m_Entity);
        if (!tree)
            return;
        SetText("Vertices: " + std::to_string(tree->RuntimeVertexCount) +
                "  Triangles: " + std::to_string(tree->RuntimeTriangleCount) +
                "  Branches: " + std::to_string(tree->RuntimeBranchCount));
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
};

class EZTreeEditorPlugin final : public Editor::IEditorPlugin
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

    void RegisterInspectors() override
    {
        InspectorRegistry::Get().RegisterComponentInspector<Components::EZTree>(
            [](const InspectorContext& ctx)
            {
                auto* tree = ctx.World ? ctx.World->GetComponentForWrite<Components::EZTree>(ctx.Entity) : nullptr;
                if (!tree)
                    return;
                if (ApplyDefaultTreeAssets(*tree))
                    MarkDirty(ctx, *tree);
                auto& o = tree->Options;

                ctx.Parent->AddChild(std::make_unique<TreeStatsLine>(ctx.World, ctx.Entity));

                {
                    auto row = std::make_unique<UIElement>();
                    row->AddClass("inspector-row");
                    auto regenerate = std::make_unique<Button>();
                    regenerate->AddClass("small");
                    regenerate->AddClass("secondary");
                    regenerate->SetText("Regenerate");
                    regenerate->RegisterEventHandler(kEventButtonClick, [ctx, tree](UIEvent&) {
                        tree->RuntimeOptionsHash = 0;
                        tree->RuntimeMeshHandleId = 0;
                        if (ctx.World)
                            ctx.World->AddComponentImmediate(ctx.Entity, *tree);
                        if (ctx.ChangeNotifications)
                            ctx.ChangeNotifications->NotifyComponentChange<Components::EZTree>(
                                ctx.World, ctx.Entity, Editor::EditorChangeNotifications::ChangeKind::Commit);
                    });
                    row->AddChild(std::move(regenerate));

                    auto randomize = std::make_unique<Button>();
                    randomize->AddClass("small");
                    randomize->AddClass("secondary");
                    randomize->SetText("Randomize");
                    randomize->RegisterEventHandler(kEventButtonClick, [ctx, tree](UIEvent&) {
                        ApplyInspectorCommit(ctx, *tree, "Randomize", [](Components::EZTree& t)
                        {
                            std::random_device rd;
                            t.Options.seed = rd();
                            ++t.RandomizeCounter;
                        });
                    });
                    row->AddChild(std::move(randomize));
                    ctx.Parent->AddChild(std::move(row));
                }

                static const std::vector<Dropdown::Option> typeOptions{{"Deciduous", "deciduous"}, {"Evergreen", "evergreen"}};
                AddDropdown(ctx.Parent, ctx, *tree, "Type", typeOptions, o.type == EZTree::TreeType::Evergreen ? 1 : 0,
                    [](Components::EZTree& t, const std::string& v) { t.Options.type = EZTree::TreeTypeFromString(v, t.Options.type); });
                AddInt(ctx.Parent, ctx, *tree, "Seed", static_cast<int>(o.seed),
                       [](Components::EZTree& t, int v) { t.Options.seed = static_cast<uint32>(std::max(0, v)); });

                if (auto* materials = AddFoldout(ctx.Parent, "Materials", false))
                {
                    AddAssetSlot(materials, ctx, *tree, "Bark", o.barkMaterialGuid, {AssetType::Material},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.barkMaterialGuid); });
                    AddAssetSlot(materials, ctx, *tree, "Leaves", o.leafMaterialGuid, {AssetType::Material},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.leafMaterialGuid); });
                    AddAssetSlot(materials, ctx, *tree, "Trellis", o.trellisMaterialGuid, {AssetType::Material},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.trellisMaterialGuid); });
                }

                if (auto* textures = AddFoldout(ctx.Parent, "Textures", false))
                {
                    AddAssetSlot(textures, ctx, *tree, "Bark Color", o.barkColorTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.barkColorTextureGuid); });
                    AddAssetSlot(textures, ctx, *tree, "Bark Normal", o.barkNormalTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.barkNormalTextureGuid); });
                    AddAssetSlot(textures, ctx, *tree, "Bark Roughness", o.barkRoughnessTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.barkRoughnessTextureGuid); });
                    AddAssetSlot(textures, ctx, *tree, "Bark AO", o.barkAoTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.barkAoTextureGuid); });
                    AddAssetSlot(textures, ctx, *tree, "Leaf Color", o.leafColorTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.leafColorTextureGuid); });
                    AddAssetSlot(textures, ctx, *tree, "Trellis", o.trellisTextureGuid, {AssetType::Texture},
                        [](Components::EZTree& t, const GUID& guid) { WriteGuidToArray(guid, t.Options.trellisTextureGuid); });
                }

                if (auto* bark = AddFoldout(ctx.Parent, "Bark", false))
                {
                    static const std::vector<Dropdown::Option> barkOptions{{"Oak", "oak"}, {"Birch", "birch"}, {"Pine", "pine"}, {"Willow", "willow"}};
                    AddDropdown(bark, ctx, *tree, "Type", barkOptions, static_cast<int>(o.bark.type),
                        [](Components::EZTree& t, const std::string& v)
                        {
                            t.Options.bark.type = EZTree::BarkTypeFromString(v, t.Options.bark.type);
                            (void)ApplyDefaultTreeAssets(t);
                        });
                    AddInt(bark, ctx, *tree, "Tint", static_cast<int>(o.bark.tint), [](Components::EZTree& t, int v) { t.Options.bark.tint = static_cast<uint32>(v); }, 0xFFFFFF);
                    AddToggle(bark, ctx, *tree, "Flat Shading", o.bark.flatShading, [](Components::EZTree& t, bool v) { t.Options.bark.flatShading = v; });
                    AddToggle(bark, ctx, *tree, "Textured", o.bark.textured, [](Components::EZTree& t, bool v) { t.Options.bark.textured = v; });
                    AddFloat(bark, ctx, *tree, "Texture Scale X", o.bark.textureScale.x, [](Components::EZTree& t, float v) { t.Options.bark.textureScale.x = v; }, 1.0f);
                    AddFloat(bark, ctx, *tree, "Texture Scale Y", o.bark.textureScale.y, [](Components::EZTree& t, float v) { t.Options.bark.textureScale.y = v; }, 1.0f);
                }

                if (auto* branch = AddFoldout(ctx.Parent, "Branches", false))
                {
                    AddInt(branch, ctx, *tree, "Levels", static_cast<int>(o.branch.levels), [](Components::EZTree& t, int v) { t.Options.branch.levels = std::min<uint32>(3u, static_cast<uint32>(std::max(0, v))); }, 3);
                    AddFloat(branch, ctx, *tree, "Force X", o.branch.forceDirection.x, [](Components::EZTree& t, float v) { t.Options.branch.forceDirection.x = v; });
                    AddFloat(branch, ctx, *tree, "Force Y", o.branch.forceDirection.y, [](Components::EZTree& t, float v) { t.Options.branch.forceDirection.y = v; }, 1.0f);
                    AddFloat(branch, ctx, *tree, "Force Z", o.branch.forceDirection.z, [](Components::EZTree& t, float v) { t.Options.branch.forceDirection.z = v; });
                    AddFloat(branch, ctx, *tree, "Force Strength", o.branch.forceStrength, [](Components::EZTree& t, float v) { t.Options.branch.forceStrength = v; }, 0.01f);
                    for (uint32 level = 0; level < 4; ++level)
                    {
                        auto* levelFoldout = AddFoldout(branch, "Level " + std::to_string(level), false);
                        AddFloat(levelFoldout, ctx, *tree, "Angle", o.branch.angle[level], [level](Components::EZTree& t, float v) { t.Options.branch.angle[level] = v; });
                        AddInt(levelFoldout, ctx, *tree, "Children", static_cast<int>(o.branch.children[level]), [level](Components::EZTree& t, int v) { t.Options.branch.children[level] = static_cast<uint32>(std::max(0, v)); });
                        AddFloat(levelFoldout, ctx, *tree, "Length", o.branch.length[level], [level](Components::EZTree& t, float v) { t.Options.branch.length[level] = v; });
                        AddFloat(levelFoldout, ctx, *tree, "Radius", o.branch.radius[level], [level](Components::EZTree& t, float v) { t.Options.branch.radius[level] = v; });
                        AddInt(levelFoldout, ctx, *tree, "Sections", static_cast<int>(o.branch.sections[level]), [level](Components::EZTree& t, int v) { t.Options.branch.sections[level] = static_cast<uint32>(std::max(1, v)); });
                        AddInt(levelFoldout, ctx, *tree, "Segments", static_cast<int>(o.branch.segments[level]), [level](Components::EZTree& t, int v) { t.Options.branch.segments[level] = static_cast<uint32>(std::max(3, v)); });
                        AddFloat(levelFoldout, ctx, *tree, "Start", o.branch.start[level], [level](Components::EZTree& t, float v) { t.Options.branch.start[level] = v; });
                        AddFloat(levelFoldout, ctx, *tree, "Taper", o.branch.taper[level], [level](Components::EZTree& t, float v) { t.Options.branch.taper[level] = v; });
                        AddFloat(levelFoldout, ctx, *tree, "Twist", o.branch.twist[level], [level](Components::EZTree& t, float v) { t.Options.branch.twist[level] = v; });
                        AddFloat(levelFoldout, ctx, *tree, "Gnarliness", o.branch.gnarliness[level], [level](Components::EZTree& t, float v) { t.Options.branch.gnarliness[level] = v; });
                    }
                }

                if (auto* leaves = AddFoldout(ctx.Parent, "Leaves", false))
                {
                    static const std::vector<Dropdown::Option> leafOptions{{"Oak", "oak"}, {"Ash", "ash"}, {"Aspen", "aspen"}, {"Pine", "pine"}};
                    AddDropdown(leaves, ctx, *tree, "Type", leafOptions, static_cast<int>(o.leaves.type),
                        [](Components::EZTree& t, const std::string& v)
                        {
                            t.Options.leaves.type = EZTree::LeafTypeFromString(v, t.Options.leaves.type);
                            (void)ApplyDefaultTreeAssets(t);
                        });
                    static const std::vector<Dropdown::Option> billboardOptions{{"Single", "single"}, {"Double", "double"}};
                    AddDropdown(leaves, ctx, *tree, "Billboard", billboardOptions, static_cast<int>(o.leaves.billboard),
                        [](Components::EZTree& t, const std::string& v) { t.Options.leaves.billboard = EZTree::BillboardModeFromString(v, t.Options.leaves.billboard); });
                    AddInt(leaves, ctx, *tree, "Count", static_cast<int>(o.leaves.count), [](Components::EZTree& t, int v) { t.Options.leaves.count = static_cast<uint32>(std::max(0, v)); }, 1);
                    AddFloat(leaves, ctx, *tree, "Angle", o.leaves.angle, [](Components::EZTree& t, float v) { t.Options.leaves.angle = v; }, 10.0f);
                    AddFloat(leaves, ctx, *tree, "Start", o.leaves.start, [](Components::EZTree& t, float v) { t.Options.leaves.start = v; });
                    AddFloat(leaves, ctx, *tree, "Size", o.leaves.size, [](Components::EZTree& t, float v) { t.Options.leaves.size = v; }, 2.5f);
                    AddFloat(leaves, ctx, *tree, "Size Variance", o.leaves.sizeVariance, [](Components::EZTree& t, float v) { t.Options.leaves.sizeVariance = v; }, 0.7f);
                    AddInt(leaves, ctx, *tree, "Tint", static_cast<int>(o.leaves.tint), [](Components::EZTree& t, int v) { t.Options.leaves.tint = static_cast<uint32>(v); }, 0xFFFFFF);
                    AddFloat(leaves, ctx, *tree, "Alpha Test", o.leaves.alphaTest, [](Components::EZTree& t, float v) { t.Options.leaves.alphaTest = std::clamp(v, 0.0f, 1.0f); }, 0.5f);
                    AddInt(leaves, ctx, *tree, "Atlas Columns", static_cast<int>(o.leaves.textureColumns), [](Components::EZTree& t, int v) { t.Options.leaves.textureColumns = static_cast<uint32>(std::max(1, v)); }, 1);
                    AddInt(leaves, ctx, *tree, "Atlas Rows", static_cast<int>(o.leaves.textureRows), [](Components::EZTree& t, int v) { t.Options.leaves.textureRows = static_cast<uint32>(std::max(1, v)); }, 1);
                    AddInt(leaves, ctx, *tree, "Atlas Tile", static_cast<int>(o.leaves.textureTile), [](Components::EZTree& t, int v) { t.Options.leaves.textureTile = static_cast<uint32>(std::max(0, v)); }, 0);
                    AddFloat(leaves, ctx, *tree, "UV Scale X", o.leaves.textureScale.x, [](Components::EZTree& t, float v) { t.Options.leaves.textureScale.x = std::max(0.0001f, v); }, 1.0f);
                    AddFloat(leaves, ctx, *tree, "UV Scale Y", o.leaves.textureScale.y, [](Components::EZTree& t, float v) { t.Options.leaves.textureScale.y = std::max(0.0001f, v); }, 1.0f);
                    AddFloat(leaves, ctx, *tree, "UV Offset X", o.leaves.textureOffset.x, [](Components::EZTree& t, float v) { t.Options.leaves.textureOffset.x = v; });
                    AddFloat(leaves, ctx, *tree, "UV Offset Y", o.leaves.textureOffset.y, [](Components::EZTree& t, float v) { t.Options.leaves.textureOffset.y = v; });
                    AddToggle(leaves, ctx, *tree, "Random Atlas Tile", o.leaves.randomTextureTile, [](Components::EZTree& t, bool v) { t.Options.leaves.randomTextureTile = v; });
                    AddToggle(leaves, ctx, *tree, "Rounded Normals", o.leaves.roundedNormals, [](Components::EZTree& t, bool v) { t.Options.leaves.roundedNormals = v; });
                }

                if (auto* trellis = AddFoldout(ctx.Parent, "Trellis", false))
                {
                    AddToggle(trellis, ctx, *tree, "Enabled", o.trellis.enabled, [](Components::EZTree& t, bool v) { t.Options.trellis.enabled = v; });
                    AddToggle(trellis, ctx, *tree, "Visible", o.trellis.visible, [](Components::EZTree& t, bool v) { t.Options.trellis.visible = v; });
                    AddFloat(trellis, ctx, *tree, "Position X", o.trellis.position.x, [](Components::EZTree& t, float v) { t.Options.trellis.position.x = v; });
                    AddFloat(trellis, ctx, *tree, "Position Y", o.trellis.position.y, [](Components::EZTree& t, float v) { t.Options.trellis.position.y = v; });
                    AddFloat(trellis, ctx, *tree, "Position Z", o.trellis.position.z, [](Components::EZTree& t, float v) { t.Options.trellis.position.z = v; }, -2.0f);
                    AddFloat(trellis, ctx, *tree, "Width", o.trellis.width, [](Components::EZTree& t, float v) { t.Options.trellis.width = v; }, 10.0f);
                    AddFloat(trellis, ctx, *tree, "Height", o.trellis.height, [](Components::EZTree& t, float v) { t.Options.trellis.height = v; }, 20.0f);
                    AddFloat(trellis, ctx, *tree, "Spacing", o.trellis.spacing, [](Components::EZTree& t, float v) { t.Options.trellis.spacing = v; }, 2.0f);
                    AddFloat(trellis, ctx, *tree, "Force Strength", o.trellis.forceStrength, [](Components::EZTree& t, float v) { t.Options.trellis.forceStrength = v; }, 0.02f);
                    AddFloat(trellis, ctx, *tree, "Max Distance", o.trellis.forceMaxDistance, [](Components::EZTree& t, float v) { t.Options.trellis.forceMaxDistance = v; }, 3.0f);
                    AddFloat(trellis, ctx, *tree, "Falloff", o.trellis.forceFalloff, [](Components::EZTree& t, float v) { t.Options.trellis.forceFalloff = v; }, 1.0f);
                    AddFloat(trellis, ctx, *tree, "Cylinder Radius", o.trellis.cylinderRadius, [](Components::EZTree& t, float v) { t.Options.trellis.cylinderRadius = v; }, 0.05f);
                    AddInt(trellis, ctx, *tree, "Color", static_cast<int>(o.trellis.color), [](Components::EZTree& t, int v) { t.Options.trellis.color = static_cast<uint32>(v); }, 0x8B4513);
                }

                if (auto* wind = AddFoldout(ctx.Parent, "Wind", false))
                {
                    AddToggle(wind, ctx, *tree, "Enabled", o.wind.enabled, [](Components::EZTree& t, bool v) { t.Options.wind.enabled = v; });
                    AddFloat(wind, ctx, *tree, "Strength X", o.wind.strength.x, [](Components::EZTree& t, float v) { t.Options.wind.strength.x = v; }, 0.5f);
                    AddFloat(wind, ctx, *tree, "Strength Z", o.wind.strength.z, [](Components::EZTree& t, float v) { t.Options.wind.strength.z = v; }, 0.5f);
                    AddFloat(wind, ctx, *tree, "Frequency", o.wind.frequency, [](Components::EZTree& t, float v) { t.Options.wind.frequency = v; }, 0.5f);
                    AddFloat(wind, ctx, *tree, "Scale", o.wind.scale, [](Components::EZTree& t, float v) { t.Options.wind.scale = v; }, 70.0f);
                }

                if (auto* overrides = AddFoldout(ctx.Parent, "Branch Overrides", false))
                {
                    AddInt(overrides, ctx, *tree, "Count", static_cast<int>(o.branchOverrideCount), [](Components::EZTree& t, int v) { t.Options.branchOverrideCount = std::min<uint32>(16u, static_cast<uint32>(std::max(0, v))); });
                    for (uint32 i = 0; i < o.branchOverrideCount && i < o.branchOverrides.size(); ++i)
                    {
                        auto* ovFoldout = AddFoldout(overrides, "Override " + std::to_string(i), false);
                        AddToggle(ovFoldout, ctx, *tree, "Enabled", o.branchOverrides[i].enabled, [i](Components::EZTree& t, bool v) { t.Options.branchOverrides[i].enabled = v; });
                        AddInt(ovFoldout, ctx, *tree, "Branch Id", static_cast<int>(o.branchOverrides[i].branchId), [i](Components::EZTree& t, int v) { t.Options.branchOverrides[i].branchId = static_cast<uint32>(std::max(0, v)); });
                        AddInt(ovFoldout, ctx, *tree, "Level", static_cast<int>(o.branchOverrides[i].level), [i](Components::EZTree& t, int v) { t.Options.branchOverrides[i].level = std::min<uint32>(3u, static_cast<uint32>(std::max(0, v))); });
                        AddFloat(ovFoldout, ctx, *tree, "Length Scale", o.branchOverrides[i].lengthScale, [i](Components::EZTree& t, float v) { t.Options.branchOverrides[i].lengthScale = v; }, 1.0f);
                        AddFloat(ovFoldout, ctx, *tree, "Radius Scale", o.branchOverrides[i].radiusScale, [i](Components::EZTree& t, float v) { t.Options.branchOverrides[i].radiusScale = v; }, 1.0f);
                        AddFloat(ovFoldout, ctx, *tree, "Angle Offset", o.branchOverrides[i].angleOffsetDegrees, [i](Components::EZTree& t, float v) { t.Options.branchOverrides[i].angleOffsetDegrees = v; });
                        AddFloat(ovFoldout, ctx, *tree, "Twist Offset", o.branchOverrides[i].twistOffset, [i](Components::EZTree& t, float v) { t.Options.branchOverrides[i].twistOffset = v; });
                    }
                }
            });
    }

    void BuildHierarchyContextMenu(ContextMenuBuilder& builder,
                                   const Editor::HierarchyContextMenuContext& context) override
    {
        builder.AddItem("Create/Tree Generator", kCreateEZTreeCommand, MenuItemFlag_None, 40, kTreeGeneratorIcon);
        // Panel entry point: only offered on entities that are trees.
        if (context.World && context.HasTargetEntity &&
            context.World->GetComponent<Components::EZTree>(context.TargetEntity))
            builder.AddItem("Tree Stats", kShowTreeStatsCommand, MenuItemFlag_None, 41,
                            kTreeGeneratorIcon);
    }

    bool HandleHierarchyCommand(uint32 commandId,
                                const Editor::HierarchyCommandContext& context) override
    {
        if (commandId == kShowTreeStatsCommand)
        {
            Editor::EditorPanelRegistry::Get().OpenPanel("EZTreeStats");
            return true;
        }
        if (commandId != kCreateEZTreeCommand || !context.World)
            return false;

        auto command = std::make_unique<CreateEZTreeCommand>(
            context.World,
            context.HasTargetEntity ? context.TargetEntity : ECS::EntityHandle{},
            NextOrder(context.World),
            context.SelectEntity,
            context.NotifyEntityCreated,
            context.NotifyWorldStructureChanged);
        if (context.Undo)
            context.Undo->Execute(std::move(command));
        else
            command->Do();
        return true;
    }

    bool ContributesSelectionMask() const override { return true; }

    void CollectSelectionMaskParts(ECS::World& world,
                                   ECS::EntityHandle entity,
                                   std::vector<Editor::SelectionMaskPart>& outParts) override
    {
        const auto* tree = world.GetComponent<Components::EZTree>(entity);
        if (!tree || !ECS::Entity(&world, entity).IsEnabled<Components::EZTree>())
            return;
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return;
        const auto* xf = world.GetComponent<Components::WorldTransform>(entity);
        if (!xf)
            return;
        const auto* bounds = world.GetComponent<Components::LocalBounds>(entity);

        const TreeMaskWind branchWind = BuildTreeMaskWind(world, *tree, *xf, bounds, 1.0f);
        const TreeMaskWind leafWind = BuildTreeMaskWind(world, *tree, *xf, bounds, 1.35f);

        bool appendedTreePart = false;
        if (IsMaskMeshDrawable(rs, tree->RuntimeBranchMeshHandleId))
        {
            Editor::SelectionMaskPart part{};
            part.MeshGpuHandleId = tree->RuntimeBranchMeshHandleId;
            std::memcpy(part.WindStrength, branchWind.Strength, sizeof(part.WindStrength));
            std::memcpy(part.WindParams, branchWind.Params, sizeof(part.WindParams));
            part.WindSeed = static_cast<float>(tree->RuntimeBranchInstanceIndex);
            outParts.push_back(part);
            appendedTreePart = true;
        }

        if (IsMaskMeshDrawable(rs, tree->RuntimeLeafMeshHandleId))
        {
            GUID leafTextureGuid(GuidFromArray(tree->Options.leafColorTextureGuid));
            if (leafTextureGuid.IsNull())
                leafTextureGuid = ResolveDefaultLeafTexture(tree->Options.leaves.type);

            Editor::SelectionMaskPart part{};
            part.MeshGpuHandleId = tree->RuntimeLeafMeshHandleId;
            part.AlphaTexture = leafTextureGuid.IsNull()
                ? Rendering::TextureHandle{}
                : rs->Textures().GetOrUpload(leafTextureGuid);
            part.AlphaCutoff = tree->Options.leaves.alphaTest;
            part.UvScale[0] = std::max(0.0001f, tree->Options.leaves.textureScale.x);
            part.UvScale[1] = std::max(0.0001f, tree->Options.leaves.textureScale.y);
            part.UvOffset[0] = tree->Options.leaves.textureOffset.x;
            part.UvOffset[1] = tree->Options.leaves.textureOffset.y;
            std::memcpy(part.WindStrength, leafWind.Strength, sizeof(part.WindStrength));
            std::memcpy(part.WindParams, leafWind.Params, sizeof(part.WindParams));
            part.WindSeed = static_cast<float>(tree->RuntimeLeafInstanceIndex);
            outParts.push_back(part);
            appendedTreePart = true;
        }

        if (IsMaskMeshDrawable(rs, tree->RuntimeTrellisMeshHandleId))
        {
            Editor::SelectionMaskPart part{};
            part.MeshGpuHandleId = tree->RuntimeTrellisMeshHandleId;
            outParts.push_back(part);
            appendedTreePart = true;
        }

        // No individual part built yet — outline the combined mesh so a
        // freshly created tree still highlights.
        if (!appendedTreePart && IsMaskMeshDrawable(rs, tree->RuntimeMeshHandleId))
        {
            Editor::SelectionMaskPart part{};
            part.MeshGpuHandleId = tree->RuntimeMeshHandleId;
            outParts.push_back(part);
        }
    }
};

EZTreeEditorPlugin& GetPlugin()
{
    static EZTreeEditorPlugin plugin;
    return plugin;
}

bool ResolveTreePick(ECS::World& world, ECS::EntityHandle entity, Editor::Picking::SyntheticMeshPick& outPick)
{
    const auto* tree = world.GetComponent<Components::EZTree>(entity);
    if (!tree || !ECS::Entity(&world, entity).IsEnabled<Components::EZTree>() || tree->RuntimeMeshHandleId == 0u)
        return false;
    outPick.MeshGpuHandleId = tree->RuntimeMeshHandleId;
    outPick.ModelAssetGuid = GuidFromArray(tree->RuntimeMeshGuid);
    outPick.RenderLayerMask = 1u;
    return true;
}

} // namespace

void RegisterEZTreeEditorPlugin()
{
    const ECS::ComponentTypeId treeTypeId = ECS::GetComponentTypeId<Components::EZTree>();

    Editor::EditorComponentTraits traits;
    traits.DisplayName = "Tree Generator";
    traits.InspectorIconClass = "inspector-section-icon-tree-generator";
    traits.HierarchyRowClass = "hierarchy-entity-tree-generator";
    traits.IsPickInstanceRoot = true;
    traits.GizmoPivotAtEntityOrigin = true;
    Editor::EditorComponentTraitsRegistry::Get().Register(treeTypeId, std::move(traits));

    Editor::Picking::EditorPickProvider pickProvider;
    pickProvider.Resolve = &ResolveTreePick;
    pickProvider.Enumerate = [](ECS::World& world,
                                const std::function<void(ECS::EntityHandle,
                                                         const Editor::Picking::SyntheticMeshPick&)>& emit)
    {
        world.Query<ECS::Read<Components::EZTree>, ECS::Read<Components::WorldTransform>>()
            .Each([&world, &emit](ECS::EntityHandle entity,
                                  const Components::EZTree&,
                                  const Components::WorldTransform&)
            {
                Editor::Picking::SyntheticMeshPick pick;
                if (ResolveTreePick(world, entity, pick))
                    emit(entity, pick);
            });
    };
    Editor::Picking::EditorPickProviderRegistry::Get().Register(treeTypeId, std::move(pickProvider));

    // Editor-chrome classes (hierarchy/inspector/search icons, tab icon) ship
    // with the package and attach at the editor UI root.
    Editor::EditorPanelRegistry::Get().RegisterEditorStyleSheet(
        {"eztree", "Editor/UI/EZTreeEditorChrome.css"});

    // Tree Stats dockable panel: layout + stylesheet resolve from the package
    // mount; the descriptor mirrors a layout.uxml DockablePanel inventory row.
    Editor::EditorPanelDescriptor statsPanel;
    statsPanel.PanelId = "EZTreeStats";
    statsPanel.Title = "Tree Stats";
    statsPanel.TabIconClass = "eztree-stats-tab-icon";
    statsPanel.AssetSourceAlias = "eztree";
    statsPanel.LayoutAssetPath = "Editor/UI/panels/EZTreeStatsPanel.uxml";
    statsPanel.StyleAssetPath = "Editor/UI/panels/EZTreeStatsPanel.css";
    statsPanel.Factory = []() -> std::unique_ptr<UIElement> {
        return std::make_unique<EZTreeStatsPanel>();
    };
    Editor::EditorPanelRegistry::Get().RegisterPanel(std::move(statsPanel));

    Editor::EditorPluginRegistry::Get().RegisterPlugin(GetPlugin());
}

} // namespace GameEngine::EZTreeEditor
