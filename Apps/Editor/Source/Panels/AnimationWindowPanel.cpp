#include "Panels/AnimationWindowPanel.h"
#include "Panels/Animation/CurveReduction.h"
#include "AssetCore/SharedFileRead.h"
#include "EditorPanelIds.h"
#include "Panels/TimelineBarElement.h"
#include "Panels/ColorPicker.h"
#include "Panels/ConfirmActionModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Panels/RenameLayoutModal.h"
#include "Panels/DopeSheetView.h"
#include "Panels/CurvesGraphView.h"
#include "Panels/TimeCompositeView.h"
#include "Panels/TimeCompositeTimelineBridge.h"
#include "Panels/LaneClipEditorView.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Assets/AssetCreation.h"
#include "AssetCore/GUID.h"
#include "Platform/Shell.h"
#include "Assets/AnimationClip.h"
#include "Assets/TimelineAsset.h"
#include "Assets/ClipSetAsset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "UI/AnimationWindowSettings.h"
#include "UI/EditorIcons.h"
#include "UI/EditorSearchBars.h"
#include "UI/AssetField.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/StyleProperties.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/TreeView.h"
#include "UI/Interaction/FocusIsInside.h"
#include "UI/Interaction/Selection.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIManager.h"
#include "UI/InspectorSection.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>

#if defined(GE_HAVE_UFBX)
  #include <ufbx.h>
#endif

#if defined(GE_HAVE_CGLTF)
  #include <cgltf.h>
#endif

namespace GameEngine
{

namespace
{
constexpr float kKeyTimeEpsilon = 0.0001f;

const char* GetAnimationPanelTitle(AnimationWindowPanel::PanelKind kind)
{
    switch (kind)
    {
    case AnimationWindowPanel::PanelKind::Timeline:
        return "Timeline";
    case AnimationWindowPanel::PanelKind::ClipEditor:
        return "Clip Editor";
    case AnimationWindowPanel::PanelKind::Animation:
    default:
        return "Animation";
    }
}

const char* GetAnimationPanelTabIcon(AnimationWindowPanel::PanelKind kind)
{
    switch (kind)
    {
    case AnimationWindowPanel::PanelKind::Timeline:
        return "film-icon";
    case AnimationWindowPanel::PanelKind::ClipEditor:
        return "dock-clip-editor-icon";
    case AnimationWindowPanel::PanelKind::Animation:
    default:
        return "alarm-icon";
    }
}

const char* GetAnimationPanelId(AnimationWindowPanel::PanelKind kind)
{
    switch (kind)
    {
    case AnimationWindowPanel::PanelKind::Timeline:
        return EditorPanelIds::Timeline;
    case AnimationWindowPanel::PanelKind::ClipEditor:
        return EditorPanelIds::ClipEditor;
    case AnimationWindowPanel::PanelKind::Animation:
    default:
        return EditorPanelIds::Animation;
    }
}

// Find key index by time (within epsilon). Returns keys.size() if not found.
template<typename KeyframeType>
size_t FindKeyIndex(const std::vector<KeyframeType>& keys, float time)
{
    for (size_t index = 0; index < keys.size(); ++index)
    {
        if (std::abs(keys[index].time - time) <= kKeyTimeEpsilon)
            return index;
    }
    return keys.size();
}

// When multiple keys share the same time, return the last one (most recently inserted).
template<typename KeyframeType>
size_t FindLastKeyIndex(const std::vector<KeyframeType>& keys, float time)
{
    size_t found = static_cast<size_t>(-1);
    for (size_t index = 0; index < keys.size(); ++index)
    {
        if (std::abs(keys[index].time - time) <= kKeyTimeEpsilon)
            found = index;
    }
    return found;
}

constexpr uint32_t kCmdAnimationResampleBakedToCurve = 0xA701u;
constexpr uint32_t kCmdAnimationColorTagNone   = 0xA710u;
constexpr uint32_t kCmdAnimationColorTagYellow = 0xA711u;
constexpr uint32_t kCmdAnimationColorTagRed    = 0xA712u;
constexpr uint32_t kCmdAnimationColorTagGreen  = 0xA713u;
constexpr uint32_t kCmdAnimationColorTagBlue   = 0xA714u;

constexpr uint32_t kCmdPreInfConstant       = 0xA720u;
constexpr uint32_t kCmdPreInfLinear         = 0xA721u;
constexpr uint32_t kCmdPreInfCycle          = 0xA722u;
constexpr uint32_t kCmdPreInfCycleOffset    = 0xA723u;
constexpr uint32_t kCmdPreInfOscillate      = 0xA724u;
constexpr uint32_t kCmdPostInfConstant      = 0xA730u;
constexpr uint32_t kCmdPostInfLinear        = 0xA731u;
constexpr uint32_t kCmdPostInfCycle         = 0xA732u;
constexpr uint32_t kCmdPostInfCycleOffset   = 0xA733u;
constexpr uint32_t kCmdPostInfOscillate     = 0xA734u;

constexpr uint32_t kCmdSequencerDeleteTrack = 0xA740u;
constexpr uint32_t kCmdSequencerDeleteLane  = 0xA741u;
constexpr uint32_t kCmdAddTrackProperty   = 0xA750u;
constexpr uint32_t kCmdAddTrackTransform  = 0xA751u;
constexpr uint32_t kCmdAddTrackPosition3D = 0xA752u;
constexpr uint32_t kCmdAddTrackRotation3D = 0xA753u;
constexpr uint32_t kCmdAddTrackScale3D    = 0xA754u;
constexpr uint32_t kCmdAddTrackBlendShape = 0xA755u;
constexpr uint32_t kCmdAddTrackMethod     = 0xA756u;
constexpr uint32_t kCmdAddTrackBezier     = 0xA757u;
constexpr uint32_t kCmdAddTrackAudio      = 0xA758u;
constexpr uint32_t kCmdAddTrackAnimation  = 0xA759u;
constexpr uint32_t kCmdAddTrackEvent      = 0xA75Au;
constexpr uint32_t kCmdAddTrackVideo      = 0xA75Bu;
constexpr uint32_t kCmdTimelineInsertKey = 0xA760u;
constexpr uint32_t kCmdTimelineInsertMarker = 0xA761u;
constexpr uint32_t kCmdTimelineOpenCurveEditor = 0xA762u;
constexpr uint32_t kCmdTimelineRecomputeLinkedOffsets = 0xA763u;
constexpr uint32_t kCmdTimelineRemoveKey = 0xA764u;

float ApplyArithmetic(float currentValue, const std::string& text)
{
    if (text.empty()) return currentValue;
    if (text.size() >= 2 && (text[0] == '+' || text[0] == '-' || text[0] == '*' || text[0] == '/'))
    {
        try
        {
            const float operand = std::stof(text.substr(1));
            switch (text[0])
            {
            case '+': return currentValue + operand;
            case '-': return currentValue - operand;
            case '*': return currentValue * operand;
            case '/': return (std::abs(operand) > 1e-7f) ? currentValue / operand : currentValue;
            }
        }
        catch (...) {}
        return currentValue;
    }
    try { return std::stof(text); }
    catch (...) { return currentValue; }
}

struct AnimationTreeNode
{
    TreeId Id = 0;
    TreeId ParentId = 0;
    String Label;
    bool Expandable = false;
    std::vector<TreeId> Children;
};

class AnimationChannelTreeProvider final : public TreeChangeTrackingProvider
{
  public:
    explicit AnimationChannelTreeProvider(std::vector<AnimationTreeNode> nodes)
        : m_Nodes(std::move(nodes))
    {
        for (size_t index = 0; index < m_Nodes.size(); ++index)
        {
            m_IndexById.emplace(m_Nodes[index].Id, index);
            if (m_Nodes[index].ParentId == 0)
                m_RootIds.push_back(m_Nodes[index].Id);
        }
        MarkAllChanged();
    }

    int GetRootCount() const override { return static_cast<int>(m_RootIds.size()); }

    TreeId GetRootId(int index) const override
    {
        return index >= 0 && static_cast<size_t>(index) < m_RootIds.size() ? m_RootIds[static_cast<size_t>(index)] : 0;
    }

    int GetChildCount(TreeId parent) const override
    {
        const AnimationTreeNode* node = FindNode(parent);
        return node ? static_cast<int>(node->Children.size()) : 0;
    }

    TreeId GetChildId(TreeId parent, int index) const override
    {
        const AnimationTreeNode* node = FindNode(parent);
        if (!node || index < 0 || static_cast<size_t>(index) >= node->Children.size())
        return 0;
        return node->Children[static_cast<size_t>(index)];
    }

    const char* GetLabel(TreeId id) const override
    {
        const AnimationTreeNode* node = FindNode(id);
        return node ? node->Label.c_str() : "";
    }

    bool IsExpandable(TreeId id) const override
    {
        const AnimationTreeNode* node = FindNode(id);
        return node && node->Expandable;
    }

  private:
    const AnimationTreeNode* FindNode(TreeId id) const
    {
        const auto it = m_IndexById.find(id);
        if (it == m_IndexById.end())
            return nullptr;
        return &m_Nodes[it->second];
    }

    std::vector<AnimationTreeNode> m_Nodes;
    std::vector<TreeId> m_RootIds;
    std::unordered_map<TreeId, size_t> m_IndexById;
};

class AnimationSeekCommand final : public Editor::IEditorCommand
{
  public:
    using ApplyFn = std::function<void(float)>;

    AnimationSeekCommand(float beforeTime, float afterTime, ApplyFn applyFn)
        : m_BeforeTime(beforeTime)
        , m_AfterTime(afterTime)
        , m_ApplyFn(std::move(applyFn))
    {
    }

    const char* GetName() const override { return "Move Playhead"; }
    void Do() override {}

    void Undo() override
    {
        if (m_ApplyFn) m_ApplyFn(m_BeforeTime);
    }

    void Redo() override
    {
        if (m_ApplyFn) m_ApplyFn(m_AfterTime);
    }

  private:
    float m_BeforeTime;
    float m_AfterTime;
    ApplyFn m_ApplyFn;
};

class AnimationClipSnapshotCommand final : public Editor::IEditorCommand
{
  public:
    using Snapshot = std::vector<std::uint8_t>;
    using AppliedFn = std::function<void(AnimationClip*)>;

    AnimationClipSnapshotCommand(std::string name,
                                 std::shared_ptr<AnimationClip> clip,
                                 Snapshot beforeSnapshot,
                                 Snapshot afterSnapshot,
                                 AppliedFn onApplied)
        : m_Name(std::move(name))
        , m_Clip(std::move(clip))
        , m_BeforeSnapshot(std::move(beforeSnapshot))
        , m_AfterSnapshot(std::move(afterSnapshot))
        , m_OnApplied(std::move(onApplied))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override
    {
        // Already applied before being committed to the undo stack.
    }

    void Undo() override
    {
        ApplySnapshot(m_BeforeSnapshot);
    }

    void Redo() override
    {
        ApplySnapshot(m_AfterSnapshot);
    }

  private:
    void ApplySnapshot(const Snapshot& snapshot)
    {
        if (!m_Clip)
            return;

        Vector<uint8> data(snapshot.begin(), snapshot.end());
        if (!m_Clip->RestoreFromData(data))
            return;

        if (m_OnApplied)
            m_OnApplied(m_Clip.get());
    }

    std::string m_Name;
    std::shared_ptr<AnimationClip> m_Clip;
    Snapshot m_BeforeSnapshot;
    Snapshot m_AfterSnapshot;
    AppliedFn m_OnApplied;
};

class AnimationPanelSnapshotCommand final : public Editor::IEditorCommand
{
  public:
    AnimationPanelSnapshotCommand(std::string name,
                                  AnimationWindowPanel* panel,
                                  AnimationWindowPanel::PanelEditSnapshot beforeSnapshot,
                                  AnimationWindowPanel::PanelEditSnapshot afterSnapshot,
                                  bool mergeable)
        : m_Name(std::move(name))
        , m_Panel(panel)
        , m_BeforeSnapshot(std::move(beforeSnapshot))
        , m_AfterSnapshot(std::move(afterSnapshot))
        , m_Mergeable(mergeable)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override {}

    void Undo() override
    {
        if (m_Panel)
            m_Panel->ApplyPanelEditSnapshot(m_BeforeSnapshot);
    }

    void Redo() override
    {
        if (m_Panel)
            m_Panel->ApplyPanelEditSnapshot(m_AfterSnapshot);
    }

    bool CanMergeWith(const Editor::IEditorCommand& other) const override
    {
        if (!m_Mergeable)
            return false;
        const auto* otherCommand = dynamic_cast<const AnimationPanelSnapshotCommand*>(&other);
        return otherCommand && otherCommand->m_Mergeable && otherCommand->m_Panel == m_Panel &&
               otherCommand->m_Name == m_Name;
    }

    bool MergeWith(const Editor::IEditorCommand& other) override
    {
        const auto* otherCommand = dynamic_cast<const AnimationPanelSnapshotCommand*>(&other);
        if (!otherCommand || !m_Mergeable || !otherCommand->m_Mergeable || otherCommand->m_Panel != m_Panel ||
            otherCommand->m_Name != m_Name)
        {
            return false;
        }

        m_AfterSnapshot = otherCommand->m_AfterSnapshot;
        return true;
    }

  private:
    std::string m_Name;
    AnimationWindowPanel* m_Panel = nullptr; // not owned
    AnimationWindowPanel::PanelEditSnapshot m_BeforeSnapshot;
    AnimationWindowPanel::PanelEditSnapshot m_AfterSnapshot;
    bool m_Mergeable = false;
};

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(path, bytes))
        return {};
    return bytes;
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    if (path.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

void RegisterAnimationAssetPath(AssetManager* assets, const std::filesystem::path& path)
{
    if (!assets || path.empty())
        return;
    try
    {
        (void)assets->GetRegistry().RegisterAsset(path);
    }
    catch (...)
    {
    }
}

void UnregisterAnimationAssetPath(AssetManager* assets, const std::filesystem::path& path)
{
    if (!assets || path.empty())
        return;
    try
    {
        (void)assets->GetRegistry().TryUnregisterAssetByPath(path);
    }
    catch (...)
    {
    }
}

void RenameAnimationAssetPathInRegistry(AssetManager* assets,
                                        const std::filesystem::path& oldPath,
                                        const std::filesystem::path& newPath)
{
    if (!assets || oldPath.empty() || newPath.empty())
        return;
    try
    {
        if (!assets->GetRegistry().TryRenameAssetPath(oldPath, newPath))
            (void)assets->GetRegistry().RegisterAsset(newPath);
    }
    catch (...)
    {
    }
}

class CopyAnimationAssetFileCommand final : public Editor::IEditorCommand
{
  public:
    CopyAnimationAssetFileCommand(std::filesystem::path source,
                                  std::filesystem::path destination,
                                  std::vector<std::uint8_t> bytes,
                                  AssetManager* assets,
                                  std::function<void()> onChanged)
        : m_Source(std::move(source))
        , m_Destination(std::move(destination))
        , m_Bytes(std::move(bytes))
        , m_Assets(assets)
        , m_OnChanged(std::move(onChanged))
    {
    }

    const char* GetName() const override { return "Duplicate Animation Asset"; }

    void Do() override { WriteDestination(); }
    void Undo() override { RemoveDestination(); }
    void Redo() override { WriteDestination(); }

  private:
    void WriteDestination()
    {
        if (!WriteBinaryFile(m_Destination, m_Bytes))
        {
            Logger::Log::Warning("AnimationWindow: failed to duplicate '{}' to '{}'",
                                 m_Source.string(), m_Destination.string());
            return;
        }
        RegisterAnimationAssetPath(m_Assets, m_Destination);
        if (m_OnChanged) m_OnChanged();
    }

    void RemoveDestination()
    {
        std::error_code ec;
        (void)std::filesystem::remove(m_Destination, ec);
        UnregisterAnimationAssetPath(m_Assets, m_Destination);
        if (m_OnChanged) m_OnChanged();
    }

    std::filesystem::path m_Source;
    std::filesystem::path m_Destination;
    std::vector<std::uint8_t> m_Bytes;
    AssetManager* m_Assets = nullptr;
    std::function<void()> m_OnChanged;
};

class RenameAnimationAssetFileCommand final : public Editor::IEditorCommand
{
  public:
    RenameAnimationAssetFileCommand(std::filesystem::path oldPath,
                                    std::filesystem::path newPath,
                                    AssetManager* assets,
                                    std::function<void(const std::filesystem::path&)> onPathChanged)
        : m_OldPath(std::move(oldPath))
        , m_NewPath(std::move(newPath))
        , m_Assets(assets)
        , m_OnPathChanged(std::move(onPathChanged))
    {
    }

    const char* GetName() const override { return "Rename Animation Asset"; }

    void Do() override { Rename(m_OldPath, m_NewPath); }
    void Undo() override { Rename(m_NewPath, m_OldPath); }
    void Redo() override { Rename(m_OldPath, m_NewPath); }

  private:
    void Rename(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        if (from.empty() || to.empty() || from == to)
            return;
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (ec)
        {
            Logger::Log::Warning("AnimationWindow: failed to rename '{}' to '{}': {}",
                                 from.string(), to.string(), ec.message());
            return;
        }
        RenameAnimationAssetPathInRegistry(m_Assets, from, to);
        if (m_OnPathChanged) m_OnPathChanged(to);
    }

    std::filesystem::path m_OldPath;
    std::filesystem::path m_NewPath;
    AssetManager* m_Assets = nullptr;
    std::function<void(const std::filesystem::path&)> m_OnPathChanged;
};

class DeleteAnimationAssetFileCommand final : public Editor::IEditorCommand
{
  public:
    DeleteAnimationAssetFileCommand(std::filesystem::path path,
                                    std::vector<std::uint8_t> bytes,
                                    AssetManager* assets,
                                    std::function<void(bool)> onDeletedChanged)
        : m_Path(std::move(path))
        , m_Bytes(std::move(bytes))
        , m_Assets(assets)
        , m_OnDeletedChanged(std::move(onDeletedChanged))
    {
    }

    const char* GetName() const override { return "Remove Animation Asset"; }

    void Do() override { Remove(); }
    void Undo() override { Restore(); }
    void Redo() override { Remove(); }

  private:
    void Remove()
    {
        std::error_code ec;
        (void)std::filesystem::remove(m_Path, ec);
        if (ec)
        {
            Logger::Log::Warning("AnimationWindow: failed to remove '{}': {}", m_Path.string(), ec.message());
            return;
        }
        UnregisterAnimationAssetPath(m_Assets, m_Path);
        if (m_OnDeletedChanged) m_OnDeletedChanged(true);
    }

    void Restore()
    {
        if (!WriteBinaryFile(m_Path, m_Bytes))
        {
            Logger::Log::Warning("AnimationWindow: failed to restore removed animation asset '{}'", m_Path.string());
            return;
        }
        RegisterAnimationAssetPath(m_Assets, m_Path);
        if (m_OnDeletedChanged) m_OnDeletedChanged(false);
    }

    std::filesystem::path m_Path;
    std::vector<std::uint8_t> m_Bytes;
    AssetManager* m_Assets = nullptr;
    std::function<void(bool)> m_OnDeletedChanged;
};

struct SourceHierarchyNode
{
    std::string Key;
    std::string ParentKey;
    String Label;
};

struct SourceHierarchyData
{
    std::vector<SourceHierarchyNode> Nodes;
    std::unordered_map<int, std::string> ChannelParentKeys;
};

std::string NormalizeExtension(std::string extension);

bool BuildHierarchyFromGltf(const std::filesystem::path& path,
                            const AnimationClip& clip,
                            SourceHierarchyData& outData)
{
#if defined(GE_HAVE_CGLTF)
    cgltf_options options{};
    cgltf_data* gltf = nullptr;
    if (cgltf_parse_file(&options, path.string().c_str(), &gltf) != cgltf_result_success || !gltf)
        return false;

    outData.Nodes.reserve(gltf->nodes_count);
    for (size_t nodeIndex = 0; nodeIndex < gltf->nodes_count; ++nodeIndex)
    {
        const cgltf_node& node = gltf->nodes[nodeIndex];
        SourceHierarchyNode hierarchyNode;
        hierarchyNode.Key = std::to_string(nodeIndex);
        hierarchyNode.ParentKey = node.parent ? std::to_string(static_cast<size_t>(node.parent - gltf->nodes)) : "";
        hierarchyNode.Label = node.name ? node.name : ("Node " + std::to_string(nodeIndex));
        outData.Nodes.push_back(std::move(hierarchyNode));
    }

    const std::vector<AnimChannel>& channels = clip.GetChannels();
    for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
    {
        const uint32 boneIndex = channels[channelIndex].boneIndex;
        if (static_cast<size_t>(boneIndex) < gltf->nodes_count)
            outData.ChannelParentKeys.emplace(static_cast<int>(channelIndex), std::to_string(boneIndex));
    }

    cgltf_free(gltf);
    return !outData.Nodes.empty();
#else
    (void)path;
    (void)clip;
    (void)outData;
    return false;
#endif
}

bool BuildHierarchyFromFbx(const std::filesystem::path& path,
                           const AnimationClip& clip,
                           SourceHierarchyData& outData)
{
#if defined(GE_HAVE_UFBX)
    ufbx_load_opts opts{};
    opts.ignore_geometry = true;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    opts.load_external_files = false;
    opts.ignore_missing_external_files = true;

    const std::string pathStr = path.string();
    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_file(pathStr.c_str(), &opts, &err);
    if (!scene)
        return false;

    // Walk depth-first from the root node so the resulting hierarchy
    // matches what the user sees in DCC tools.
    std::unordered_map<std::string, std::string> nodeKeyByName;
    std::function<void(const ufbx_node*, const std::string&)> visitNode =
        [&](const ufbx_node* node, const std::string& parentKey)
    {
        if (!node)
            return;

        std::string nodeName = node->name.length > 0
            ? std::string(node->name.data, node->name.length)
            : std::string{};
        if (nodeName.empty())
            nodeName = "Node";
        const std::string nodeKey = parentKey.empty() ? nodeName : (parentKey + "/" + nodeName);

        SourceHierarchyNode hierarchyNode;
        hierarchyNode.Key = nodeKey;
        hierarchyNode.ParentKey = parentKey;
        hierarchyNode.Label = nodeName;
        outData.Nodes.push_back(std::move(hierarchyNode));
        nodeKeyByName.emplace(nodeName, nodeKey);

        for (size_t ci = 0; ci < node->children.count; ++ci)
            visitNode(node->children.data[ci], nodeKey);
    };
    visitNode(scene->root_node, "");

    const std::vector<AnimChannel>& channels = clip.GetChannels();
    for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
    {
        const auto it = nodeKeyByName.find(channels[channelIndex].targetName);
        if (it != nodeKeyByName.end())
            outData.ChannelParentKeys.emplace(static_cast<int>(channelIndex), it->second);
    }

    ufbx_free_scene(scene);
    return !outData.Nodes.empty();
#else
    (void)path;
    (void)clip;
    (void)outData;
    return false;
#endif
}

bool BuildSourceHierarchy(const std::filesystem::path& path,
                          const AnimationClip& clip,
                          SourceHierarchyData& outData)
{
    outData = {};
    if (path.empty())
        return false;

    const std::string extension = NormalizeExtension(path.extension().string());
    if (extension == ".gltf" || extension == ".glb")
        return BuildHierarchyFromGltf(path, clip, outData);
    if (extension == ".fbx")
        return BuildHierarchyFromFbx(path, clip, outData);
    return false;
}

std::string NormalizeExtension(std::string extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// Standard multi-select model — supports Shift+click range, Cmd/Ctrl+click toggle,
// single click, and keyboard navigation, matching the Hierarchy panel behaviour.
using AnimatablePropertiesSelection = UI::Interaction::SelectionModel;

void ClearElementChildren(UIElement* element)
{
    if (!element)
        return;

    std::vector<UIElement*> children;
    children.reserve(element->GetChildren().size());
    for (const auto& child : element->GetChildren())
        children.push_back(child.get());
    for (UIElement* child : children)
        element->RemoveChild(child);
}

Label* AddSidebarLabel(UIElement* parent, const std::string& text, const char* className)
{
    if (!parent)
        return nullptr;

    auto label = std::make_unique<Label>();
    Label* labelPtr = label.get();
    if (className && className[0] != '\0')
        labelPtr->AddClass(className);
    labelPtr->SetText(text);
    parent->AddChild(std::move(label));
    return labelPtr;
}

Button* AddSidebarButton(UIElement* parent,
                         const std::string& text,
                         bool active,
                         const UIElement::EventHandler& onClick)
{
    if (!parent)
        return nullptr;

    auto button = std::make_unique<Button>();
    Button* buttonPtr = button.get();
    buttonPtr->AddClass("small");
    buttonPtr->AddClass("secondary");
    buttonPtr->AddClass("animationwindow-sequencer-select-button");
    if (active)
        buttonPtr->AddClass("active");
    buttonPtr->SetText(text);
    buttonPtr->RegisterEventHandler(kEventButtonClick, onClick);
    parent->AddChild(std::move(button));
    return buttonPtr;
}

std::string SidebarTrackDisplayName(const std::string& fullName)
{
    constexpr size_t kTailChars = 12u;
    if (fullName.size() <= kTailChars + 3u)
        return fullName;
    return "..." + fullName.substr(fullName.size() - kTailChars);
}

std::string ClipDropdownDisplayName(const std::string& fullName)
{
    constexpr size_t kTailChars = 12u;
    if (fullName.size() <= kTailChars + 3u)
        return fullName;
    return "..." + fullName.substr(fullName.size() - kTailChars);
}

std::string DefaultTrackName(CompositeTrackType type, size_t index)
{
    return std::string(CompositeTrackTypeDisplayName(type)) + " " + std::to_string(index + 1u);
}

CompositeTrack MakeDefaultCompositeTrack(CompositeTrackType type, size_t index, float currentTime)
{
    CompositeTrack track;
    track.type = type;
    track.name = DefaultTrackName(type, index);
    track.targetPath = "Node";
    track.propertyPath = "";

    if (CompositeTrackTypeUsesValueKeys(type))
    {
        switch (type)
        {
        case CompositeTrackType::Property:
            track.propertyPath = "property";
            break;
        case CompositeTrackType::Position3D:
            track.propertyPath = "position";
            break;
        case CompositeTrackType::Rotation3D:
            track.propertyPath = "rotation";
            break;
        case CompositeTrackType::Scale3D:
            track.propertyPath = "scale";
            break;
        case CompositeTrackType::BlendShape:
            track.propertyPath = "blend_shapes/Shape";
            break;
        case CompositeTrackType::Bezier:
            track.propertyPath = "curve";
            break;
        default:
            break;
        }
        CompositeValueKey key;
        key.time = std::max(0.0f, currentTime);
        key.componentCount = CompositeTrackTypeComponentCount(type);
        key.label = "Key";
        if (type == CompositeTrackType::Scale3D)
        {
            key.value[0] = 1.0f;
            key.value[1] = 1.0f;
            key.value[2] = 1.0f;
        }
        else if (type == CompositeTrackType::Rotation3D)
        {
            key.value[3] = 1.0f;
        }
        track.valueKeys.push_back(key);
    }
    else if (type == CompositeTrackType::Method)
    {
        track.propertyPath = "method";
        track.methodKeys.push_back({std::max(0.0f, currentTime), "method", ""});
    }
    else if (type == CompositeTrackType::Event)
    {
        track.propertyPath = "event";
        track.methodKeys.push_back({std::max(0.0f, currentTime), "event", ""});
    }
    else if (type == CompositeTrackType::Audio)
    {
        track.propertyPath = "audio";
        CompositeAudioKey key;
        key.time = std::max(0.0f, currentTime);
        key.name = "Audio";
        track.audioKeys.push_back(std::move(key));
    }
    else if (type == CompositeTrackType::Animation)
    {
        track.propertyPath = "animation";
        CompositeAnimationKey key;
        key.time = std::max(0.0f, currentTime);
        key.animationName = "Animation";
        track.animationKeys.push_back(std::move(key));
    }
    else if (type == CompositeTrackType::Video)
    {
        track.propertyPath = "video";
    }

    return track;
}

CompositeTrackType TrackTypeFromAddCommand(uint32_t cmd)
{
    switch (cmd)
    {
    case kCmdAddTrackAudio:      return CompositeTrackType::Audio;
    case kCmdAddTrackAnimation:  return CompositeTrackType::Animation;
    case kCmdAddTrackVideo:      return CompositeTrackType::Video;
    case kCmdAddTrackEvent:      return CompositeTrackType::Event;
    case kCmdAddTrackPosition3D: return CompositeTrackType::Position3D;
    case kCmdAddTrackRotation3D: return CompositeTrackType::Rotation3D;
    case kCmdAddTrackScale3D:    return CompositeTrackType::Scale3D;
    case kCmdAddTrackBlendShape: return CompositeTrackType::BlendShape;
    case kCmdAddTrackMethod:     return CompositeTrackType::Method;
    case kCmdAddTrackBezier:     return CompositeTrackType::Bezier;
    case kCmdAddTrackProperty:
    default:                     return CompositeTrackType::Property;
    }
}

Button* AddSidebarEnableButton(UIElement* parent,
                               bool enabled,
                               std::function<void(bool)> onChanged)
{
    if (!parent)
        return nullptr;

    auto button = std::make_unique<Button>();
    Button* buttonPtr = button.get();
    buttonPtr->AddClass("small");
    buttonPtr->AddClass("secondary");
    buttonPtr->AddClass("icon-button");
    buttonPtr->AddClass("eye-icon");
    buttonPtr->AddClass("animationwindow-sequencer-enable-button");
    if (enabled)
    {
        buttonPtr->AddClass("active");
        buttonPtr->AddClass("icon-active");
    }
    else
    {
        buttonPtr->AddClass("muted");
    }
    buttonPtr->SetText("");
    buttonPtr->SetTooltip(enabled ? "Enabled" : "Disabled");
    buttonPtr->RegisterEventHandler(kEventButtonClick, [enabled, onChanged = std::move(onChanged)](UIEvent& e)
    {
        UIElement& button = *e.CurrentTarget;
        if (enabled)
        {
            button.RemoveClass("active");
            button.RemoveClass("icon-active");
            button.AddClass("muted");
            button.SetTooltip("Disabled");
        }
        else
        {
            button.RemoveClass("muted");
            button.AddClass("active");
            button.AddClass("icon-active");
            button.SetTooltip("Enabled");
        }
        if (onChanged)
            onChanged(!enabled);
    });
    parent->AddChild(std::move(button));
    return buttonPtr;
}

UIElement* AddTimelineInspectorSection(UIElement* parent, const std::string& title)
{
    if (!parent)
        return nullptr;

    auto section = std::make_unique<InspectorSection>(title);
    UIElement* content = section->GetContentRoot();
    parent->AddChild(std::move(section));
    return content;
}

Label* AddTimelineInspectorRow(UIElement* parent, const std::string& labelText, const char* tooltip = nullptr)
{
    if (!parent)
        return nullptr;
    UIElement* row = InspectorUI::AddRow(parent);
    return InspectorUI::AddLabel(row, labelText, tooltip);
}

UIElement* RowFromTimelineInspectorLabel(Label* label)
{
    if (!label)
        return nullptr;
    if (UIElement* cell = label->GetParent())
        return cell->GetParent();
    return nullptr;
}

TextField* AddTimelineInspectorText(UIElement* parent,
                                    const std::string& labelText,
                                    const std::string& value,
                                    Field<std::string>::ValueCallback onChanged,
                                    const char* tooltip = nullptr)
{
    Label* label = AddTimelineInspectorRow(parent, labelText, tooltip);
    UIElement* row = RowFromTimelineInspectorLabel(label);
    if (!row)
        return nullptr;
    auto field = std::make_unique<TextField>();
    TextField* raw = field.get();
    raw->SetValue(value);
    if (tooltip)
        raw->SetTooltip(tooltip);
    raw->SetOnValueChanged(std::move(onChanged));
    if (UIElement* fieldContainer = InspectorUI::AddFieldContainer(row))
        fieldContainer->AddChild(std::move(field));
    return raw;
}

FloatField* AddTimelineInspectorFloat(UIElement* parent,
                                      const std::string& labelText,
                                      float value,
                                      std::function<void(float)> onPreview,
                                      std::function<void(float)> onCommit,
                                      float resetValue = std::numeric_limits<float>::quiet_NaN(),
                                      const char* tooltip = nullptr,
                                      float minValue = -std::numeric_limits<float>::infinity(),
                                      float maxValue = std::numeric_limits<float>::infinity())
{
    Label* label = AddTimelineInspectorRow(parent, labelText, tooltip);
    UIElement* row = RowFromTimelineInspectorLabel(label);
    if (!row || !label)
        return nullptr;

    auto field = std::make_unique<FloatField>();
    FloatField* raw = field.get();
    raw->SetValue(value);
    if (tooltip)
        raw->SetTooltip(tooltip);

    auto previewCopy = std::move(onPreview);
    auto commitCopy = std::move(onCommit);
    raw->SetOnValueChanging([previewCopy](const float& v)
    {
        if (previewCopy)
            previewCopy(v);
    });
    raw->SetOnValueChanged([commitCopy](const float& v)
    {
        if (commitCopy)
            commitCopy(v);
    });

    if (UIElement* fieldContainer = InspectorUI::AddFieldContainer(row))
        fieldContainer->AddChild(std::move(field));

    const float resetTo = std::isnan(resetValue) ? value : resetValue;
    InspectorDrag::SetupLabelDragFloat(
        label,
        raw,
        [previewCopy, raw]()
        {
            if (previewCopy)
                previewCopy(raw->GetValue());
        },
        [commitCopy, raw]()
        {
            if (commitCopy)
                commitCopy(raw->GetValue());
        },
        resetTo,
        minValue,
        maxValue);

    return raw;
}

Checkbox* AddTimelineInspectorCheckbox(UIElement* parent,
                                       const std::string& labelText,
                                       bool value,
                                       Field<bool>::ValueCallback onChanged,
                                       const char* tooltip = nullptr)
{
    Label* label = AddTimelineInspectorRow(parent, labelText, tooltip);
    UIElement* row = RowFromTimelineInspectorLabel(label);
    if (!row)
        return nullptr;
    auto field = std::make_unique<Checkbox>();
    Checkbox* raw = field.get();
    raw->SetChecked(value);
    if (tooltip)
        raw->SetTooltip(tooltip);
    raw->SetOnValueChanged(std::move(onChanged));
    if (UIElement* fieldContainer = InspectorUI::AddFieldContainer(row))
    {
        fieldContainer->AddClass("inspector-field-toggle");
        fieldContainer->AddChild(std::move(field));
    }
    return raw;
}

Dropdown* AddTimelineInspectorDropdown(UIElement* parent,
                                       const std::string& labelText,
                                       const std::vector<Dropdown::Option>& options,
                                       const std::string& value,
                                       Field<std::string>::ValueCallback onChanged,
                                       const char* tooltip = nullptr)
{
    Label* label = AddTimelineInspectorRow(parent, labelText, tooltip);
    UIElement* row = RowFromTimelineInspectorLabel(label);
    if (!row)
        return nullptr;
    auto field = std::make_unique<Dropdown>();
    Dropdown* raw = field.get();
    raw->AddClass("inspector-dropdown");
    if (tooltip)
        raw->SetTooltip(tooltip);
    int selected = 0;
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].value == value)
        {
            selected = static_cast<int>(i);
            break;
        }
    }
    raw->SetOptions(options, selected);
    raw->SetOnValueChanged(std::move(onChanged));
    if (UIElement* fieldContainer = InspectorUI::AddFieldContainer(row))
        fieldContainer->AddChild(std::move(field));
    return raw;
}

AssetField* AddTimelineInspectorAsset(UIElement* parent,
                                      const std::string& labelText,
                                      const GUID& value,
                                      const std::vector<AssetType>& acceptedTypes,
                                      AssetRegistry* registry,
                                      AssetField::ValueChangedCallback onChanged,
                                      const char* tooltip = nullptr)
{
    Label* label = AddTimelineInspectorRow(parent, labelText, tooltip);
    UIElement* row = RowFromTimelineInspectorLabel(label);
    if (!row)
        return nullptr;
    auto field = std::make_unique<AssetField>();
    AssetField* raw = field.get();
    raw->SetAcceptedTypes(acceptedTypes);
    raw->SetAssetRegistry(registry);
    raw->SetValue(value);
    if (tooltip)
        raw->SetTooltip(tooltip);
    raw->SetOnValueChanged(std::move(onChanged));
    if (UIElement* fieldContainer = InspectorUI::AddFieldContainer(row))
        fieldContainer->AddChild(std::move(field));
    return raw;
}

std::string FormatTypedArguments(const std::vector<CompositeMethodArgument>& args)
{
    std::string out;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0) out += ", ";
        out += args[i].type;
        out += ":";
        out += args[i].value;
    }
    return out;
}

std::vector<CompositeMethodArgument> ParseTypedArguments(std::string text)
{
    std::vector<CompositeMethodArgument> result;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t comma = text.find(',', pos);
        std::string part = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const auto trim = [](std::string s)
        {
            const auto first = s.find_first_not_of(" \t");
            const auto last = s.find_last_not_of(" \t");
            if (first == std::string::npos) return std::string{};
            return s.substr(first, last - first + 1);
        };
        part = trim(part);
        if (!part.empty())
        {
            const size_t colon = part.find(':');
            CompositeMethodArgument arg;
            if (colon == std::string::npos)
            {
                arg.type = "string";
                arg.value = part;
            }
            else
            {
                arg.type = trim(part.substr(0, colon));
                arg.value = trim(part.substr(colon + 1));
                if (arg.type.empty()) arg.type = "string";
            }
            result.push_back(std::move(arg));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

UIElement* AddSidebarCard(UIElement* parent)
{
    if (!parent)
        return nullptr;

    auto card = std::make_unique<UIElement>();
    UIElement* cardPtr = card.get();
    cardPtr->AddClass("animationwindow-sequencer-card");
    parent->AddChild(std::move(card));
    return cardPtr;
}

// Small clickable color square for the sequencer sidebar. argb==0 renders as a checker-stub
// (subtle outlined square) so the user can still click it to assign an initial color.
UIElement* AddSidebarColorSwatch(UIElement* parent,
                                 uint32_t argb,
                                 std::function<void()> onClick)
{
    if (!parent)
        return nullptr;

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchPtr = swatch.get();
    swatchPtr->AddClass("animationwindow-sequencer-color-swatch");

    constexpr float kSwatchSize = 20.0f;
    constexpr uint32_t kEmptySwatchFill = kDefaultSequencerRowColorArgb;
    constexpr uint32_t kSwatchBorder    = 0xFF555555U;
    const uint32_t fill = (argb == 0u) ? kEmptySwatchFill : (0xFF000000u | (argb & 0x00FFFFFFu));
    swatchPtr->Overrides()
        .Set(Style::Width,  StyleLength::Px(kSwatchSize))
        .Set(Style::Height, StyleLength::Px(kSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kSwatchSize))
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kSwatchBorder, kSwatchBorder, kSwatchBorder, kSwatchBorder})
        .Set(Style::BackgroundColor, fill)
        .Set(Style::Cursor, CursorStyle::Pointer);
    swatchPtr->RegisterEventHandler(kEventMouseDown, [onClick = std::move(onClick)](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        e.Stop();
        if (onClick)
            onClick();
    });
    parent->AddChild(std::move(swatch));
    return swatchPtr;
}

// ---------- Tangent computation helpers ----------

uint32 AnimChannelComponentCount(const AnimChannel& ch)
{
    if (ch.path == AnimPath::MorphWeight)
        return 1u;
    return (ch.path == AnimPath::Rotation) ? 4u : 3u;
}

float GetKeyComponentValue(const AnimKeyframe& key, AnimPath path, uint32 comp)
{
    switch (path)
    {
    case AnimPath::Translation: return key.translation[comp];
    case AnimPath::Rotation:    return key.rotation[comp];
    case AnimPath::Scale:       return key.scale[comp];
    case AnimPath::MorphWeight: return key.translation[0];
    }
    return 0.0f;
}

// Catmull-Rom slope with anti-overshoot clamp.
float ComputeTangentAuto(const AnimChannel& ch, size_t ki, uint32 c)
{
    const auto& keys = ch.keys;
    const size_t n = keys.size();
    if (n <= 1)
        return 0.0f;

    const float v  = GetKeyComponentValue(keys[ki],     ch.path, c);
    const float v0 = (ki > 0)     ? GetKeyComponentValue(keys[ki - 1], ch.path, c) : v;
    const float v1 = (ki + 1 < n) ? GetKeyComponentValue(keys[ki + 1], ch.path, c) : v;

    if (ki == 0)
        return (n >= 2) ? (v1 - v) / std::max(1e-6f, keys[1].time - keys[0].time) : 0.0f;
    if (ki + 1 == n)
        return (v - v0) / std::max(1e-6f, keys[ki].time - keys[ki - 1].time);

    const float s0 = (v  - v0) / std::max(1e-6f, keys[ki].time     - keys[ki - 1].time);
    const float s1 = (v1 - v)  / std::max(1e-6f, keys[ki + 1].time - keys[ki].time);
    // Local extremum → flat tangent
    if (s0 * s1 <= 0.0f)
        return 0.0f;
    // Catmull-Rom slope, clamped to ≤3× each segment slope to prevent overshoot
    const float dt = std::max(1e-6f, keys[ki + 1].time - keys[ki - 1].time);
    const float slope = (v1 - v0) / dt;
    const float limit = 3.0f * std::min(std::abs(s0), std::abs(s1));
    return std::clamp(slope, -limit, limit);
}

// Auto but forced flat at local extrema (peaks/valleys).
float ComputeTangentPlateau(const AnimChannel& ch, size_t ki, uint32 c)
{
    const auto& keys = ch.keys;
    const size_t n = keys.size();
    if (n <= 1)
        return 0.0f;

    const float v  = GetKeyComponentValue(keys[ki],     ch.path, c);
    const float v0 = (ki > 0)     ? GetKeyComponentValue(keys[ki - 1], ch.path, c) : v;
    const float v1 = (ki + 1 < n) ? GetKeyComponentValue(keys[ki + 1], ch.path, c) : v;

    // Flat at any local extremum (not just sign-change — compare against both neighbors)
    if (ki > 0 && ki + 1 < n)
    {
        const bool isPeak   = v >= v0 && v >= v1;
        const bool isValley = v <= v0 && v <= v1;
        if (isPeak || isValley)
            return 0.0f;
    }
    return ComputeTangentAuto(ch, ki, c);
}

// Spline slope, but switches to linear when adjacent values are very close.
float ComputeTangentClamped(const AnimChannel& ch, size_t ki, uint32 c, float fullValueRange)
{
    const auto& keys = ch.keys;
    const size_t n = keys.size();
    if (n <= 1)
        return 0.0f;

    const float v  = GetKeyComponentValue(keys[ki],     ch.path, c);
    const float v0 = (ki > 0)     ? GetKeyComponentValue(keys[ki - 1], ch.path, c) : v;
    const float v1 = (ki + 1 < n) ? GetKeyComponentValue(keys[ki + 1], ch.path, c) : v;
    const float threshold = 0.05f * std::max(1e-6f, fullValueRange);

    const float autoSlope = ComputeTangentAuto(ch, ki, c);

    if (ki > 0 && std::abs(v - v0) < threshold)
    {
        const float linSlope = (v - v0) / std::max(1e-6f, keys[ki].time - keys[ki - 1].time);
        return std::abs(autoSlope) < std::abs(linSlope) ? autoSlope : linSlope;
    }
    if (ki + 1 < n && std::abs(v1 - v) < threshold)
    {
        const float linSlope = (v1 - v) / std::max(1e-6f, keys[ki + 1].time - keys[ki].time);
        return std::abs(autoSlope) < std::abs(linSlope) ? autoSlope : linSlope;
    }
    return autoSlope;
}

float ComputeTangentLinear(const AnimChannel& ch, size_t ki, uint32 c, bool incoming)
{
    const auto& keys = ch.keys;
    const size_t n = keys.size();
    if (n <= 1)
        return 0.0f;

    const float v = GetKeyComponentValue(keys[ki], ch.path, c);
    if (incoming && ki > 0)
    {
        const float v0 = GetKeyComponentValue(keys[ki - 1], ch.path, c);
        return (v - v0) / std::max(1e-6f, keys[ki].time - keys[ki - 1].time);
    }
    if (!incoming && ki + 1 < n)
    {
        const float v1 = GetKeyComponentValue(keys[ki + 1], ch.path, c);
        return (v1 - v) / std::max(1e-6f, keys[ki + 1].time - keys[ki].time);
    }
    return 0.0f;
}

// Compute the full value range for a component across all keys in the channel.
float ComputeChannelValueRange(const AnimChannel& ch, uint32 c)
{
    if (ch.keys.empty())
        return 1.0f;
    float mn = GetKeyComponentValue(ch.keys[0], ch.path, c);
    float mx = mn;
    for (const auto& k : ch.keys)
    {
        const float v = GetKeyComponentValue(k, ch.path, c);
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    return std::max(1e-6f, mx - mn);
}

} // namespace

void AnimationWindowPanel::RefreshLeftPane()
{
    const bool showSequencerSidebar =
        m_ActiveView == ActiveView::TimeComposite || m_ActiveView == ActiveView::ClipEditor;
    if (m_PropertiesTree)
    {
        if (showSequencerSidebar)
            m_PropertiesTree->AddClass("animationwindow-sidebar-hidden");
        else
            m_PropertiesTree->RemoveClass("animationwindow-sidebar-hidden");
    }

    if (m_SequencerSidebar)
    {
        if (showSequencerSidebar)
            m_SequencerSidebar->RemoveClass("animationwindow-sidebar-hidden");
        else
            m_SequencerSidebar->AddClass("animationwindow-sidebar-hidden");
    }

    if (showSequencerSidebar)
        RefreshSequencerSidebar();
    RefreshTimelineInspector();
}

void AnimationWindowPanel::SelectCompositeClip(size_t trackIdx, size_t clipIdx)
{
    m_SelectedCompositeTrack = trackIdx;
    m_SelectedCompositeClip = clipIdx;
    m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
    m_SelectedTimelineKey = static_cast<size_t>(-1);
    if (m_OnClipSourcePreview && m_CompositeModel &&
        trackIdx < m_CompositeModel->tracks.size())
    {
        const auto& clips = m_CompositeModel->tracks[trackIdx].clips;
        if (clipIdx < clips.size() && !clips[clipIdx].sourcePath.empty())
            m_OnClipSourcePreview(clips[clipIdx].sourcePath);
    }
    RefreshSequencerSidebar();
    RefreshTimelineInspector();
    if (m_ActiveView == ActiveView::TimeComposite)
        RefreshClipDropdown();
}

void AnimationWindowPanel::SelectLaneClip(size_t laneIdx, size_t clipIdx)
{
    m_SelectedLane = laneIdx;
    m_SelectedLaneClip = clipIdx;
    RefreshSequencerSidebar();
    if (m_ActiveView == ActiveView::ClipEditor)
        RefreshClipDropdown();
}

size_t AnimationWindowPanel::InsertTimelineTrackKeyAt(size_t trackIdx, float time)
{
    if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
        return static_cast<size_t>(-1);

    const CompositeTrackType trackType = m_CompositeModel->tracks[trackIdx].type;
    const float keyTime = std::max(0.0f, time);
    const auto armInsertedKey = [this, trackIdx](CompositeTrackType keyType, size_t keyIdx)
    {
        if (!m_TimeCompositeView || keyIdx == static_cast<size_t>(-1))
            return;

        m_TimeCompositeView->ArmTrackKeyForImmediateDrag(trackIdx, keyType, keyIdx);
    };

    if (CompositeTrackTypeUsesValueKeys(trackType))
    {
        size_t insertedKeyIdx = static_cast<size_t>(-1);
        const bool inserted = ExecutePanelEditWithUndo("Add Track Key", [this, trackIdx, keyTime, &insertedKeyIdx]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            auto& track = m_CompositeModel->tracks[trackIdx];
            if (!CompositeTrackTypeUsesValueKeys(track.type))
                return false;

            CompositeValueKey key;
            key.time = keyTime;
            key.componentCount = CompositeTrackTypeComponentCount(track.type);
            key.label = "Key";
            if (track.type == CompositeTrackType::Scale3D)
            {
                key.value[0] = 1.0f;
                key.value[1] = 1.0f;
                key.value[2] = 1.0f;
            }
            else if (track.type == CompositeTrackType::Rotation3D)
            {
                key.value[3] = 1.0f;
            }

            track.valueKeys.push_back(key);
            std::sort(track.valueKeys.begin(), track.valueKeys.end(),
                      [](const CompositeValueKey& a, const CompositeValueKey& b) { return a.time < b.time; });

            insertedKeyIdx = FindLastKeyIndex(track.valueKeys, keyTime);
            if (insertedKeyIdx == static_cast<size_t>(-1))
                insertedKeyIdx = track.valueKeys.empty() ? static_cast<size_t>(-1) : track.valueKeys.size() - 1u;

            if (m_TimeCompositeView)
                m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            RefreshTimelineInspector();
            UIElement::MarkDirty(UIElement::LayoutDirty);
            UpdateTimelineViewHeaderOffsets();
            return true;
        }, false);
        if (inserted)
            armInsertedKey(CompositeTrackType::Property, insertedKeyIdx);
        return inserted ? insertedKeyIdx : static_cast<size_t>(-1);
    }

    if (CompositeTrackTypeUsesMethodKeys(trackType))
    {
        size_t insertedKeyIdx = static_cast<size_t>(-1);
        const bool inserted = ExecutePanelEditWithUndo(trackType == CompositeTrackType::Event ? "Add Event Key" : "Add Method Call",
            [this, trackIdx, keyTime, &insertedKeyIdx]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            auto& track = m_CompositeModel->tracks[trackIdx];
            if (!CompositeTrackTypeUsesMethodKeys(track.type))
                return false;

            track.methodKeys.push_back({keyTime, track.type == CompositeTrackType::Event ? "event" : "method", ""});
            std::sort(track.methodKeys.begin(), track.methodKeys.end(),
                      [](const CompositeMethodKey& a, const CompositeMethodKey& b) { return a.time < b.time; });

            insertedKeyIdx = FindLastKeyIndex(track.methodKeys, keyTime);
            if (insertedKeyIdx == static_cast<size_t>(-1))
                insertedKeyIdx = track.methodKeys.empty() ? static_cast<size_t>(-1) : track.methodKeys.size() - 1u;

            if (m_TimeCompositeView)
                m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            RefreshTimelineInspector();
            UIElement::MarkDirty(UIElement::LayoutDirty);
            UpdateTimelineViewHeaderOffsets();
            return true;
        }, false);
        if (inserted)
            armInsertedKey(CompositeTrackType::Method, insertedKeyIdx);
        return inserted ? insertedKeyIdx : static_cast<size_t>(-1);
    }

    if (CompositeTrackTypeUsesAudioKeys(trackType))
    {
        size_t insertedKeyIdx = static_cast<size_t>(-1);
        const bool inserted = ExecutePanelEditWithUndo("Add Audio Key", [this, trackIdx, keyTime, &insertedKeyIdx]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            auto& track = m_CompositeModel->tracks[trackIdx];
            if (!CompositeTrackTypeUsesAudioKeys(track.type))
                return false;

            CompositeAudioKey key;
            key.time = keyTime;
            key.name = "Audio";
            track.audioKeys.push_back(std::move(key));
            std::sort(track.audioKeys.begin(), track.audioKeys.end(),
                      [](const CompositeAudioKey& a, const CompositeAudioKey& b) { return a.time < b.time; });

            insertedKeyIdx = FindLastKeyIndex(track.audioKeys, keyTime);
            if (insertedKeyIdx == static_cast<size_t>(-1))
                insertedKeyIdx = track.audioKeys.empty() ? static_cast<size_t>(-1) : track.audioKeys.size() - 1u;

            if (m_TimeCompositeView)
                m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            RefreshTimelineInspector();
            UIElement::MarkDirty(UIElement::LayoutDirty);
            UpdateTimelineViewHeaderOffsets();
            return true;
        }, false);
        if (inserted)
            armInsertedKey(CompositeTrackType::Audio, insertedKeyIdx);
        return inserted ? insertedKeyIdx : static_cast<size_t>(-1);
    }

    if (CompositeTrackTypeUsesAnimationKeys(trackType))
    {
        size_t insertedKeyIdx = static_cast<size_t>(-1);
        const bool inserted = ExecutePanelEditWithUndo("Add Animation Key", [this, trackIdx, keyTime, &insertedKeyIdx]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            auto& track = m_CompositeModel->tracks[trackIdx];
            if (!CompositeTrackTypeUsesAnimationKeys(track.type))
                return false;

            CompositeAnimationKey key;
            key.time = keyTime;
            key.animationName = "Animation";
            track.animationKeys.push_back(std::move(key));
            std::sort(track.animationKeys.begin(), track.animationKeys.end(),
                      [](const CompositeAnimationKey& a, const CompositeAnimationKey& b) { return a.time < b.time; });

            insertedKeyIdx = FindLastKeyIndex(track.animationKeys, keyTime);
            if (insertedKeyIdx == static_cast<size_t>(-1))
                insertedKeyIdx = track.animationKeys.empty() ? static_cast<size_t>(-1) : track.animationKeys.size() - 1u;

            if (m_TimeCompositeView)
                m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            RefreshTimelineInspector();
            UIElement::MarkDirty(UIElement::LayoutDirty);
            UpdateTimelineViewHeaderOffsets();
            return true;
        }, false);
        if (inserted)
            armInsertedKey(CompositeTrackType::Animation, insertedKeyIdx);
        return inserted ? insertedKeyIdx : static_cast<size_t>(-1);
    }

    return static_cast<size_t>(-1);
}

bool AnimationWindowPanel::InsertTimelineTrackMarkerAt(size_t trackIdx, float time)
{
    if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
        return false;

    const float markerTime = std::max(0.0f, time);
    return ExecutePanelEditWithUndo("Add Track Marker", [this, trackIdx, markerTime]()
    {
        if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
            return false;
        m_CompositeModel->tracks[trackIdx].markers.push_back({markerTime, ""});
        m_SelectedCompositeTrack = trackIdx;
        m_SelectedCompositeClip = static_cast<size_t>(-1);
        if (m_TimeCompositeView)
            m_TimeCompositeView->MarkDirty(VisualDirty);
        RefreshSequencerSidebar();
        RefreshTimelineInspector();
        return true;
    }, false);
}

bool AnimationWindowPanel::MoveTimelineTrackInTime(size_t trackIdx, float deltaSeconds, bool refreshSidebar)
{
    if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size() || deltaSeconds == 0.0f)
        return false;

    auto applyMove = [this, trackIdx, deltaSeconds, refreshSidebar]()
    {
        if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
            return false;

        auto& track = m_CompositeModel->tracks[trackIdx];
        float earliest = std::numeric_limits<float>::max();
        auto includeTime = [&earliest](float time) { earliest = std::min(earliest, time); };
        for (const auto& clip : track.clips)
            includeTime(clip.offsetOnTimeline);
        for (const auto& key : track.valueKeys)
            includeTime(key.time);
        for (const auto& key : track.methodKeys)
            includeTime(key.time);
        for (const auto& key : track.audioKeys)
            includeTime(key.time);
        for (const auto& key : track.animationKeys)
            includeTime(key.time);
        for (const auto& marker : track.markers)
            includeTime(marker.time);
        if (earliest == std::numeric_limits<float>::max())
            return false;

        const float clampedDelta = std::max(deltaSeconds, -earliest);
        if (clampedDelta == 0.0f)
            return false;

        for (auto& clip : track.clips)
            clip.offsetOnTimeline = std::max(0.0f, clip.offsetOnTimeline + clampedDelta);
        for (auto& key : track.valueKeys)
            key.time = std::max(0.0f, key.time + clampedDelta);
        for (auto& key : track.methodKeys)
            key.time = std::max(0.0f, key.time + clampedDelta);
        for (auto& key : track.audioKeys)
            key.time = std::max(0.0f, key.time + clampedDelta);
        for (auto& key : track.animationKeys)
            key.time = std::max(0.0f, key.time + clampedDelta);
        for (auto& marker : track.markers)
            marker.time = std::max(0.0f, marker.time + clampedDelta);

        m_SelectedCompositeTrack = trackIdx;
        m_SelectedCompositeClip = static_cast<size_t>(-1);
        if (m_TimeCompositeView)
        {
            m_TimeCompositeView->SetModel(m_CompositeModel.get());
            m_TimeCompositeView->ClearSelection();
            m_TimeCompositeView->MarkDirty(VisualDirty);
        }
        if (refreshSidebar)
        {
            RefreshSequencerSidebar();
            RefreshTimelineInspector();
        }
        return true;
    };

    if (m_PendingPanelUndoActive)
    {
        if (!applyMove())
            return false;

        if (m_ActiveView == ActiveView::TimeComposite && m_CurrentTimelineAsset)
            m_TimelineDirty = true;
        else
            m_Dirty = true;

        UpdateTitle();
        if (m_UnsavedIndicator)
            m_UnsavedIndicator->RemoveClass("hidden");
        return true;
    }

    return ExecutePanelEditWithUndo("Move Track in Time", applyMove, true);
}

void AnimationWindowPanel::OpenSequencerColorPicker(uint32_t initialArgb,
                                                    std::function<void(uint32_t)> onApply)
{
    if (!m_OpenColorPickerWindow)
        return;
    // Default an empty swatch to the normal row tint (#373737).
    const uint32_t shown = (initialArgb == 0u) ? kDefaultSequencerRowColorArgb : initialArgb;
    ColorPickerCallbacks cbs;
    cbs.onApply = [fn = std::move(onApply)](uint32_t argb, float)
    {
        if (fn)
            fn(argb);
    };
    m_OpenColorPickerWindow(shown, 1.0f, std::move(cbs));
}

void AnimationWindowPanel::RefreshSequencerSidebar()
{
    // Sequencer mutations (add/remove track or lane, drop a clip, etc.) flow through
    // here, so it's the cheapest spot to keep the per-mode clip dropdown in sync.
    if (m_ActiveView == ActiveView::TimeComposite || m_ActiveView == ActiveView::ClipEditor)
        RefreshClipDropdown();

    if (!m_SequencerSidebar)
        return;

    if (UIElement::IsInEventDispatch())
    {
        PostAction([this]() { RefreshSequencerSidebar(); });
        return;
    }

    ClearElementChildren(m_SequencerSidebar);
    ClearElementChildren(m_TimeCompositeView);
    ClearElementChildren(m_LaneClipEditorView);

    auto addTrackOfType = [this](CompositeTrackType type)
    {
        (void)ExecutePanelEditWithUndo("Add Track", [this, type]()
        {
            if (!m_CompositeModel) return false;
            CompositeTrack track = MakeDefaultCompositeTrack(type, m_CompositeModel->tracks.size(), m_TimelineState.currentTime);
            m_CompositeModel->tracks.push_back(std::move(track));
            if (m_TimeCompositeView)
            {
                m_TimeCompositeView->SetModel(m_CompositeModel.get());
                m_TimeCompositeView->MarkDirty(VisualDirty);
            }
            m_TimelineDirty = true;
            RefreshSequencerSidebar();
            return true;
        }, false);
    };
    auto addTransformTrackBundle = [this]()
    {
        (void)ExecutePanelEditWithUndo("Add Transform Track", [this]()
        {
            if (!m_CompositeModel) return false;
            const size_t baseIndex = m_CompositeModel->tracks.size();
            m_CompositeModel->tracks.push_back(
                MakeDefaultCompositeTrack(CompositeTrackType::Position3D, baseIndex, m_TimelineState.currentTime));
            m_CompositeModel->tracks.push_back(
                MakeDefaultCompositeTrack(CompositeTrackType::Rotation3D, baseIndex + 1, m_TimelineState.currentTime));
            m_CompositeModel->tracks.push_back(
                MakeDefaultCompositeTrack(CompositeTrackType::Scale3D, baseIndex + 2, m_TimelineState.currentTime));
            if (m_TimeCompositeView)
            {
                m_TimeCompositeView->SetModel(m_CompositeModel.get());
                m_TimeCompositeView->MarkDirty(VisualDirty);
            }
            m_TimelineDirty = true;
            RefreshSequencerSidebar();
            return true;
        }, false);
    };

    auto showAddTrackMenu = [this, addTrackOfType, addTransformTrackBundle](UIEvent& e)
    {
        UIElement& button = *e.CurrentTarget;
        if (!m_Window)
        {
            addTrackOfType(CompositeTrackType::Animation);
            return;
        }
        if (!m_AddTrackContextMenu)
        {
            m_AddTrackContextMenu = CreateContextMenu();
            if (m_AddTrackContextMenu)
            {
                m_AddTrackContextMenu->SetCommandHandler([addTrackOfType, addTransformTrackBundle](uint32_t cmd)
                {
                    if (cmd == kCmdAddTrackTransform)
                    {
                        addTransformTrackBundle();
                        return;
                    }
                    addTrackOfType(TrackTypeFromAddCommand(cmd));
                });
            }
        }
        if (!m_AddTrackContextMenu)
            return;
        m_AddTrackContextMenu->Clear();
        ContextMenuBuilder builder;
        builder.AddItem("Transform Track...", kCmdAddTrackTransform, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Property Track...", kCmdAddTrackProperty, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Blend Shape Track...", kCmdAddTrackBlendShape, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Call Method Track...", kCmdAddTrackMethod, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Event Track...", kCmdAddTrackEvent, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Bezier Curve Track...", kCmdAddTrackBezier, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Audio Playback Track...", kCmdAddTrackAudio, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Animation Playback Track...", kCmdAddTrackAnimation, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.AddItem("Video Playback Track...", kCmdAddTrackVideo, MenuItemFlag_None, 0, EditorIcons::kFilm);
        builder.Build(m_AddTrackContextMenu.get());
        const int x = static_cast<int>(button.GetLayoutX());
        const int y = static_cast<int>(button.GetLayoutY() + button.GetLayoutHeight());
        ShowContextMenuKeepingFocus(m_AddTrackContextMenu.get(), x, y);
    };

    auto addLaneAction = [this](UIEvent&)
    {
        (void)ExecutePanelEditWithUndo("Add Lane", [this]()
        {
            if (!m_LaneClipModel) return false;
            LaneClipLane lane;
            lane.name = "Lane " + std::to_string(m_LaneClipModel->lanes.size() + 1);
            m_LaneClipModel->lanes.push_back(std::move(lane));
            if (m_LaneClipEditorView)
            {
                m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
                m_LaneClipEditorView->MarkDirty(VisualDirty);
            }
            m_ClipSetDirty = true;
            RefreshSequencerSidebar();
            return true;
        }, false);
    };

    if (m_ActiveView == ActiveView::TimeComposite)
    {
        if (!m_CompositeModel || m_CompositeModel->tracks.empty())
        {
            if (m_CurrentTimelineAsset)
                AddSidebarButton(m_SequencerSidebar, "+ Add Track", false, showAddTrackMenu);
            else
                AddSidebarLabel(m_TimeCompositeView, "Create a new timeline or open an existing one to get started.", "animationwindow-sequencer-empty");
            return;
        }

        auto spacer = std::make_unique<UIElement>();
        spacer->Overrides()
            .Set(Style::Height, StyleLength::Px(2.0f))
            .Set(Style::MinHeight, StyleLength::Px(2.0f))
            .Set(Style::FlexShrink, 0.0f);
        m_SequencerSidebar->AddChild(std::move(spacer));

        auto list = std::make_unique<UIElement>();
        UIElement* listPtr = list.get();
        listPtr->AddClass("animationwindow-sequencer-track-list");
        m_SequencerSidebar->AddChild(std::move(list));

        if (!m_SequencerTrackContextMenu)
        {
            m_SequencerTrackContextMenu = CreateContextMenu();
            if (m_SequencerTrackContextMenu)
            {
                m_SequencerTrackContextMenu->SetCommandHandler([this](uint32_t cmd)
                {
                    if (cmd == kCmdTimelineInsertKey)
                    {
                        (void)InsertTimelineTrackKeyAt(m_ContextMenuSequencerIdx, m_TimelineState.currentTime);
                    }
                    else if (cmd == kCmdTimelineInsertMarker)
                    {
                        (void)InsertTimelineTrackMarkerAt(m_ContextMenuSequencerIdx, m_TimelineState.currentTime);
                    }
                    else if (cmd == kCmdSequencerDeleteTrack)
                    {
                        const size_t idx = m_ContextMenuSequencerIdx;
                        (void)ExecutePanelEditWithUndo("Delete Track", [this, idx]()
                        {
                            if (!m_CompositeModel || idx >= m_CompositeModel->tracks.size()) return false;
                            m_CompositeModel->tracks.erase(m_CompositeModel->tracks.begin() + static_cast<std::ptrdiff_t>(idx));
                            if (m_TimeCompositeView)
                            {
                                m_TimeCompositeView->SetModel(m_CompositeModel.get());
                                m_TimeCompositeView->ClearSelection();
                                m_TimeCompositeView->MarkDirty(VisualDirty);
                            }
                            if (m_SelectedCompositeTrack == idx)
                            {
                                m_SelectedCompositeTrack = static_cast<size_t>(-1);
                                m_SelectedCompositeClip  = static_cast<size_t>(-1);
                            }
                            m_TimelineDirty = true;
                            RefreshSequencerSidebar();
                            return true;
                        }, false);
                    }
                });
            }
        }
        if (m_SequencerTrackContextMenu)
        {
            m_SequencerTrackContextMenu->Clear();
            ContextMenuBuilder builder;
            builder.AddItem("Insert Key at Playhead", kCmdTimelineInsertKey, MenuItemFlag_None, 0, EditorIcons::kPlus);
            builder.AddItem("Insert Marker at Playhead", kCmdTimelineInsertMarker, MenuItemFlag_None, 0, EditorIcons::kPlus);
            builder.AddItem("Delete Track", kCmdSequencerDeleteTrack, MenuItemFlag_None, 0, EditorIcons::kTrash);
            builder.Build(m_SequencerTrackContextMenu.get());
        }

        auto selectTrackRow = [this](size_t trackIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return;

            const auto& clips = m_CompositeModel->tracks[trackIdx].clips;
            if (clips.empty())
            {
                m_SelectedCompositeTrack = trackIdx;
                m_SelectedCompositeClip = static_cast<size_t>(-1);
                m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                m_SelectedTimelineKey = static_cast<size_t>(-1);
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->ClearSelection();
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
                RefreshClipDropdown();
                return;
            }

            SelectCompositeClip(trackIdx, 0u);
            if (m_TimeCompositeView)
            {
                m_TimeCompositeView->SetSelectedClip(trackIdx, 0u);
                m_TimeCompositeView->MarkDirty(VisualDirty);
            }
            RefreshTimelineInspector();
        };

        for (size_t trackIdx = 0; trackIdx < m_CompositeModel->tracks.size(); ++trackIdx)
        {
            const CompositeTrack& track = m_CompositeModel->tracks[trackIdx];
            auto row = std::make_unique<UIElement>();
            UIElement* rowPtr = row.get();
            rowPtr->AddClass("animationwindow-sequencer-track-row");
            listPtr->AddChild(std::move(row));

            rowPtr->RegisterEventHandler(kEventMouseDown, [this, trackIdx, selectTrackRow](UIEvent& e)
            {
                if (e.Button == 1)
                {
                    if (!m_Window || !m_SequencerTrackContextMenu)
                        return;
                    m_ContextMenuSequencerIdx = trackIdx;
                    if (m_CompositeModel && trackIdx < m_CompositeModel->tracks.size())
                    {
                        const auto& track = m_CompositeModel->tracks[trackIdx];
                        const bool canInsertKey = CompositeTrackTypeUsesValueKeys(track.type) ||
                                                  CompositeTrackTypeUsesMethodKeys(track.type) ||
                                                  CompositeTrackTypeUsesAudioKeys(track.type) ||
                                                  CompositeTrackTypeUsesAnimationKeys(track.type);
                        m_SequencerTrackContextMenu->Clear();
                        ContextMenuBuilder builder;
                        builder.AddItem("Insert Key at Playhead", kCmdTimelineInsertKey,
                                        canInsertKey ? MenuItemFlag_None : MenuItemFlag_Disabled,
                                        0, EditorIcons::kPlus);
                        builder.AddItem("Insert Marker at Playhead", kCmdTimelineInsertMarker, MenuItemFlag_None, 0, EditorIcons::kPlus);
                        builder.AddItem("Delete Track", kCmdSequencerDeleteTrack, MenuItemFlag_None, 0, EditorIcons::kTrash);
                        builder.Build(m_SequencerTrackContextMenu.get());
                    }
                    ShowContextMenuKeepingFocus(m_SequencerTrackContextMenu.get(),
                                                static_cast<int>(e.X), static_cast<int>(e.Y));
                    e.Stop();
                    return;
                }

                if (e.Button != 0)
                    return;
                selectTrackRow(trackIdx);
                e.Stop();
            });

            auto content = std::make_unique<UIElement>();
            UIElement* contentPtr = content.get();
            contentPtr->AddClass("animationwindow-sequencer-track-content");
            rowPtr->AddChild(std::move(content));

            const bool active = m_SelectedCompositeTrack == trackIdx;
            AddSidebarEnableButton(contentPtr,
                                   track.enabled,
                                   [this, trackIdx](const bool enabled)
                                   {
                                       (void)ExecutePanelEditWithUndo("Toggle Track Enabled",
                                                                     [this, trackIdx, enabled]()
                                                                     {
                                                                         if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                                                                             return false;
                                                                         m_CompositeModel->tracks[trackIdx].enabled = enabled;
                                                                         if (m_TimeCompositeView)
                                                                             m_TimeCompositeView->MarkDirty(VisualDirty);
                                                                         m_TimelineDirty = true;
                                                                         return true;
                                                                     },
                                                                     false);
                                       RefreshSequencerSidebar();
                                       RefreshTimelineInspector();
                                   });
            const std::string fullTrackName = track.name.empty() ? ("Track " + std::to_string(trackIdx + 1u)) : track.name;
            Button* trackNameButton = AddSidebarButton(contentPtr,
                                                       SidebarTrackDisplayName(fullTrackName),
                                                       active,
                                                       [selectTrackRow, trackIdx](UIEvent&)
                                                       {
                                                           selectTrackRow(trackIdx);
                                                       });
            if (trackNameButton)
            {
                const std::string displayName = SidebarTrackDisplayName(fullTrackName);
                std::string tooltip = fullTrackName;
                const std::string typeName = CompositeTrackTypeDisplayName(track.type);
                if (tooltip != typeName)
                    tooltip += " - " + typeName;
                if (displayName != fullTrackName || tooltip != fullTrackName)
                    trackNameButton->SetTooltip(tooltip);
            }
            auto trackSpacer = std::make_unique<UIElement>();
            trackSpacer->AddClass("animationwindow-sequencer-track-spacer");
            contentPtr->AddChild(std::move(trackSpacer));
            if (CompositeTrackTypeUsesClips(track.type))
            {
                AddSidebarLabel(contentPtr,
                                std::to_string(track.clips.size()) + (track.clips.size() == 1u ? " clip" : " clips"),
                                "animationwindow-sequencer-meta");
            }
            else if (CompositeTrackTypeUsesValueKeys(track.type))
            {
                AddSidebarLabel(contentPtr,
                                std::to_string(track.valueKeys.size()) + (track.valueKeys.size() == 1u ? " key" : " keys"),
                                "animationwindow-sequencer-meta");
            }
            else if (CompositeTrackTypeUsesMethodKeys(track.type))
            {
                AddSidebarLabel(contentPtr,
                                std::to_string(track.methodKeys.size()) + (track.methodKeys.size() == 1u ? " key" : " keys"),
                                "animationwindow-sequencer-meta");
            }
            if (CompositeTrackTypeUsesAudioKeys(track.type))
            {
                AddSidebarLabel(contentPtr,
                                std::to_string(track.audioKeys.size()) + (track.audioKeys.size() == 1u ? " audio key" : " audio keys"),
                                "animationwindow-sequencer-meta");
            }
            if (CompositeTrackTypeUsesAnimationKeys(track.type) && !track.animationKeys.empty())
            {
                AddSidebarLabel(contentPtr,
                                std::to_string(track.animationKeys.size()) + (track.animationKeys.size() == 1u ? " animation key" : " animation keys"),
                                "animationwindow-sequencer-meta");
            }
            AddSidebarColorSwatch(contentPtr, track.color,
                                  [this, trackIdx]()
                                  {
                                      if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                                          return;
                                      const uint32_t initial = m_CompositeModel->tracks[trackIdx].color;
                                      OpenSequencerColorPicker(initial, [this, trackIdx](uint32_t argb)
                                      {
                                          (void)ExecutePanelEditWithUndo("Set Track Color",
                                              [this, trackIdx, argb]()
                                              {
                                                  if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                                                      return false;
                                                  m_CompositeModel->tracks[trackIdx].color = argb;
                                                  if (m_TimeCompositeView)
                                                      m_TimeCompositeView->MarkDirty(VisualDirty);
                                                  m_TimelineDirty = true;
                                                  return true;
                                              }, false);
                                          RefreshSequencerSidebar();
                                      });
                                  });
        }
        if (m_CurrentTimelineAsset)
            AddSidebarButton(m_SequencerSidebar, "+ Add Track", false, showAddTrackMenu);
        return;
    }

    if (m_ActiveView != ActiveView::ClipEditor)
        return;

    if (!m_LaneClipModel || m_LaneClipModel->lanes.empty())
    {
        if (m_CurrentClipSetAsset)
            AddSidebarButton(m_SequencerSidebar, "+ Add Lane", false, addLaneAction);
        else
            AddSidebarLabel(m_LaneClipEditorView, "Create a new clip set or open an existing one to get started.", "animationwindow-sequencer-empty");
        return;
    }

    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("animationwindow-sequencer-top-spacer");
    m_SequencerSidebar->AddChild(std::move(spacer));

    auto list = std::make_unique<UIElement>();
    UIElement* listPtr = list.get();
    listPtr->AddClass("animationwindow-sequencer-track-list");
    m_SequencerSidebar->AddChild(std::move(list));

    if (!m_SequencerLaneContextMenu)
    {
        m_SequencerLaneContextMenu = CreateContextMenu();
        if (m_SequencerLaneContextMenu)
        {
            m_SequencerLaneContextMenu->SetCommandHandler([this](uint32_t cmd)
            {
                if (cmd == kCmdSequencerDeleteLane)
                {
                    const size_t idx = m_ContextMenuSequencerIdx;
                    (void)ExecutePanelEditWithUndo("Delete Lane", [this, idx]()
                    {
                        if (!m_LaneClipModel || idx >= m_LaneClipModel->lanes.size()) return false;
                        m_LaneClipModel->lanes.erase(m_LaneClipModel->lanes.begin() + static_cast<std::ptrdiff_t>(idx));
                        if (m_LaneClipEditorView)
                        {
                            m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
                            m_LaneClipEditorView->ClearSelection();
                            m_LaneClipEditorView->MarkDirty(VisualDirty);
                        }
                        if (m_SelectedLane == idx)
                        {
                            m_SelectedLane     = static_cast<size_t>(-1);
                            m_SelectedLaneClip = static_cast<size_t>(-1);
                        }
                        m_ClipSetDirty = true;
                        RefreshSequencerSidebar();
                        return true;
                    }, false);
                }
            });
        }
    }
    if (m_SequencerLaneContextMenu)
    {
        m_SequencerLaneContextMenu->Clear();
        ContextMenuBuilder builder;
        builder.AddItem("Delete Lane", kCmdSequencerDeleteLane, MenuItemFlag_None, 0, EditorIcons::kTrash);
        builder.Build(m_SequencerLaneContextMenu.get());
    }

    for (size_t laneIdx = 0; laneIdx < m_LaneClipModel->lanes.size(); ++laneIdx)
    {
        const LaneClipLane& lane = m_LaneClipModel->lanes[laneIdx];
        auto row = std::make_unique<UIElement>();
        UIElement* rowPtr = row.get();
        rowPtr->AddClass("animationwindow-sequencer-track-row");
        listPtr->AddChild(std::move(row));

        rowPtr->RegisterEventHandler(kEventMouseDown, [this, laneIdx](UIEvent& e)
        {
            if (e.Button != 1 || !m_Window || !m_SequencerLaneContextMenu) return;
            m_ContextMenuSequencerIdx = laneIdx;
            ShowContextMenuKeepingFocus(m_SequencerLaneContextMenu.get(),
                                        static_cast<int>(e.X), static_cast<int>(e.Y));
            e.Stop();
        });

        auto content = std::make_unique<UIElement>();
        UIElement* contentPtr = content.get();
        contentPtr->AddClass("animationwindow-sequencer-track-content");
        rowPtr->AddChild(std::move(content));

        const bool active = m_SelectedLane == laneIdx;
        const size_t targetClipIdx =
            (active && m_SelectedLaneClip < lane.clips.size()) ? m_SelectedLaneClip : 0u;
        const LaneClipInstance* targetClip = targetClipIdx < lane.clips.size() ? &lane.clips[targetClipIdx] : nullptr;
        if (targetClip)
        {
            AddSidebarEnableButton(contentPtr,
                                   !targetClip->muted,
                                   [this, laneIdx, clipIdx = targetClipIdx](const bool enabled)
                                   {
                                       (void)ExecutePanelEditWithUndo("Toggle Lane Clip",
                                                                     [this, laneIdx, clipIdx, enabled]()
                                                                     {
                                                                         if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size())
                                                                             return false;
                                                                         auto& lane = m_LaneClipModel->lanes[laneIdx];
                                                                         if (clipIdx >= lane.clips.size())
                                                                             return false;
                                                                         lane.clips[clipIdx].muted = !enabled;
                                                                         if (m_LaneClipEditorView)
                                                                             m_LaneClipEditorView->MarkDirty(VisualDirty);
                                                                         return true;
                                                                     },
                                                                     false);
                                       RefreshSequencerSidebar();
                                   });
        }
        AddSidebarButton(contentPtr,
                         lane.name.empty() ? ("Lane " + std::to_string(laneIdx + 1u)) : lane.name,
                         active,
                         [this, laneIdx, targetClipIdx](UIEvent&)
                         {
                             if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size() ||
                                 targetClipIdx >= m_LaneClipModel->lanes[laneIdx].clips.size())
                             {
                                 return;
                             }
                             SelectLaneClip(laneIdx, targetClipIdx);
                             if (m_LaneClipEditorView)
                             {
                                 m_LaneClipEditorView->SetSelectedClip(laneIdx, targetClipIdx);
                                 m_LaneClipEditorView->MarkDirty(VisualDirty);
                             }
                         });
        auto trackSpacer = std::make_unique<UIElement>();
        trackSpacer->AddClass("animationwindow-sequencer-track-spacer");
        contentPtr->AddChild(std::move(trackSpacer));
        AddSidebarLabel(contentPtr,
                        std::to_string(lane.clips.size()) + (lane.clips.size() == 1u ? " clip" : " clips"),
                        "animationwindow-sequencer-meta");
        if (targetClip)
        {
            AddSidebarLabel(contentPtr,
                            std::string("×") + std::to_string(targetClip->loopCount),
                            "animationwindow-sequencer-value");
        }
        AddSidebarColorSwatch(contentPtr, lane.color,
                              [this, laneIdx]()
                              {
                                  if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size())
                                      return;
                                  const uint32_t initial = m_LaneClipModel->lanes[laneIdx].color;
                                  OpenSequencerColorPicker(initial, [this, laneIdx](uint32_t argb)
                                  {
                                      (void)ExecutePanelEditWithUndo("Set Lane Color",
                                          [this, laneIdx, argb]()
                                          {
                                              if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size())
                                                  return false;
                                              m_LaneClipModel->lanes[laneIdx].color = argb;
                                              if (m_LaneClipEditorView)
                                                  m_LaneClipEditorView->MarkDirty(VisualDirty);
                                              m_ClipSetDirty = true;
                                              return true;
                                          }, false);
                                      RefreshSequencerSidebar();
                                  });
                              });
    }

    if (m_CurrentClipSetAsset)
        AddSidebarButton(m_SequencerSidebar, "+ Add Lane", false, addLaneAction);
}

void AnimationWindowPanel::RefreshTimelineInspector()
{
    if (!m_OnTimelineInspectorRequested || m_ActiveView != ActiveView::TimeComposite ||
        !m_CompositeModel || m_CompositeModel->tracks.empty())
        return;

    const size_t trackIdx = (m_SelectedTimelineKeyTrack < m_CompositeModel->tracks.size())
        ? m_SelectedTimelineKeyTrack
        : m_SelectedCompositeTrack;
    if (trackIdx >= m_CompositeModel->tracks.size())
        return;

    const CompositeTrack& track = m_CompositeModel->tracks[trackIdx];
    std::string title = "Timeline Track";
    if (m_SelectedTimelineKeyTrack == trackIdx && m_SelectedTimelineKey != static_cast<size_t>(-1))
        title = "Timeline Key";
    else if (m_SelectedCompositeClip < track.clips.size())
        title = "Timeline Clip";

    m_OnTimelineInspectorRequested(title, [this](UIElement* parent)
    {
        BuildTimelineInspector(parent);
    });
}

std::pair<std::function<void(float)>, std::function<void(float)>>
AnimationWindowPanel::MakeTimelineFloatEditHandlers(const char* undoLabel,
                                                    std::function<void(float)> applyToModel)
{
    const std::function<void(float)> apply = std::move(applyToModel);

    auto preview = [this, undoLabel, apply](float v)
    {
        if (!apply)
            return;
        if (!m_PendingPanelUndoActive)
            BeginPanelUndoGesture(undoLabel);
        apply(v);
        if (m_TimeCompositeView)
            m_TimeCompositeView->MarkDirty(VisualDirty);
    };

    auto commit = [this, undoLabel, apply](float v)
    {
        if (!apply)
            return;

        if (m_PendingPanelUndoActive)
        {
            apply(v);
            CommitPanelUndoGesture();
        }
        else
        {
            (void)ExecutePanelEditWithUndo(
                undoLabel,
                [this, apply, v]()
                {
                    apply(v);
                    if (m_TimeCompositeView)
                        m_TimeCompositeView->MarkDirty(VisualDirty);
                    RefreshSequencerSidebar();
                    return true;
                },
                false);
        }
        RefreshTimelineInspector();
    };

    return {std::move(preview), std::move(commit)};
}

void AnimationWindowPanel::BuildTimelineInspector(UIElement* parent)
{
    if (!parent || m_ActiveView != ActiveView::TimeComposite || !m_CompositeModel)
        return;

    const size_t trackIdx = (m_SelectedTimelineKeyTrack < m_CompositeModel->tracks.size())
        ? m_SelectedTimelineKeyTrack
        : m_SelectedCompositeTrack;
    if (trackIdx >= m_CompositeModel->tracks.size())
        return;

    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    CompositeTrack& track = m_CompositeModel->tracks[trackIdx];
    UIElement* trackSection = AddTimelineInspectorSection(parent, "Track");

    auto commitTrack = [this, trackIdx](const char* label, auto setter)
    {
        (void)ExecutePanelEditWithUndo(label, [this, trackIdx, setter]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            setter(m_CompositeModel->tracks[trackIdx]);
            if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            return true;
        }, false);
        RefreshTimelineInspector();
    };

    AddTimelineInspectorText(trackSection, "Name", track.name,
        [commitTrack](const std::string& v) { commitTrack("Edit Track Name", [v](CompositeTrack& t) { t.name = v; }); },
        "Display name for this timeline track.");
    AddTimelineInspectorText(trackSection, "Target", track.targetPath,
        [commitTrack](const std::string& v) { commitTrack("Edit Track Target", [v](CompositeTrack& t) { t.targetPath = v; }); },
        "Entity or node path this track animates.");
    AddTimelineInspectorText(trackSection, "Property", track.propertyPath,
        [commitTrack](const std::string& v) { commitTrack("Edit Track Property", [v](CompositeTrack& t) { t.propertyPath = v; }); },
        "Component property path driven by this track.");
    AddTimelineInspectorCheckbox(trackSection, "Enabled", track.enabled,
        [commitTrack](const bool& v) { commitTrack("Toggle Track Enabled", [v](CompositeTrack& t) { t.enabled = v; }); },
        "Disable to keep this track from contributing during playback.");
    AddTimelineInspectorCheckbox(trackSection, "Imported", track.imported,
        [commitTrack](const bool& v) { commitTrack("Toggle Track Imported", [v](CompositeTrack& t) { t.imported = v; }); },
        "Marks this track as sourced from imported animation data.");
    AddTimelineInspectorDropdown(trackSection, "Update",
        {{"continuous", "Continuous"}, {"discrete", "Discrete"}, {"capture", "Capture"}},
        CompositeTrackUpdateModeToString(track.updateMode),
        [commitTrack](const std::string& v)
        {
            commitTrack("Set Track Update Mode", [v](CompositeTrack& t) { t.updateMode = CompositeTrackUpdateModeFromString(v); });
        },
        "How values are sampled between keys.");
    AddTimelineInspectorDropdown(trackSection, "Interpolation",
        {{"nearest", "Nearest"}, {"linear", "Linear"}, {"cubic", "Cubic"}},
        CompositeTrackInterpolationToString(track.interpolation),
        [commitTrack](const std::string& v)
        {
            commitTrack("Set Track Interpolation", [v](CompositeTrack& t) { t.interpolation = CompositeTrackInterpolationFromString(v); });
        },
        "Curve interpolation used by value keys.");
    AddTimelineInspectorCheckbox(trackSection, "Loop Wrap", track.loopWrap,
        [commitTrack](const bool& v) { commitTrack("Toggle Track Loop Wrap", [v](CompositeTrack& t) { t.loopWrap = v; }); },
        "Wrap sampled time around the playback range for looping tracks.");
    if (track.type == CompositeTrackType::Audio)
    {
        AddTimelineInspectorDropdown(trackSection, "Sync Mode",
            {{"lock", "Lock"}, {"keepOffset", "Keep Offset"}},
            CompositeMediaSyncModeToString(track.mediaSyncMode),
            [commitTrack](const std::string& v)
            {
                commitTrack("Set Audio/Video Sync Mode", [v](CompositeTrack& t) { t.mediaSyncMode = CompositeMediaSyncModeFromString(v); });
            },
            "Lock keeps linked clips exactly aligned. Keep Offset preserves manual offset while syncing.");
        AddTimelineInspectorCheckbox(trackSection, "Use Blend", track.useBlend,
            [commitTrack](const bool& v) { commitTrack("Toggle Audio Track Blend", [v](CompositeTrack& t) { t.useBlend = v; }); },
            "Blend audio keys instead of hard switching.");
        AddTimelineInspectorCheckbox(trackSection, "Sync Linked Video", track.syncLinkedVideo,
            [this, trackIdx, commitTrack](const bool& v)
            {
                if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                    return;
                if (v)
                {
                    const CompositeTrack& audioTrack = m_CompositeModel->tracks[trackIdx];
                    if (audioTrack.mediaSyncMode == CompositeMediaSyncMode::KeepOffset)
                    {
                        (void)ExecutePanelEditWithUndo("Enable Audio/Video Sync", [this, trackIdx]()
                        {
                            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                                return false;
                            auto& tracks = m_CompositeModel->tracks;
                            auto& audio = tracks[trackIdx];
                            audio.syncLinkedVideo = true;
                            for (auto& audioClip : audio.clips)
                            {
                                if (audioClip.linkedMediaGroupGuid.IsNull())
                                    continue;
                                bool foundVideo = false;
                                for (auto& candidateTrack : tracks)
                                {
                                    if (candidateTrack.type != CompositeTrackType::Video)
                                        continue;
                                    for (const auto& videoClip : candidateTrack.clips)
                                    {
                                        if (videoClip.linkedMediaGroupGuid == audioClip.linkedMediaGroupGuid)
                                        {
                                            audioClip.linkedMediaOffsetSeconds = audioClip.offsetOnTimeline - videoClip.offsetOnTimeline;
                                            foundVideo = true;
                                            break;
                                        }
                                    }
                                    if (foundVideo)
                                        break;
                                }
                                if (!foundVideo)
                                    audioClip.linkedMediaOffsetSeconds = 0.0f;
                            }
                            if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                            RefreshSequencerSidebar();
                            RefreshTimelineInspector();
                            return true;
                        }, false);
                        return;
                    }
                }
                commitTrack("Toggle Audio/Video Sync", [v](CompositeTrack& t) { t.syncLinkedVideo = v; });
            },
            "When enabled, linked audio and video clips move/trim together.");
    }

    const bool hasSelectedKey = m_SelectedTimelineKeyTrack == trackIdx &&
                                m_SelectedTimelineKey != static_cast<size_t>(-1);
    if (!hasSelectedKey)
    {
        if (m_SelectedCompositeClip < track.clips.size())
        {
            const size_t clipIdx = m_SelectedCompositeClip;
            CompositeClip& clip = track.clips[clipIdx];
            UIElement* clipSection = AddTimelineInspectorSection(parent, "Clip");
            auto commitClip = [this, trackIdx, clipIdx](const char* label, auto setter)
            {
                (void)ExecutePanelEditWithUndo(label, [this, trackIdx, clipIdx, setter]()
                {
                    if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                        return false;
                    auto& clips = m_CompositeModel->tracks[trackIdx].clips;
                    if (clipIdx >= clips.size())
                        return false;
                    setter(clips[clipIdx]);
                    if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                    RefreshSequencerSidebar();
                    return true;
                }, false);
                RefreshTimelineInspector();
            };
            auto addClipFloat = [this, trackIdx, clipIdx](UIElement* section,
                                                          const char* label,
                                                          float initial,
                                                          const char* undoLabel,
                                                          std::function<void(CompositeClip&, float)> mutate,
                                                          float resetValue = 0.0f,
                                                          const char* fieldTooltip = nullptr,
                                                          float minValue = 0.0f)
            {
                auto handlers = MakeTimelineFloatEditHandlers(
                    undoLabel,
                    [this, trackIdx, clipIdx, mutate](float v)
                    {
                        if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                            return;
                        auto& clips = m_CompositeModel->tracks[trackIdx].clips;
                        if (clipIdx >= clips.size())
                            return;
                        mutate(clips[clipIdx], v);
                    });
                AddTimelineInspectorFloat(section,
                                          label,
                                          initial,
                                          handlers.first,
                                          handlers.second,
                                          resetValue,
                                          fieldTooltip,
                                          minValue);
            };

            AddTimelineInspectorText(clipSection, "Name", clip.name,
                [commitClip](const std::string& v) { commitClip("Edit Clip Name", [v](CompositeClip& c) { c.name = v; }); },
                "Display name for this timeline clip.");
            addClipFloat(clipSection,
                         "Offset",
                         clip.offsetOnTimeline,
                         "Edit Clip Offset",
                         [](CompositeClip& c, float v) { c.offsetOnTimeline = std::max(0.0f, v); },
                         0.0f,
                         "Clip start time on the destination timeline.");
            addClipFloat(clipSection,
                         "In",
                         clip.inTime,
                         "Edit Clip In",
                         [](CompositeClip& c, float v) { c.inTime = std::max(0.0f, std::min(v, c.outTime)); },
                         0.0f,
                         "Source time where this clip begins.");
            addClipFloat(clipSection,
                         "Out",
                         clip.outTime,
                         "Edit Clip Out",
                         [](CompositeClip& c, float v) { c.outTime = std::max(c.inTime, v); },
                         0.0f,
                         "Source time where this clip ends.");
            AddTimelineInspectorCheckbox(clipSection, "Muted", clip.muted,
                [commitClip](const bool& v) { commitClip("Toggle Clip Muted", [v](CompositeClip& c) { c.muted = v; }); },
                "Mute this clip without deleting it.");
            addClipFloat(clipSection,
                         "Fade In",
                         clip.fadeInDuration,
                         "Edit Clip Fade In",
                         [](CompositeClip& c, float v) { c.fadeInDuration = std::max(0.0f, v); },
                         0.0f,
                         "Seconds used to fade this clip in.");
            addClipFloat(clipSection,
                         "Fade Out",
                         clip.fadeOutDuration,
                         "Edit Clip Fade Out",
                         [](CompositeClip& c, float v) { c.fadeOutDuration = std::max(0.0f, v); },
                         0.0f,
                         "Seconds used to fade this clip out.");
        }
        return;
    }

    UIElement* keySection = AddTimelineInspectorSection(parent, "Key");
    auto commitKey = [this, trackIdx](const char* label, auto setter)
    {
        (void)ExecutePanelEditWithUndo(label, [this, trackIdx, setter]()
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            setter(m_CompositeModel->tracks[trackIdx]);
            if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
            RefreshSequencerSidebar();
            return true;
        }, false);
        RefreshTimelineInspector();
    };

    auto addKeyFloat = [this, trackIdx](UIElement* section,
                                        const char* label,
                                        float initial,
                                        const char* undoLabel,
                                        std::function<void(CompositeTrack&, float)> mutate,
                                        float resetValue = 0.0f,
                                        const char* fieldTooltip = nullptr,
                                        float minValue = 0.0f)
    {
        auto handlers = MakeTimelineFloatEditHandlers(
            undoLabel,
            [this, trackIdx, mutate](float v)
            {
                if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                    return;
                mutate(m_CompositeModel->tracks[trackIdx], v);
            });
        AddTimelineInspectorFloat(section,
                                label,
                                initial,
                                handlers.first,
                                handlers.second,
                                resetValue,
                                fieldTooltip,
                                minValue);
    };

    if (CompositeTrackTypeUsesValueKeys(track.type) && m_SelectedTimelineKey < track.valueKeys.size())
    {
        CompositeValueKey& key = track.valueKeys[m_SelectedTimelineKey];
        const size_t keyIdx = m_SelectedTimelineKey;
        addKeyFloat(keySection,
                    "Time",
                    key.time,
                    "Edit Value Key Time",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.valueKeys.size())
                            t.valueKeys[keyIdx].time = std::max(0.0f, v);
                    },
                    0.0f,
                    "Key time in seconds.");
        AddTimelineInspectorText(keySection, "Label", key.label,
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Value Key Label", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.valueKeys.size()) t.valueKeys[keyIdx].label = v; }); },
            "Optional label shown for this key.");
        static const char* kComponentLabels[4] = {"Value X", "Value Y", "Value Z", "Value W"};
        for (uint8_t i = 0; i < key.componentCount && i < 4u; ++i)
        {
            const float componentValue = key.value[i];
            addKeyFloat(keySection,
                        kComponentLabels[i],
                        componentValue,
                        "Edit Value Key",
                        [keyIdx, i](CompositeTrack& t, float v)
                        {
                            if (keyIdx < t.valueKeys.size())
                                t.valueKeys[keyIdx].value[i] = v;
                        },
                        componentValue,
                        "Animated value component at this key.");
        }
        if (track.type == CompositeTrackType::Bezier)
        {
            AddTimelineInspectorDropdown(keySection, "Handle Mode",
                {{"free", "Free"}, {"linear", "Linear"}, {"balanced", "Balanced"}, {"mirrored", "Mirrored"}},
                CompositeBezierHandleModeToString(key.handleMode),
                [commitKey, keyIdx](const std::string& v) { commitKey("Edit Bezier Handle Mode", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.valueKeys.size()) t.valueKeys[keyIdx].handleMode = CompositeBezierHandleModeFromString(v); }); },
                "How Bezier in/out handles are constrained.");
            addKeyFloat(keySection,
                        "In X",
                        key.inHandle[0],
                        "Edit Bezier In Handle",
                        [keyIdx](CompositeTrack& t, float v)
                        {
                            if (keyIdx < t.valueKeys.size())
                                t.valueKeys[keyIdx].inHandle[0] = v;
                        },
                        0.0f,
                        "Incoming Bezier handle time offset.");
            addKeyFloat(keySection,
                        "In Y",
                        key.inHandle[1],
                        "Edit Bezier In Handle",
                        [keyIdx](CompositeTrack& t, float v)
                        {
                            if (keyIdx < t.valueKeys.size())
                                t.valueKeys[keyIdx].inHandle[1] = v;
                        },
                        0.0f,
                        "Incoming Bezier handle value offset.");
            addKeyFloat(keySection,
                        "Out X",
                        key.outHandle[0],
                        "Edit Bezier Out Handle",
                        [keyIdx](CompositeTrack& t, float v)
                        {
                            if (keyIdx < t.valueKeys.size())
                                t.valueKeys[keyIdx].outHandle[0] = v;
                        },
                        0.0f,
                        "Outgoing Bezier handle time offset.");
            addKeyFloat(keySection,
                        "Out Y",
                        key.outHandle[1],
                        "Edit Bezier Out Handle",
                        [keyIdx](CompositeTrack& t, float v)
                        {
                            if (keyIdx < t.valueKeys.size())
                                t.valueKeys[keyIdx].outHandle[1] = v;
                        },
                        0.0f,
                        "Outgoing Bezier handle value offset.");
        }
        return;
    }

    if (CompositeTrackTypeUsesMethodKeys(track.type) && m_SelectedTimelineKey < track.methodKeys.size())
    {
        CompositeMethodKey& key = track.methodKeys[m_SelectedTimelineKey];
        const size_t keyIdx = m_SelectedTimelineKey;
        addKeyFloat(keySection,
                    "Time",
                    key.time,
                    "Edit Method Key Time",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.methodKeys.size())
                            t.methodKeys[keyIdx].time = std::max(0.0f, v);
                    },
                    0.0f,
                    "Key time in seconds.");
        AddTimelineInspectorText(keySection, track.type == CompositeTrackType::Event ? "Event" : "Method", key.methodName,
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Method Name", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.methodKeys.size()) t.methodKeys[keyIdx].methodName = v; }); },
            "Event or method name fired by this key.");
        AddTimelineInspectorText(keySection, "Arguments", key.arguments,
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Method Arguments", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.methodKeys.size()) t.methodKeys[keyIdx].arguments = v; }); },
            "Raw comma-separated argument string passed to the callback.");
        AddTimelineInspectorText(keySection, "Typed Args", FormatTypedArguments(key.typedArguments),
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Typed Arguments", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.methodKeys.size()) t.methodKeys[keyIdx].typedArguments = ParseTypedArguments(v); }); },
            "Typed arguments in type:value form, separated by commas.");
        return;
    }

    if (CompositeTrackTypeUsesAudioKeys(track.type) && m_SelectedTimelineKey < track.audioKeys.size())
    {
        CompositeAudioKey& key = track.audioKeys[m_SelectedTimelineKey];
        const size_t keyIdx = m_SelectedTimelineKey;
        AddTimelineInspectorAsset(keySection, "Stream", key.audioGuid, {AssetType::Audio}, &registry,
            [commitKey, keyIdx](const GUID& v) { commitKey("Edit Audio Stream", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.audioKeys.size()) t.audioKeys[keyIdx].audioGuid = v; }); },
            "Audio asset played by this key.");
        AddTimelineInspectorText(keySection, "Name", key.name,
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Audio Key Name", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.audioKeys.size()) t.audioKeys[keyIdx].name = v; }); },
            "Display name for this audio key.");
        addKeyFloat(keySection,
                    "Time",
                    key.time,
                    "Edit Audio Key Time",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.audioKeys.size())
                            t.audioKeys[keyIdx].time = std::max(0.0f, v);
                    },
                    0.0f,
                    "Key time in seconds.");
        addKeyFloat(keySection,
                    "Start Offset",
                    key.startOffset,
                    "Edit Audio Start Offset",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.audioKeys.size())
                            t.audioKeys[keyIdx].startOffset = std::max(0.0f, v);
                    },
                    0.0f,
                    "Offset into the audio stream where playback begins.");
        addKeyFloat(keySection,
                    "End Offset",
                    key.endOffset,
                    "Edit Audio End Offset",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.audioKeys.size())
                            t.audioKeys[keyIdx].endOffset = std::max(0.0f, v);
                    },
                    0.0f,
                    "Offset into the audio stream where playback stops.");
        addKeyFloat(keySection,
                    "Volume dB",
                    key.volumeDb,
                    "Edit Audio Volume",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.audioKeys.size())
                            t.audioKeys[keyIdx].volumeDb = v;
                    },
                    0.0f,
                    "Playback gain for this audio key in decibels.");
        addKeyFloat(keySection,
                    "Pitch",
                    key.pitchScale,
                    "Edit Audio Pitch",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.audioKeys.size())
                            t.audioKeys[keyIdx].pitchScale = std::max(0.01f, v);
                    },
                    1.0f,
                    "Pitch multiplier for this audio key.",
                    0.01f);
        return;
    }

    if (CompositeTrackTypeUsesAnimationKeys(track.type) && m_SelectedTimelineKey < track.animationKeys.size())
    {
        CompositeAnimationKey& key = track.animationKeys[m_SelectedTimelineKey];
        const size_t keyIdx = m_SelectedTimelineKey;
        AddTimelineInspectorAsset(keySection, "Animation", key.animationGuid, {AssetType::Animation, AssetType::Timeline}, &registry,
            [commitKey, keyIdx](const GUID& v) { commitKey("Edit Animation Key Asset", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.animationKeys.size()) t.animationKeys[keyIdx].animationGuid = v; }); },
            "Animation or timeline asset triggered by this key.");
        AddTimelineInspectorText(keySection, "Name", key.animationName,
            [commitKey, keyIdx](const std::string& v) { commitKey("Edit Animation Key Name", [keyIdx, v](CompositeTrack& t) { if (keyIdx < t.animationKeys.size()) t.animationKeys[keyIdx].animationName = v; }); },
            "Named animation to play from the assigned asset.");
        addKeyFloat(keySection,
                    "Time",
                    key.time,
                    "Edit Animation Key Time",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.animationKeys.size())
                            t.animationKeys[keyIdx].time = std::max(0.0f, v);
                    },
                    0.0f,
                    "Key time in seconds.");
        addKeyFloat(keySection,
                    "Speed",
                    key.speedScale,
                    "Edit Animation Key Speed",
                    [keyIdx](CompositeTrack& t, float v)
                    {
                        if (keyIdx < t.animationKeys.size())
                            t.animationKeys[keyIdx].speedScale = std::max(0.0f, v);
                    },
                    1.0f,
                    "Playback speed multiplier for this animation key.");
    }
}

void AnimationWindowPanel::RefreshClipViewEmptyState()
{
    if (!m_DopeSheetView && !m_CurvesGraphView)
        return;
    if (UIElement::IsInEventDispatch())
    {
        PostAction([this]() { RefreshClipViewEmptyState(); });
        return;
    }

    if (m_DopeSheetView)
        ClearElementChildren(m_DopeSheetView);
    if (m_CurvesGraphView)
        ClearElementChildren(m_CurvesGraphView);

    if (m_CurrentClip)
        return;

    UIElement* target = nullptr;
    if (m_ActiveView == ActiveView::DopeSheet)
        target = m_DopeSheetView;
    else if (m_ActiveView == ActiveView::Curves)
        target = m_CurvesGraphView;
    if (!target)
        return;

    AddSidebarLabel(target, "Create a new animation or open an existing one to get started.",
                    "animationwindow-sequencer-empty");
}


AnimationWindowPanel::AnimationWindowPanel()
    : AnimationWindowPanel(PanelKind::Animation)
{
}

AnimationWindowPanel::AnimationWindowPanel(PanelKind kind)
    : DockPanel(GetAnimationPanelTitle(kind))
{
    m_PanelKind = kind;
    m_ActiveView = GetDefaultActiveView();

    // Match root of AnimationWindowPanel.uxml so flex column layout applies (toolbar + content + timeline bar).
    AddClass("animationwindow-panel");
    if (m_PanelKind == PanelKind::Timeline)
        AddClass("animationtimeline-panel");
    else if (m_PanelKind == PanelKind::ClipEditor)
        AddClass("animationclipeditor-panel");
    SetFocusable(true); // So Space can play/pause when panel has focus
    SetId(GetAnimationPanelId(m_PanelKind));  // Stable id so focus/key events resolve to this panel (matches dock registration)
    UpdateTitle();
}

std::string_view AnimationWindowPanel::DeclaredTabIconClass() const
{
    return GetAnimationPanelTabIcon(m_PanelKind);
}

AnimationTimelinePanel::AnimationTimelinePanel()
    : AnimationWindowPanel(PanelKind::Timeline)
{
}

AnimationClipEditorPanel::AnimationClipEditorPanel()
    : AnimationWindowPanel(PanelKind::ClipEditor)
{
}

AnimationWindowPanel::~AnimationWindowPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_LayoutLoadHandle)
        m_LayoutLoadHandle->Cancel();
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    if (m_PropertiesSearchField && m_PropertiesSearchField->GetParent())
        EditorSearchBars::UnregisterFocusTarget(m_PropertiesSearchField->GetParent());
    ModelThumbnailHandler::ClearExternalAnimationPreview();
}

void AnimationWindowPanel::SetContext(const EditorContext* ctx)
{
    m_Context = ctx;
    m_Window = ctx ? ctx->MainWindow : nullptr;
}

bool AnimationWindowPanel::CanResampleBakedChannelAtTreeId(TreeId treeId) const
{
    if (!m_CurrentClip || treeId == 0)
        return false;

    const auto bindingIt = m_ChannelBindings.find(treeId);
    if (bindingIt == m_ChannelBindings.end())
        return false;

    const int channelIndex = bindingIt->second.first;
    if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= m_CurrentClip->GetChannels().size())
        return false;

    const AnimChannel& channel = m_CurrentClip->GetChannels()[static_cast<size_t>(channelIndex)];
    if (channel.keys.size() < 2)
        return false;
    if (channel.interp == AnimInterp::CubicSpline)
        return false;

    if (channel.path == AnimPath::Rotation)
        return channel.interp == AnimInterp::Step;

    return channel.interp == AnimInterp::Step || channel.interp == AnimInterp::Linear;
}

void AnimationWindowPanel::RecomputeAutoTangents(size_t channelIndex)
{
    if (!m_CurrentClipAsset)
        return;

    const std::vector<AnimChannel>& channels = m_CurrentClipAsset->GetChannels();
    if (channelIndex >= channels.size())
        return;

    const AnimChannel& ch = channels[channelIndex];
    if (ch.interp != AnimInterp::CubicSpline)
        return;

    const uint32 compCount = AnimChannelComponentCount(ch);
    const size_t n = ch.keys.size();
    const float fullRange = ComputeChannelValueRange(ch, 0); // use comp 0 for range; per-comp below

    for (size_t ki = 0; ki < n; ++ki)
    {
        for (uint32 c = 0; c < compCount; ++c)
        {
            const float perCompRange = ComputeChannelValueRange(ch, c);
            const AnimTangentType inType  = ch.keys[ki].inTangentType[c];
            const AnimTangentType outType = ch.keys[ki].outTangentType[c];
            (void)fullRange;

            float inSlope  = ch.keys[ki].inTangent[c];
            float outSlope = ch.keys[ki].outTangent[c];
            float inWeight  = ch.keys[ki].inWeight[c];
            float outWeight = ch.keys[ki].outWeight[c];

            auto computeSlope = [&](AnimTangentType type, bool incoming) -> float {
                switch (type)
                {
                case AnimTangentType::Auto:    return ComputeTangentAuto(ch, ki, c);
                case AnimTangentType::Plateau: return ComputeTangentPlateau(ch, ki, c);
                case AnimTangentType::Clamped: return ComputeTangentClamped(ch, ki, c, perCompRange);
                case AnimTangentType::Linear:  return ComputeTangentLinear(ch, ki, c, incoming);
                case AnimTangentType::Flat:    return 0.0f;
                case AnimTangentType::Fixed:   return incoming ? inSlope : outSlope; // preserve slope, only weight can shift
                case AnimTangentType::Custom:  return incoming ? inSlope : outSlope;
                }
                return 0.0f;
            };

            if (inType != AnimTangentType::Custom)
                inSlope = computeSlope(inType, true);
            if (outType != AnimTangentType::Custom)
                outSlope = computeSlope(outType, false);

            m_CurrentClipAsset->SetKeyframeTangent(channelIndex, ch.keys[ki].time, c, true,  inSlope,  inWeight);
            m_CurrentClipAsset->SetKeyframeTangent(channelIndex, ch.keys[ki].time, c, false, outSlope, outWeight);
        }
    }
}

void AnimationWindowPanel::SetTangentTypeOnSelectedKeys(AnimTangentType type)
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<size_t, float>> targets;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
    {
        for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
            if (sk.Channel >= 0)
                targets.emplace_back(static_cast<size_t>(sk.Channel), sk.KeyTime);
    }
    else if (m_DopeSheetView)
    {
        for (const auto& sk : m_DopeSheetView->GetSelectedKeyframes())
            if (sk.Channel >= 0)
                targets.emplace_back(static_cast<size_t>(sk.Channel), sk.KeyTime);
    }
    if (targets.empty() && m_SelectedChannel >= 0 && m_ContextMenuKeyTime >= 0.0f)
        targets.emplace_back(static_cast<size_t>(m_SelectedChannel), m_ContextMenuKeyTime);
    if (targets.empty())
        return;

    // Check whether any targeted key sits on a non-CubicSpline channel that needs upgrading.
    const auto& channels = m_CurrentClipAsset->GetChannels();
    const bool needsSpline = type != AnimTangentType::Linear &&
        std::any_of(targets.begin(), targets.end(), [&](const std::pair<size_t, float>& p)
        {
            return p.first < channels.size() && channels[p.first].interp != AnimInterp::CubicSpline;
        });
    const bool isGltfSource = [&]
    {
        const std::string ext = m_CurrentClipPath.extension().string();
        return ext == ".gltf" || ext == ".glb";
    }();

    auto applyEdit = [this, targets, type]()
    {
        (void)ExecuteClipEditWithUndo("Set Tangent Type", [this, targets, type]()
        {
            bool changed = false;
            std::unordered_set<size_t> affectedChannels;
            for (const auto& [chIdx, t] : targets)
            {
                const auto& chs = m_CurrentClipAsset->GetChannels();
                if (chIdx >= chs.size()) continue;
                if (chs[chIdx].interp != AnimInterp::CubicSpline)
                {
                    if (type == AnimTangentType::Linear)
                    {
                        // Store as per-key segment override — no channel upgrade needed.
                        changed |= m_CurrentClipAsset->SetKeyframeSegmentInterp(chIdx, t, AnimInterp::Linear);
                        continue;
                    }
                    m_CurrentClipAsset->SetChannelInterpolation(chIdx, AnimInterp::CubicSpline);
                }
                const uint32 compCount = AnimChannelComponentCount(chs[chIdx]);
                for (uint32 c = 0; c < compCount; ++c)
                {
                    changed |= m_CurrentClipAsset->SetKeyframeTangentType(chIdx, t, c, true,  type);
                    changed |= m_CurrentClipAsset->SetKeyframeTangentType(chIdx, t, c, false, type);
                }
                affectedChannels.insert(chIdx);
            }
            for (size_t ch : affectedChannels)
                RecomputeAutoTangents(ch);
            return changed;
        });
    };

    if (needsSpline && isGltfSource && m_GltfInterpWarningModal)
    {
        m_GltfInterpWarningModal->SetOnConfirm([applyEdit]() { applyEdit(); });
        m_GltfInterpWarningModal->SetOnCancel({});
        m_GltfInterpWarningModal->Show(
            "glTF Interpolation Warning",
            "glTF does not support per-key interpolation. Applying a spline tangent type will\n"
            "upgrade the entire channel to Cubic Spline, which cannot be round-tripped back\n"
            "to glTF. Save as .anim to preserve this change.",
            "Apply Anyway");
    }
    else
    {
        applyEdit();
    }
}

void AnimationWindowPanel::OnEvent(UIEvent& e)
{
    auto targetHasTextInput = [](UIElement* el) -> bool {
        if (!el) return false;
        if (el->GetAsTextInput()) return true;
        for (const auto& ch : el->GetChildren())
            if (ch && ch->GetAsTextInput()) return true;
        return false;
    };

    // Chord first: it rejects almost every key with two int compares, while the
    // focus test walks the tree — and every view here proxies its focus to this
    // panel, so the panel sees a great many keys.
    if (e.Id == kEventKeyDown &&
        Editor::MatchesCatalogShortcut("Animation", "Save Clip", e.Key, e.Mods) &&
        UI::FocusIsInside(*this))
    {
        (void)SaveCurrentClip();
        // The default chord shares Cmd/Ctrl+Shift+S with the app's Save Scene
        // As; handling it here is what keeps that binding from also firing.
        // Focus is what earns that: the key is offered to a merely-hovered
        // panel too, and consuming it there would swallow Save Scene As.
        e.Stop();
        return;
    }

    // Ctrl+C: Copy selected keyframes (full keyframe data including values and tangents)
    if (e.Id == kEventKeyDown && Input::IsPrimaryShortcutModifier(e.Mods) &&
        !(e.Mods & Input::kModShift) && e.Key == Input::kKeyCode_C)
    {
        m_KeyClipboard.clear();
        if (!m_CurrentClip) { e.Stop(); return; }
        const auto& channels = m_CurrentClip->GetChannels();
        if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
        {
            for (const CurvesGraphView::SelectedKey& k : m_CurvesGraphView->GetSelectedKeyframes())
            {
                if (k.Channel < 0 || static_cast<size_t>(k.Channel) >= channels.size()) continue;
                const auto& channel = channels[k.Channel];
                for (const auto& key : channel.keys)
                {
                    if (std::abs(key.time - k.KeyTime) < 0.0001f)
                    {
                        m_KeyClipboard.push_back({k.Channel, key});
                        break;
                    }
                }
            }
        }
        else if (m_DopeSheetView)
        {
            for (const DopeSheetView::SelectedKey& k : m_DopeSheetView->GetSelectedKeyframes())
            {
                if (k.Channel < 0 || static_cast<size_t>(k.Channel) >= channels.size()) continue;
                const auto& channel = channels[k.Channel];
                for (const auto& key : channel.keys)
                {
                    if (std::abs(key.time - k.KeyTime) < 0.0001f)
                    {
                        m_KeyClipboard.push_back({k.Channel, key});
                        break;
                    }
                }
            }
        }
        e.Stop();
        return;
    }

    // Ctrl+V: Paste keyframes at current time (preserves values and tangents)
    if (e.Id == kEventKeyDown && Input::IsPrimaryShortcutModifier(e.Mods) &&
        !(e.Mods & Input::kModShift) && e.Key == Input::kKeyCode_V)
    {
        if (!m_KeyClipboard.empty() && m_CurrentClipAsset)
        {
            const float t = m_TimelineState.currentTime;
            float anchor = std::numeric_limits<float>::infinity();
            for (const auto& item : m_KeyClipboard) anchor = std::min(anchor, item.Keyframe.time);
            if (std::isfinite(anchor))
            {
                (void)ExecuteClipEditWithUndo("Paste Animation Keys",
                    [this, anchor, t]()
                    {
                        bool changed = false;
                        for (const auto& item : m_KeyClipboard)
                        {
                            AnimKeyframe keyCopy = item.Keyframe;
                            keyCopy.time = t + (item.Keyframe.time - anchor);
                            if (m_CurrentClipAsset->AddKeyframeWithData(static_cast<size_t>(item.Channel), keyCopy))
                                changed = true;
                        }
                        return changed;
                    });
            }
        }
        e.Stop();
        return;
    }

    // Ctrl+A / Ctrl+Shift+A: Godot-style select all / deselect all in the curve editor.
    if (e.Id == kEventKeyDown && Input::IsPrimaryShortcutModifier(e.Mods) &&
        e.Key == Input::kKeyCode_A && !targetHasTextInput(e.Target))
    {
        bool handled = false;
        if ((e.Mods & Input::kModShift) != 0)
        {
            if (m_CurvesGraphView)
                m_CurvesGraphView->ClearSelection();
            if (m_DopeSheetView)
                m_DopeSheetView->ClearSelection();
            UpdateStatsBar();
            UpdateInterpolationButtonState();
            handled = true;
        }
        else if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
        {
            m_CurvesGraphView->SelectAllVisibleKeys();
            handled = true;
        }
        if (handled)
        {
            e.Stop();
            return;
        }
    }

    if (e.Id == kEventKeyDown && !targetHasTextInput(e.Target))
    {
        if (Editor::MatchesCatalogShortcut("Animation", "Play/Pause", e.Key, e.Mods))
        {
            if (m_TimelineState.playing)
            {
                m_TimelineState.paused = true;
                m_TimelineState.playing = false;
            }
            else
            {
                m_TimelineState.playing = true;
                m_TimelineState.paused = false;
            }
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
            UpdatePlayButtonState();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Frame Selected", e.Key, e.Mods))
        {
            if (!FrameSelectionBounds())
                FrameAll();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Frame All", e.Key, e.Mods))
        {
            FrameAll();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Previous Frame", e.Key, e.Mods))
        {
            const float frameDuration = 1.0f / m_TimelineState.fps;
            m_TimelineState.currentTime = std::max(m_TimelineState.rangeStart,
                                                   m_TimelineState.currentTime - frameDuration);
            m_TimelineState.currentTime = std::round(m_TimelineState.currentTime * m_TimelineState.fps) / m_TimelineState.fps;
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
            if (m_Recording)
                InsertKeyAtCurrentTime();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Next Frame", e.Key, e.Mods))
        {
            const float frameDuration = 1.0f / m_TimelineState.fps;
            m_TimelineState.currentTime = std::min(m_TimelineState.rangeEnd,
                                                   m_TimelineState.currentTime + frameDuration);
            m_TimelineState.currentTime = std::round(m_TimelineState.currentTime * m_TimelineState.fps) / m_TimelineState.fps;
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
            if (m_Recording)
                InsertKeyAtCurrentTime();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Add Marker", e.Key, e.Mods))
        {
            (void)ExecutePanelEditWithUndo("Add Animation Marker",
                                           [this]()
                                           {
                                               const float t = std::round(m_TimelineState.currentTime * m_TimelineState.fps) / m_TimelineState.fps;
            m_MarkerTimes.push_back(t);
            if (!m_MarkersElement)
            {
                UIElement* markersEl = FindById("AnimationWindowTimelineMarkers");
                m_MarkersElement = markersEl ? dynamic_cast<TimelineMarkersElement*>(markersEl) : nullptr;
                if (m_MarkersElement)
                {
                    m_MarkersElement->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
                    WireMarkersElementCallbacks();
                }
            }
                                               if (m_MarkersElement)
                m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
                                               return true;
                                           },
                                           false);
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Insert Key", e.Key, e.Mods) ||
            (e.Mods == 0 && (e.Key == Input::kKeyCode_K || e.Key == Input::kKeyCode_Insert)))
        {
            InsertKeyAtCurrentTime();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Duplicate", e.Key, e.Mods))
        {
            if (m_ActiveView == ActiveView::TimeComposite || m_ActiveView == ActiveView::ClipEditor)
                DuplicateSelectedStrip();
            else
                DuplicateSelectedKey();
            e.Stop();
            return;
        }
        if (Editor::MatchesCatalogShortcut("Animation", "Delete Selected", e.Key, e.Mods) ||
            (e.Mods == 0 && e.Key == Input::kKeyCode_Backspace))
        {
            if (m_ActiveView == ActiveView::TimeComposite)
            {
                const bool hasTimelineKeySelection =
                    m_TimeCompositeView &&
                    (!m_TimeCompositeView->GetSelectedTrackKeys().empty() ||
                     (m_SelectedTimelineKeyTrack != static_cast<size_t>(-1) &&
                      m_SelectedTimelineKey != static_cast<size_t>(-1)));
                const bool hasClipSelection =
                    m_CompositeModel &&
                    m_SelectedCompositeTrack < m_CompositeModel->tracks.size() &&
                    m_SelectedCompositeClip != static_cast<size_t>(-1) &&
                    m_SelectedCompositeClip < m_CompositeModel->tracks[m_SelectedCompositeTrack].clips.size();
                if (hasTimelineKeySelection)
                    DeleteSelectedTimelineKeys();
                else if (hasClipSelection)
                    DeleteSelectedStrip();
            }
            else if (m_ActiveView == ActiveView::ClipEditor)
                DeleteSelectedStrip();
            else
                DeleteSelectedKey();
            e.Stop();
            return;
        }
    }

    if (e.Id == kEventKeyDown && e.Mods == 0)
    {
        // Don't consume shortcuts when a text field is the event source.
        // Key events bubble up from focused fields through the panel hierarchy.
        if (targetHasTextInput(e.Target))
            return;

        if (e.Key == Input::kKeyCode_Escape && m_CurveOptionsMode != CurveOptionsMode::None)
        {
            ExitCurveOptionsMode(false);
            e.Stop();
            return;
        }

        // Lattice tool: [ / ] change the number of control points; \ toggles
        // between the Bezier (default, all keys deform) and Catmull-Rom
        // (interpolating, curve passes through every CP) deformation bases.
        if (m_CurvesGraphView &&
            m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice &&
            (e.Key == Input::kKeyCode_LeftBracket || e.Key == Input::kKeyCode_RightBracket))
        {
            m_CurvesGraphView->BumpLatticePointCount(e.Key == Input::kKeyCode_RightBracket ? +1 : -1);
            e.Stop();
            return;
        }
        if (m_CurvesGraphView &&
            m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice &&
            e.Key == Input::kKeyCode_Backslash)
        {
            m_CurvesGraphView->ToggleLatticeBasis();
            e.Stop();
            return;
        }

        if (e.Key == Input::kKeyCode_S)
        {
            m_ScrollWithPlayhead = !m_ScrollWithPlayhead;
            UpdateSyncPlayheadButtonState();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_B)
        {
            // B: Toggle tangent mode for selected keys (break if unified, unify if broken)
            ToggleTangentMode();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Comma)
        {
            GoToPreviousKey();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Period)
        {
            GoToNextKey();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_Up || e.Key == Input::kKeyCode_Down)
        {
            if (e.Key == Input::kKeyCode_Up)
                GoToPreviousKey();
            else
                GoToNextKey();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_C)
        {
            CenterViewOnCurrentTime();
            e.Stop();
            return;
        }
        if (e.Key == Input::kKeyCode_P)
        {
            FramePlaybackRange();
            e.Stop();
            return;
        }
    }
    // Ctrl+Left/Right: nudge selected keyframes by one frame in time.
    if (e.Id == kEventKeyDown && Input::IsPrimaryShortcutModifier(e.Mods) &&
        !(e.Mods & Input::kModShift) &&
        m_CurvesGraphView && m_CurrentClip && m_CurrentClipAsset &&
        (e.Key == Input::kKeyCode_Left || e.Key == Input::kKeyCode_Right))
    {
        const auto& selRef = m_CurvesGraphView->GetSelectedKeyframes();
        if (!selRef.empty())
        {
            std::vector<CurvesGraphView::SelectedKey> keys(selRef.begin(), selRef.end());
            const bool isForward = (e.Key == Input::kKeyCode_Right);
            const float frameDelta = (isForward ? 1.0f : -1.0f) / m_TimelineState.fps;

            if (!EnsureEditableClip() || !m_CurrentClipAsset) { e.Stop(); return; }

            // Process in order that avoids key-time collisions.
            std::sort(keys.begin(), keys.end(), [isForward](const auto& a, const auto& b) {
                return isForward ? a.KeyTime > b.KeyTime : a.KeyTime < b.KeyTime;
            });
            (void)ExecuteClipEditWithUndo("Nudge Keyframe Time",
                [this, keys, frameDelta]() -> bool {
                    bool changed = false;
                    for (const auto& sk : keys)
                    {
                        if (sk.Channel < 0) continue;
                        const float newT = std::max(0.0f, sk.KeyTime + frameDelta);
                        if (m_CurrentClipAsset->SetKeyframeTime(static_cast<size_t>(sk.Channel), sk.KeyTime, newT))
                        {
                            RecomputeAutoTangents(static_cast<size_t>(sk.Channel));
                            changed = true;
                        }
                    }
                    return changed;
                });
            if (m_CurvesGraphView)
            {
                m_CurvesGraphView->ClearSelection();
                for (const auto& sk : keys)
                    if (sk.Channel >= 0)
                        m_CurvesGraphView->SelectKeyAtTime(sk.Channel, std::max(0.0f, sk.KeyTime + frameDelta));
            }
            e.Stop();
            return;
        }
    }
    if (e.Id != kEventScroll)
        return;
    auto isUnderTimelineBar = [this](UIElement* el) -> bool
    {
        for (UIElement* p = el; p != nullptr; p = p->GetParent())
        {
            if (p == m_TimelineBar || p == m_TimeLabelsBar || p == m_TimelineLeftBar)
                return true;
        }
        return false;
    };
    auto isUnderCurvesView = [this](UIElement* el) -> bool
    {
        for (UIElement* p = el; p != nullptr; p = p->GetParent())
        {
            if (p == m_CurvesGraphView)
                return true;
        }
        return false;
    };
    auto isUnderContentArea = [this](UIElement* el) -> bool
    {
        for (UIElement* p = el; p != nullptr; p = p->GetParent())
        {
            if (p == m_ContentArea)
                return true;
        }
        return false;
    };
    // Vertical zoom on curves: Control+scroll over curves view.
    if (isUnderCurvesView(e.Target) && m_CurvesGraphView && (e.Mods & Input::kModControl) != 0 && e.ScrollY != 0.0f)
    {
        m_CurvesGraphView->ApplyValueZoom(e.ScrollY);
        e.Stop();
        return;
    }
    // Timeline (horizontal) zoom: scroll over timeline/curves/content area, or Control+scroll elsewhere.
    if (isUnderTimelineBar(e.Target) || isUnderCurvesView(e.Target) || isUnderContentArea(e.Target) || (e.Mods & Input::kModControl) != 0)
    {
        ApplyTimelineZoom(e.ScrollY, e.X);
        e.Stop();
    }
}

void AnimationWindowPanel::UpdateTimelineViewHeaderOffsets()
{
    if (m_PropertiesTree && m_DopeSheetView)
    {
        const float offset = m_PropertiesTree->GetViewportLayoutY() - m_DopeSheetView->GetLayoutY();
        m_DopeSheetView->SetRowTopOffset(offset);
    }
    if (m_PropertiesTree && m_TimeCompositeView)
    {
        float offset = m_PropertiesTree->GetViewportLayoutY() - m_TimeCompositeView->GetLayoutY();
        if (m_ActiveView == ActiveView::TimeComposite && m_SequencerSidebar)
        {
            const auto& sidebarChildren = m_SequencerSidebar->GetChildren();
            if (sidebarChildren.size() >= 2u && sidebarChildren[1])
                offset = sidebarChildren[1]->GetLayoutY() - m_TimeCompositeView->GetLayoutY();
        }
        m_TimeCompositeView->SetHeaderOffset(offset);
    }
    if (m_PropertiesTree && m_LaneClipEditorView)
    {
        float offset = m_PropertiesTree->GetViewportLayoutY() - m_LaneClipEditorView->GetLayoutY();
        if (m_ActiveView == ActiveView::ClipEditor && m_SequencerSidebar)
        {
            const auto& sidebarChildren = m_SequencerSidebar->GetChildren();
            if (sidebarChildren.size() >= 2u && sidebarChildren[1])
                offset = sidebarChildren[1]->GetLayoutY() - m_LaneClipEditorView->GetLayoutY();
        }
        m_LaneClipEditorView->SetHeaderOffset(offset);
    }
}

void AnimationWindowPanel::OnPostLayout()
{
    UpdateTimelineViewHeaderOffsets();
    if (m_NeedsInitialFrame && GetLayoutWidth() > 0.0f)
    {
        m_NeedsInitialFrame = false;
        FrameAll();
    }
    if (m_BindApplied || m_BindPending || m_BindFailed)
        return;
    if (!GetOwnerManager())
        return;

    m_BindPending = true;
    // A dropped action never runs, so it would never release the latch: release it here
    // instead and let the next laid-out frame try again.
    if (!this->PostAction([this]()
                          { this->BindFromAssetsDeferred(); }))
        m_BindPending = false;
}

void AnimationWindowPanel::BindFromAssetsDeferred()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the
        // latch and let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    // m_BindPending stays set: LoadBindAttachLayoutAndStyle hands it to the async
    // load, whose completion is what resolves it.
    // INVARIANT: this call must wire NOTHING. The layout subtree is created by
    // BindLayoutToSubtreeChildrenFromAsset, which runs only inside the async load
    // callback below, so every FindById here resolves to null and every wiring guard
    // falls through. The second call, from that callback, is the one that wires.
    //
    // Clicks are additive handler-table subscriptions, so if a synchronous bind fast
    // path is ever added here (GameViewPanel has that shape: a cached GetAsset that
    // binds outright), BOTH calls would wire the same button instances and one click
    // would fire twice. Enforcement is that the bind is reachable only through
    // PostAction/async completion -- keep it that way, or make the second call
    // conditional on the first having wired nothing.
    LoadBindAttachLayoutAndStyle(ui);
    RefreshElementPointers();
    WireAnimationControls();
}

void AnimationWindowPanel::MarkBindFailed(std::string_view reason)
{
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error(
        "AnimationWindowPanel: layout bind failed ({}). The panel stays unbound; "
        "check that UI/panels/AnimationWindowPanel.uxml is staged under the editor asset mount.",
        reason);
}

void AnimationWindowPanel::LoadBindAttachLayoutAndStyle(UIManager* ui)
{
    (void)ui;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path layoutAssetPath = std::filesystem::path("UI") / "panels" / "AnimationWindowPanel.uxml";
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "AnimationWindowPanel.css";

    const GUID layoutGuid = am.ResolveAssetGuid(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    // The stylesheet is independent of the layout: a panel that fails to bind its
    // .uxml still wants its own styles on the root, so issue this before the
    // layout outcome can return.
    if (!styleGuid.IsNull() && !m_PanelStyleLoadHandle)
    {
        m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
            am.LoadAsset(styleGuid,
                         [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                                 return;
                             post.Post([this, styleGuid]()
                                              {
                                                  UIManager* ui3 = GetOwnerManager();
                                                  if (!ui3)
                                                      return;
                                                  auto& am3 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a3 = am3.GetAsset(styleGuid);
                                                  if (a3 && a3->GetType() == AssetType::UIStyle)
                                                  {
                                                      (void)ui3->AttachStyleToSubtreeFromAsset(
                                                          this, *static_cast<UIStyleAsset*>(a3.get()));
                                                  }
                                              });
                         },
                         AssetLoadPriority::High));
    }

    if (layoutGuid.IsNull())
    {
        MarkBindFailed("the layout guid did not resolve from the editor asset source");
        return;
    }

    // Reached once per panel: OnPostLayout holds m_BindPending for the whole attempt,
    // so there is no second entry to guard against and every exit below resolves it.
    m_LayoutLoadHandle = std::make_unique<AssetLoadHandle>(
        am.LoadAsset(layoutGuid,
                     [this, post = GetPostHandle(), layoutGuid](Result<SharedPtr<Asset>, AssetError> r)
                     {
                         // The load completes on a worker; every latch mutation below
                         // runs on the UI thread through PostAction.
                         const bool loaded = r.IsOk() && r.Value() &&
                                             r.Value()->GetType() == AssetType::UILayout;
                         post.Post([this, layoutGuid, loaded]()
                                          {
                                              if (!loaded)
                                                  return MarkBindFailed("the layout asset did not load");
                                              UIManager* ui2 = GetOwnerManager();
                                              if (!ui2 || m_BindApplied)
                                              {
                                                  m_BindPending = false;
                                                  return;
                                              }
                                              auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                              auto a2 = am2.GetAsset(layoutGuid);
                                              if (!a2 || a2->GetType() != AssetType::UILayout)
                                                  return MarkBindFailed("the loaded layout asset is not a UILayout");
                                              const bool ok = ui2->BindLayoutToSubtreeChildrenFromAsset(
                                                  this, *static_cast<UILayoutAsset*>(a2.get()));
                                              if (!ok)
                                                  return MarkBindFailed("binding the layout into the panel subtree was rejected");

                                              m_BindApplied = true;
                                              m_BindPending = false;
                                              RefreshElementPointers();
                                              WireAnimationControls();
                                          });
                     },
                     AssetLoadPriority::High));
}

void AnimationWindowPanel::RefreshElementPointers()
{
    m_Toolbar = FindById("AnimationWindowToolbar");
    if (UIElement* ind = FindById("AnimationWindowUnsavedIndicator"))
        m_UnsavedIndicator = dynamic_cast<Label*>(ind);
    m_ContentArea = FindById("AnimationWindowContentArea");
    m_PropertiesPane = FindById("AnimationWindowPropertiesPane");
    m_PropertiesHeader = FindById("AnimationWindowPropertiesHeader");
    if (UIElement* searchEl = FindById("AnimationWindowPropertiesSearch"))
        m_PropertiesSearchField = dynamic_cast<TextField*>(searchEl);
    if (UIElement* btn = FindById("AnimationWindowFilterAnimated"))
        m_FilterAnimatedButton = dynamic_cast<Button*>(btn);
    if (UIElement* btn = FindById("AnimationWindowFilterHierarchy"))
        m_FilterHierarchyButton = dynamic_cast<Button*>(btn);
    if (UIElement* btn = FindById("AnimationWindowFilterChannelsOnly"))
        m_FilterChannelsOnlyButton = dynamic_cast<Button*>(btn);
    if (UIElement* btn = FindById("AnimationWindowFilterFlat"))
        m_FilterFlatButton = dynamic_cast<Button*>(btn);
    m_PropertiesFilterRow = FindById("AnimationWindowPropertiesFilterRow");
    m_SequencerSidebar = FindById("AnimationWindowSequencerSidebar");
    m_StatsBar = FindById("AnimationWindowStatsBar");
    if (!m_StatsBar && m_Toolbar) m_StatsBar = m_Toolbar->FindById("AnimationWindowStatsBar");
    if (UIElement* tf = FindById("AnimationWindowStatsTime"))
        m_StatsTimeField = dynamic_cast<TextField*>(tf);
    if (UIElement* tf = FindById("AnimationWindowStatsValue"))
        m_StatsValueField = dynamic_cast<TextField*>(tf);
    // Find the T: and V: labels for drag support
    if (m_StatsBar)
    {
        for (auto& child : m_StatsBar->GetChildren())
        {
            if (auto* lbl = dynamic_cast<Label*>(child.get()))
            {
                const std::string& txt = lbl->GetText();
                if (txt == "T:") m_StatsTimeLabel = lbl;
                else if (txt == "V:") m_StatsValueLabel = lbl;
            }
        }
    }
    // Curve-edit options bar (Add Noise / Simplify / Smooth)
    m_OptionsBar          = FindById("AnimationWindowOptionsBar");
    m_OptionsNoisePanel   = FindById("AnimationWindowOptionsNoise");
    m_OptionsSimplifyPanel= FindById("AnimationWindowOptionsSimplify");
    m_OptionsSmoothPanel  = FindById("AnimationWindowOptionsSmooth");
    m_OptionsLatticePanel = FindById("AnimationWindowOptionsLattice");
    if (UIElement* el = FindById("AnimationWindowLatticePointCount"))      m_LatticePointCountField = dynamic_cast<IntField*>(el);
    if (UIElement* el = FindById("AnimationWindowLatticePointCountLabel")) m_LatticePointCountLabel = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowLatticeBasis"))           m_LatticeBasisDropdown   = dynamic_cast<Dropdown*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseFreqMin"))      m_NoiseFreqMinField     = dynamic_cast<IntField*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseFreqMax"))      m_NoiseFreqMaxField     = dynamic_cast<IntField*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseMagnitude"))    m_NoiseMagnitudeField   = dynamic_cast<FloatField*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyMethod"))    m_SimplifyMethodDropdown= dynamic_cast<Dropdown*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyTimeTol"))   m_SimplifyTimeTolField  = dynamic_cast<FloatField*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyValueTol")) m_SimplifyValueTolField = dynamic_cast<FloatField*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothFilterWidth"))m_SmoothFilterWidthField= dynamic_cast<IntField*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothSampleCount"))m_SmoothSampleCountField= dynamic_cast<IntField*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothTitle"))      m_SmoothTitleLabel      = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseFreqMinLabel"))      m_NoiseFreqMinLabel      = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseFreqMaxLabel"))      m_NoiseFreqMaxLabel      = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseMagnitudeLabel"))    m_NoiseMagnitudeLabel    = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyTimeTolLabel"))   m_SimplifyTimeTolLabel   = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyValueTolLabel")) m_SimplifyValueTolLabel = dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothFilterWidthLabel"))m_SmoothFilterWidthLabel= dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothSampleCountLabel"))m_SmoothSampleCountLabel= dynamic_cast<Label*>(el);
    if (UIElement* el = FindById("AnimationWindowNoiseConfirm"))     m_NoiseConfirmBtn       = dynamic_cast<Button*>(el);
    if (UIElement* el = FindById("AnimationWindowSimplifyConfirm"))  m_SimplifyConfirmBtn    = dynamic_cast<Button*>(el);
    if (UIElement* el = FindById("AnimationWindowSmoothConfirm"))    m_SmoothConfirmBtn      = dynamic_cast<Button*>(el);

    m_TimelineLeftBar = FindById("AnimationWindowTimelineLeftBar");
    m_TimelineBar = FindById("AnimationWindowTimelineRightBar");
    m_TimeLabelsBar = FindById("AnimationWindowTimeLabelsBar");
    UIElement* treeEl = FindById("AnimationWindowPropertiesTree");
    m_PropertiesTree = treeEl ? dynamic_cast<TreeView*>(treeEl) : nullptr;
    if (m_PropertiesTree)
    {
        if (!m_PropertiesTreeSelection)
            m_PropertiesTreeSelection = std::make_unique<AnimatablePropertiesSelection>();
        m_PropertiesTree->SetSelectionModel(m_PropertiesTreeSelection.get());
        {
            const float rowH = std::clamp(AnimationWindowSettings::Get().PropertiesTreeRowHeight,
                                          kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
            m_PropertiesTree->SetRowHeight(rowH);
            if (m_DopeSheetView) m_DopeSheetView->SetRowHeight(rowH);
        }
        m_PropertiesTree->SetOnItemResizeGesture([this](float scrollY)
        {
            if (!m_PropertiesTree)
                return;
            const float current = m_PropertiesTree->GetRowHeight();
            const float newH = std::clamp(current + scrollY * -2.0f,
                                          kMinEditorTreeRowHeightPx, kMaxEditorTreeRowHeightPx);
            m_PropertiesTree->SetRowHeight(newH);
            if (m_DopeSheetView) m_DopeSheetView->SetRowHeight(newH);
            auto& s = AnimationWindowSettings::Get();
            s.PropertiesTreeRowHeight = newH;
            s.Save();
        });
        m_PropertiesTree->SetChildIndent(16.0f);
        m_PropertiesTree->SetIconSize(12.0f);
        m_PropertiesTree->SetShowRoot(true);
        m_PropertiesTree->SetClearSelectionOnBackgroundClick(false);
        m_PropertiesTree->SetOnRowBound([this](TreeId id, UIElement* row)
        {
            if (!row)
                return;
            row->RemoveClass("tree-component-x");
            row->RemoveClass("tree-component-y");
            row->RemoveClass("tree-component-z");
            row->RemoveClass("tree-component-w");
            const auto it = m_ComponentLeafIndex.find(id);

            static const char* const kCompClasses[] = {
                "tree-component-x", "tree-component-y", "tree-component-z", "tree-component-w"};

            // Find existing flat suffix label (may have been added on a previous rebind of this row).
            Label* suffixLabel = nullptr;
            for (auto& child : row->GetChildren())
            {
                if (child->HasClass("tree-flat-component-suffix"))
                {
                    suffixLabel = dynamic_cast<Label*>(child.get());
                    break;
                }
            }

            const bool isFlatLeaf = m_PropertyFilter == PropertyFilter::Flat
                                 && it != m_ComponentLeafIndex.end();
            if (isFlatLeaf)
            {
                // Color only the component letter in the label, not the entire row text.
                const uint32 compIdx = it->second;
                const char* compLetters[] = {"X", "Y", "Z", "W"};
                const char* compLetter = compIdx < 4 ? compLetters[compIdx] : "";

                if (!suffixLabel)
                {
                    auto sl = std::make_unique<Label>();
                    sl->AddClass("tree-flat-component-suffix");
                    suffixLabel = sl.get();
                    row->AddChild(std::move(sl));
                }
                for (const char* cls : kCompClasses) suffixLabel->RemoveClass(cls);
                if (compIdx < 4) suffixLabel->AddClass(kCompClasses[compIdx]);
                suffixLabel->SetText(std::string(compLetter));
            }
            else
            {
                // Non-flat leaf: apply whole-row component coloring as before.
                if (it != m_ComponentLeafIndex.end() && it->second < 4u)
                    row->AddClass(kCompClasses[it->second]);

                // Clear any leftover suffix from row recycling within the same view.
                if (suffixLabel)
                    suffixLabel->SetText("");
            }
            if (m_BoneTreeIds.count(id) && AnimationWindowSettings::Get().ShowBoneIcons)
                row->AddClass("anim-tree-bone");
            else
                row->RemoveClass("anim-tree-bone");
            EnsurePropertyTreeRowActions(row);
            m_RowToTreeId[row] = id;
            UpdatePropertyTreeRowActionState(id, row);
        });
        m_PropertiesTree->SetOnSelectionChanged([this](TreeId id)
        {
            if (m_SuppressSelectionUndo) return;

            const SelectionSnapshot before = CaptureSelectionSnapshot();

            // Tree selection sets the editable scope — drop any dopesheet key selection
            // so the orange selection box doesn't linger over rows that just became
            // non-selectable (selection follows scope).
            if (m_DopeSheetView)
                m_DopeSheetView->ClearSelection();

            m_SelectedTreeId = id;

            // Collect channels for the entire multi-selection, not just the last clicked item.
            std::unordered_set<int> allChannels;
            if (m_PropertiesTreeSelection)
            {
                for (UI::Interaction::ItemId selId : m_PropertiesTreeSelection->GetSelection())
                {
                    const auto nodeIt = m_TreeNodeChannels.find(static_cast<TreeId>(selId));
                    if (nodeIt != m_TreeNodeChannels.end())
                        for (int ch : nodeIt->second) allChannels.insert(ch);
                }
            }

            // Primary channel/component from the most-recently clicked item.
            const auto bindIt = m_ChannelBindings.find(id);
            if (bindIt != m_ChannelBindings.end())
            {
                m_SelectedChannel = bindIt->second.first;
                m_SelectedComponent = bindIt->second.second;
            }
            else
            {
                const auto channelsIt = m_TreeNodeChannels.find(id);
                if (channelsIt != m_TreeNodeChannels.end() && !channelsIt->second.empty())
                {
                    m_SelectedChannel = channelsIt->second.front();
                    m_SelectedComponent = 0u;
                }
            }

            if (allChannels.empty())
            {
                const auto channelsIt = m_TreeNodeChannels.find(id);
                if (channelsIt != m_TreeNodeChannels.end())
                    for (int ch : channelsIt->second) allChannels.insert(ch);
            }

            m_VisibleChannels.assign(allChannels.begin(), allChannels.end());
            std::sort(m_VisibleChannels.begin(), m_VisibleChannels.end());
            RefreshChannelSelection(false);

            const SelectionSnapshot after = CaptureSelectionSnapshot();
            m_LastSelectionSnapshot = after;
            PushSelectionUndo(before, after);
        });
        m_PropertiesTree->SetOnContextMenu([this](TreeId id, float x, float y)
        {
            if (!m_Window)
                return;

            m_ContextMenuTreeId = id;
            if (!m_PropertiesTreeContextMenu)
            {
                m_PropertiesTreeContextMenu = CreateContextMenu();
                if (!m_PropertiesTreeContextMenu)
                    return;

                m_PropertiesTreeContextMenu->SetCommandHandler(
                    [this](uint32_t cmd)
                    {
                        auto applyTag = [this](uint32_t tag)
                        {
                            if (m_ContextMenuTreeId == 0) return;
                            for (TreeId selId : GetEffectiveSelection(m_ContextMenuTreeId))
                            {
                                if (tag == 0u) m_TreeColorTags.erase(selId);
                                else m_TreeColorTags[selId] = tag;
                            }
                            if (m_PropertiesTree) m_PropertiesTree->RefreshFromProvider();
                        };
                        switch (cmd)
                        {
                            case kCmdAnimationColorTagNone:   applyTag(0u); return;
                            case kCmdAnimationColorTagYellow: applyTag(1u); return;
                            case kCmdAnimationColorTagRed:    applyTag(2u); return;
                            case kCmdAnimationColorTagGreen:  applyTag(3u); return;
                            case kCmdAnimationColorTagBlue:   applyTag(4u); return;
                            default: break;
                        }

                        // Pre/Post-Infinity extrapolation
                        {
                            const bool isPre  = (cmd >= kCmdPreInfConstant  && cmd <= kCmdPreInfOscillate);
                            const bool isPost = (cmd >= kCmdPostInfConstant && cmd <= kCmdPostInfOscillate);
                            if (isPre || isPost)
                            {
                                const auto infMode = static_cast<AnimExtrapolation>(isPre
                                    ? (cmd - kCmdPreInfConstant)
                                    : (cmd - kCmdPostInfConstant));
                                const auto bindingIt = m_ChannelBindings.find(m_ContextMenuTreeId);
                                if (bindingIt == m_ChannelBindings.end()) return;
                                const int channelIndex = bindingIt->second.first;
                                if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
                                const size_t chIdx = static_cast<size_t>(channelIndex);
                                const AnimExtrapolation existingPre  = (chIdx < m_CurrentClip->GetChannels().size())
                                    ? m_CurrentClip->GetChannels()[chIdx].preInfinity  : AnimExtrapolation::Constant;
                                const AnimExtrapolation existingPost = (chIdx < m_CurrentClip->GetChannels().size())
                                    ? m_CurrentClip->GetChannels()[chIdx].postInfinity : AnimExtrapolation::Constant;
                                (void)ExecuteClipEditWithUndo(
                                    isPre ? "Set Pre-Infinity" : "Set Post-Infinity",
                                    [this, chIdx, isPre, infMode, existingPre, existingPost]()
                                    {
                                        m_CurrentClipAsset->SetChannelExtrapolation(
                                            chIdx,
                                            isPre ? infMode : existingPre,
                                            isPre ? existingPost : infMode);
                                        return true;
                                    });
                                return;
                            }
                        }

                        if (cmd != kCmdAnimationResampleBakedToCurve)
                            return;
                        if (!CanResampleBakedChannelAtTreeId(m_ContextMenuTreeId))
                            return;

                        // Resolve channel before EnsureEditableClip(): that path calls RefreshPropertyTree() and
                        // replaces TreeIds, so m_ContextMenuTreeId would no longer match m_ChannelBindings.
                        const auto bindingIt = m_ChannelBindings.find(m_ContextMenuTreeId);
                        if (bindingIt == m_ChannelBindings.end())
                            return;
                        const int channelIndex = bindingIt->second.first;

                        if (!EnsureEditableClip() || !m_CurrentClipAsset)
                            return;

                        (void)ExecuteClipEditWithUndo(
                            "Resample Baked Keys to Curve",
                            [this, channelIndex]()
                            {
                                return m_CurrentClipAsset->ResampleBakedChannelToCurve(static_cast<size_t>(channelIndex));
                            });
                    });

                m_PropertiesTreeContextMenu->SetStateProvider(
                    [this](uint32_t cmdId) -> MenuItemState
                    {
                        MenuItemState st{};
                        if (cmdId == kCmdAnimationResampleBakedToCurve)
                        {
                            st.Enabled = CanResampleBakedChannelAtTreeId(m_ContextMenuTreeId);
                            st.Checked = false;
                        }
                        const bool isPre  = (cmdId >= kCmdPreInfConstant  && cmdId <= kCmdPreInfOscillate);
                        const bool isPost = (cmdId >= kCmdPostInfConstant && cmdId <= kCmdPostInfOscillate);
                        if ((isPre || isPost) && m_CurrentClip)
                        {
                            const auto bindingIt = m_ChannelBindings.find(m_ContextMenuTreeId);
                            if (bindingIt != m_ChannelBindings.end())
                            {
                                const size_t chIdx = static_cast<size_t>(bindingIt->second.first);
                                if (chIdx < m_CurrentClip->GetChannels().size())
                                {
                                    const AnimChannel& ch = m_CurrentClip->GetChannels()[chIdx];
                                    const auto mode = isPre ? ch.preInfinity : ch.postInfinity;
                                    const uint32_t base = isPre ? kCmdPreInfConstant : kCmdPostInfConstant;
                                    st.Checked = (cmdId == base + static_cast<uint32_t>(mode));
                                }
                            }
                        }
                        return st;
                    });
            }

            m_PropertiesTreeContextMenu->Clear();
            ContextMenuBuilder builder;
            builder.AddItem("Resample baked keys to curve", kCmdAnimationResampleBakedToCurve, MenuItemFlag_None, 0, EditorIcons::kReset);
            const uint32_t currentTag = [this]() -> uint32_t {
                const auto it = m_TreeColorTags.find(m_ContextMenuTreeId);
                return it != m_TreeColorTags.end() ? it->second : 0u;
            }();
            auto tagFlag = [&currentTag](uint32_t tag) -> uint32_t {
                return MenuItemFlag_Radio |
                       (currentTag == tag ? MenuItemFlag_Checked : MenuItemFlag_None);
            };
            builder.AddItem("Color Tag", 0, MenuItemFlag_None, 0, EditorIcons::kColorPicker);
            builder.AddItem("Pre-Infinity", 0, MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Post-Infinity", 0, MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Color Tag/None",   kCmdAnimationColorTagNone,   tagFlag(0u), 0);
            builder.AddItem("Color Tag/Yellow", kCmdAnimationColorTagYellow, tagFlag(1u), 0);
            builder.AddItem("Color Tag/Red",    kCmdAnimationColorTagRed,    tagFlag(2u), 0);
            builder.AddItem("Color Tag/Green",  kCmdAnimationColorTagGreen,  tagFlag(3u), 0);
            builder.AddItem("Color Tag/Blue",   kCmdAnimationColorTagBlue,   tagFlag(4u), 0);
            builder.AddItem("Pre-Infinity/Constant",     kCmdPreInfConstant,    MenuItemFlag_None, 0, EditorIcons::kSteppedCurve);
            builder.AddItem("Pre-Infinity/Linear",       kCmdPreInfLinear,      MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
            builder.AddItem("Pre-Infinity/Cycle",        kCmdPreInfCycle,       MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Pre-Infinity/Cycle+Offset", kCmdPreInfCycleOffset, MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Pre-Infinity/Oscillate",    kCmdPreInfOscillate,   MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Post-Infinity/Constant",     kCmdPostInfConstant,    MenuItemFlag_None, 0, EditorIcons::kSteppedCurve);
            builder.AddItem("Post-Infinity/Linear",       kCmdPostInfLinear,      MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
            builder.AddItem("Post-Infinity/Cycle",        kCmdPostInfCycle,       MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Post-Infinity/Cycle+Offset", kCmdPostInfCycleOffset, MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.AddItem("Post-Infinity/Oscillate",    kCmdPostInfOscillate,   MenuItemFlag_None, 0, EditorIcons::kLoop);
            builder.Build(m_PropertiesTreeContextMenu.get());
            m_PropertiesTreeContextMenu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
        });
        m_PropertiesTree->SetOnExpansionChanged([this](TreeId, bool)
        {
            BuildDopeSheetTrackRows();
        });
        m_PropertiesTree->SetOnScrollChanged([this](float scrollY)
        {
            if (m_DopeSheetView)
                m_DopeSheetView->SetScrollOffset(scrollY);
        });
        RefreshPropertyTree();
    }
    // Route focus to this panel when user clicks content, timeline, or properties pane, so Space play/pause works.
    if (m_ContentArea)
        m_ContentArea->SetFocusProxy(this);
    if (m_PropertiesPane)
        m_PropertiesPane->SetFocusProxy(this);
    if (m_TimelineBar)
        m_TimelineBar->SetFocusProxy(this);
    if (m_TimelineLeftBar)
        m_TimelineLeftBar->SetFocusProxy(this);
    if (m_Toolbar)
        m_Toolbar->SetFocusProxy(this);
    UIElement* rulerEl = FindById("AnimationWindowTimelineRuler");
    m_TimelineBarElement = rulerEl ? dynamic_cast<TimelineBarElement*>(rulerEl) : nullptr;
    UIElement* rangeSliderEl = FindById("AnimationWindowTimeRangeSlider");
    m_TimeRangeSlider = rangeSliderEl ? dynamic_cast<TimeRangeSliderElement*>(rangeSliderEl) : nullptr;
    if (m_TimeRangeSlider)
    {
        m_TimeRangeSlider->SetOnRangeChanged([this](float newStart, float newEnd)
        {
            const float minDuration = 1.0f / std::max(1.0f, m_TimelineState.fps);
            const float fullStart = m_TimelineState.fullStart;
            const float fullEnd = std::max(fullStart + minDuration, m_TimelineState.fullEnd);
            m_TimelineState.rangeStart = std::clamp(newStart, fullStart, fullEnd - minDuration);
            m_TimelineState.rangeEnd = std::clamp(newEnd, m_TimelineState.rangeStart + minDuration, fullEnd);
            m_TimelineState.viewStart = m_TimelineState.rangeStart;
            m_TimelineState.viewEnd = m_TimelineState.rangeEnd;
            UpdateTimelineRulerAndLabels();
        });
        m_TimeRangeSlider->SetOnSeekToTime([this](float t)
        {
            t = std::round(t * m_TimelineState.fps) / m_TimelineState.fps;
            m_TimelineState.currentTime = std::clamp(t, m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
        });
    }
    m_RangeStartField = dynamic_cast<IntField*>(FindById("AnimationWindowRangeStart"));
    m_RangeEndField   = dynamic_cast<IntField*>(FindById("AnimationWindowRangeEnd"));
    if (m_RangeStartField)
    {
        m_RangeStartField->SetValue(static_cast<int>(std::round(m_TimelineState.rangeStart * m_TimelineState.fps)));
        auto applyRangeStartFrame = [this](const int& frame)
        {
            const float newStart = static_cast<float>(std::max(0, frame)) / m_TimelineState.fps;
            const float minDuration = 1.0f / std::max(1.0f, m_TimelineState.fps);
            m_TimelineState.rangeStart =
                std::clamp(newStart, m_TimelineState.fullStart, m_TimelineState.rangeEnd - minDuration);
            m_TimelineState.viewStart = m_TimelineState.rangeStart;
            m_TimelineState.viewEnd = m_TimelineState.rangeEnd;
            m_TimelineState.currentTime = std::clamp(m_TimelineState.currentTime,
                                                     m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
        };
        m_RangeStartField->SetOnValueChanging(applyRangeStartFrame);
        m_RangeStartField->SetOnValueChanged(applyRangeStartFrame);
    }
    if (m_RangeEndField)
    {
        m_RangeEndField->SetValue(static_cast<int>(std::round(m_TimelineState.rangeEnd * m_TimelineState.fps)));
        auto applyRangeEndFrame = [this](const int& frame)
        {
            const float minDuration = 1.0f / std::max(1.0f, m_TimelineState.fps);
            const float newDuration = static_cast<float>(std::max(1, frame)) / m_TimelineState.fps;
            const float fullEnd = std::max(m_TimelineState.fullStart + minDuration, m_TimelineState.fullEnd);
            m_TimelineState.rangeEnd =
                std::clamp(newDuration, m_TimelineState.rangeStart + minDuration, fullEnd);
            if (m_CurrentClipAsset)
                m_CurrentClipAsset->SetDuration(m_TimelineState.rangeEnd);
            m_TimelineState.currentTime = std::clamp(m_TimelineState.currentTime,
                                                     m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
            m_TimelineState.viewStart = m_TimelineState.rangeStart;
            m_TimelineState.viewEnd = m_TimelineState.rangeEnd;
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
        };
        m_RangeEndField->SetOnValueChanging(applyRangeEndFrame);
        m_RangeEndField->SetOnValueChanged(applyRangeEndFrame);
    }
    UIElement* markersEl = FindById("AnimationWindowTimelineMarkers");
    m_MarkersElement = markersEl ? dynamic_cast<TimelineMarkersElement*>(markersEl) : nullptr;
    if (m_MarkersElement)
    {
        m_MarkersElement->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
        WireMarkersElementCallbacks();
    }
    UIElement* timeLabelsEl = FindById("AnimationWindowTimelineTimeLabels");
    m_TimeLabelsElement = timeLabelsEl ? dynamic_cast<TimelineTimeLabelsElement*>(timeLabelsEl) : nullptr;
    UIElement* frameLabelsEl = FindById("AnimationWindowTimelineFrameLabels");
    m_FrameLabelsElement = frameLabelsEl ? dynamic_cast<TimelineFrameLabelsElement*>(frameLabelsEl) : nullptr;
    m_CurrentFrameLabel = dynamic_cast<Label*>(FindById("AnimationWindowCurrentFrame"));
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();

    auto seekToTime = [this](float t)
    {
        t = std::round(t * m_TimelineState.fps) / m_TimelineState.fps;
        m_TimelineState.currentTime = std::clamp(t, m_TimelineState.viewStart, m_TimelineState.viewEnd);
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
    };

    UIElement* dopeEl = FindById("AnimationWindowDopeSheetPlaceholder");
    m_DopeSheetView = dopeEl ? dynamic_cast<DopeSheetView*>(dopeEl) : nullptr;
    if (m_DopeSheetView)
    {
        m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_PropertiesTree) m_DopeSheetView->SetRowHeight(m_PropertiesTree->GetRowHeight());
        m_DopeSheetView->SetOnSeekToTime(seekToTime);
        m_DopeSheetView->SetOnSeekBegin([this]()
        {
            if (!m_IsSeekActive)
            {
                m_IsSeekActive = true;
                m_SeekBeforeTime = m_TimelineState.currentTime;
            }
        });
        m_DopeSheetView->SetOnSeekEnd([this]()
        {
            if (m_IsSeekActive)
            {
                m_IsSeekActive = false;
                const float afterTime = m_TimelineState.currentTime;
                if (m_UndoRedo && afterTime != m_SeekBeforeTime)
                {
                    const float before = m_SeekBeforeTime;
                    auto cmd = std::make_unique<AnimationSeekCommand>(before, afterTime,
                        [this](float t)
                        {
                            m_TimelineState.currentTime = std::clamp(t, m_TimelineState.viewStart, m_TimelineState.viewEnd);
                            UpdateTimelineRulerAndLabels();
                            UpdateCurrentFrameLabel();
                        });
                    m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
                }
            }
        });
        m_DopeSheetView->SetOnEditStarted([this]()
        {
            BeginClipUndoGesture("Move Animation Key");
        });
        m_DopeSheetView->SetOnEditFinished([this]()
        {
            CommitClipUndoGesture();
        });
        m_DopeSheetView->SetOnKeyframeSelected([this](int channel, int /*keyIndex*/, float /*time*/)
        {
            m_SelectedChannel = channel;
            if (m_SelectedComponent > 2u && m_CurrentClip &&
                static_cast<size_t>(channel) < m_CurrentClip->GetChannels().size() &&
                m_CurrentClip->GetChannels()[static_cast<size_t>(channel)].path != AnimPath::Rotation)
            {
                m_SelectedComponent = 0u;
            }
            RefreshChannelSelection();
        });
        m_DopeSheetView->SetOnSelectionChanged([this]()
        {
            if (!m_SuppressSelectionUndo && !m_DopeSheetView->IsBoxSelecting())
            {
                const SelectionSnapshot before = m_LastSelectionSnapshot;
                const SelectionSnapshot after  = CaptureSelectionSnapshot();
                m_LastSelectionSnapshot = after;
                PushSelectionUndo(before, after);
            }
            UpdateStatsBar();
            UpdateInterpolationButtonState();
        });
        // Initialize button state after view is set up.
        UpdateInterpolationButtonState();
        BuildDopeSheetTrackRows();
    }

    UIElement* curvesEl = FindById("AnimationWindowCurvesPlaceholder");
    m_CurvesGraphView = curvesEl ? dynamic_cast<CurvesGraphView*>(curvesEl) : nullptr;
    if (m_CurvesGraphView)
    {
        // Seed the session's grid, snap and fps from the saved defaults. From here on
        // the toolbar owns them: a later settings change restyles the view but never
        // resets what the user toggled in this window.
        {
            const AnimationWindowSettings& animSettings = AnimationWindowSettings::Get();
            m_ShowGrid = animSettings.ShowGrid;
            m_SnapTime = animSettings.SnapTime;
            m_SnapValue = animSettings.SnapValue;
            m_SnapTimeStep = animSettings.SnapTimeStep;
            m_SnapValueStep = animSettings.SnapValueStep;
            m_TimelineState.fps = animSettings.DefaultFps;
            m_CurvesGraphView->SetFps(m_TimelineState.fps);
            m_CurvesGraphView->SetKeyInsertionEnabled(m_KeytoolEnabled);
        }
        ApplyAppearanceSettings();
        m_SettingsChangedSubscription = AnimationWindowSettings::Get().Changed.Subscribe([this]()
        {
            ApplyAppearanceSettings();
            if (m_PropertiesTree)
                m_PropertiesTree->RefreshFromProvider();
        });
        m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        m_CurvesGraphView->SetShowGrid(m_ShowGrid);
        m_CurvesGraphView->SetSnapTime(m_SnapTime);
        m_CurvesGraphView->SetSnapValue(m_SnapValue);
        m_CurvesGraphView->SetSnapTimeStep(m_SnapTimeStep);
        m_CurvesGraphView->SetSnapValueStep(m_SnapValueStep);
        m_CurvesGraphView->SetOnSeekToTime(seekToTime);
        m_CurvesGraphView->SetOnEditStarted([this](CurvesGraphView::EditGesture gesture)
        {
            // Ensure the clip is editable before the drag starts so that
            // EnsureEditableClip() inside the per-frame edit callbacks is a
            // no-op. If it ran mid-drag it would replace the clip pointer,
            // triggering SetClip() which clears m_DragKeys while the drag
            // loop is still iterating — causing an OOB vector access.
            EnsureEditableClip();
            BeginClipUndoGesture(gesture == CurvesGraphView::EditGesture::Tangent
                                     ? "Edit Animation Tangent"
                                     : "Edit Animation Curve");
        });
        m_CurvesGraphView->SetOnEditFinished([this](CurvesGraphView::EditGesture gesture)
        {
            CommitClipUndoGesture();
            if (gesture == CurvesGraphView::EditGesture::Retime ||
                gesture == CurvesGraphView::EditGesture::Lattice)
                UpdateStatsBar();
        });
        m_CurvesGraphView->SetOnKeyframeSelected([this](int channel, int component, int /*keyIndex*/, float /*time*/)
        {
            const SelectionSnapshot before = m_LastSelectionSnapshot;
            m_SelectedChannel = channel;
            m_SelectedComponent = static_cast<uint32>(std::max(component, 0));
            // Don't call RefreshChannelSelection here — it would reset ShowAllComponents and
            // collapse multi-curve display to a single curve whenever the user clicks a key.
            UpdateStatsBar();
            const SelectionSnapshot after = CaptureSelectionSnapshot();
            m_LastSelectionSnapshot = after;
            PushSelectionUndo(before, after);
        });
        m_CurvesGraphView->SetOnSelectionChanged([this]()
        {
            // Skip undo push during live box drag; push once when the box is committed on mouse-up.
            if (!m_SuppressSelectionUndo && !m_CurvesGraphView->IsBoxSelecting())
            {
                const SelectionSnapshot before = m_LastSelectionSnapshot;
                const SelectionSnapshot after  = CaptureSelectionSnapshot();
                m_LastSelectionSnapshot = after;
                PushSelectionUndo(before, after);
            }
            UpdateStatsBar();
            UpdateInterpolationButtonState();
        });
        // Initialize button state after view is set up.
        UpdateInterpolationButtonState();
        auto isChannelLocked = [this](int channel) -> bool
        {
            for (const auto& [treeId, binding] : m_ChannelBindings)
            {
                if (static_cast<int>(binding.first) == channel && m_LockedTreeIds.count(treeId))
                    return true;
            }
            return false;
        };

        m_CurvesGraphView->SetOnKeyframeEdited([this, isChannelLocked](int channel, int component, float currentTime, float newTime, float newValue)
        {
            if (isChannelLocked(channel)) return;
            if (!EnsureEditableClip() || !m_CurrentClipAsset)
                return;
            const size_t channelIndex = static_cast<size_t>(channel);
            if (!m_CurrentClipAsset->SetKeyframeTime(channelIndex, currentTime, newTime))
                return;
            (void)m_CurrentClipAsset->SetKeyframeComponentValue(channelIndex, newTime, static_cast<uint32>(std::max(component, 0)), newValue);
            RecomputeAutoTangents(channelIndex);
            RefreshEditedClipState();
            UpdateStatsBar();
        });
        m_CurvesGraphView->SetOnTangentEdited([this, isChannelLocked](int channel,
                                                     int component,
                                                     float keyTime,
                                                     bool incoming,
                                                     float tangent,
                                                     float weight)
        {
            if (isChannelLocked(channel)) return;
            if (!EnsureEditableClip() || !m_CurrentClipAsset)
                return;

            const size_t chIdx = static_cast<size_t>(channel);
            const uint32 comp = static_cast<uint32>(std::max(component, 0));
            if (!m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, comp, incoming, tangent, weight))
                return;

            // Mark this handle as Custom so RecomputeAutoTangents won't overwrite the user edit.
            m_CurrentClipAsset->SetKeyframeTangentType(chIdx, keyTime, comp, incoming, AnimTangentType::Custom);

            RefreshEditedClipState();
        });

        m_CurvesGraphView->SetOnKeyframeContextMenu([this](int channel, float keyTime, float screenX, float screenY)
        {
            if (!m_Window) return;
            m_ContextMenuKeyChannel = channel;
            m_ContextMenuKeyTime    = keyTime;

            if (!m_KeyframeContextMenu)
            {
                m_KeyframeContextMenu = CreateContextMenu();
                if (!m_KeyframeContextMenu) return;

                enum : uint32_t {
                    kCmdInterpLinear = 1, kCmdInterpStep, kCmdInterpSpline,
                    kCmdEaseIn, kCmdEaseEase, kCmdEaseOut,
                    kCmdTangentFlat,
                    kCmdLineWidth1, kCmdLineWidth2, kCmdLineWidth3,
                };
                m_KeyframeContextMenu->SetCommandHandler([this](uint32_t cmd)
                {
                    enum : uint32_t {
                        kCmdInterpLinear = 1, kCmdInterpStep, kCmdInterpSpline,
                        kCmdEaseIn, kCmdEaseEase, kCmdEaseOut,
                        kCmdTangentFlat,
                        kCmdLineWidth1, kCmdLineWidth2, kCmdLineWidth3,
                        kCmdTangentTypeAuto, kCmdTangentTypePlateau, kCmdTangentTypeClamped,
                        kCmdTangentTypeFixed, kCmdTangentTypeLinear,
                        kCmdBreakTangents, kCmdUnifyTangents,
                    };
                    if (cmd == kCmdBreakTangents) { BreakTangents(); return; }
                    if (cmd == kCmdUnifyTangents) { UnifyTangents(); return; }
                    if (cmd == kCmdLineWidth1 || cmd == kCmdLineWidth2 || cmd == kCmdLineWidth3)
                    {
                        const float w = (cmd == kCmdLineWidth1) ? 1.0f : (cmd == kCmdLineWidth2) ? 2.0f : 3.0f;
                        if (m_CurvesGraphView) m_CurvesGraphView->SetCurveLineWidth(w);
                        return;
                    }
                    if (cmd >= kCmdTangentTypeAuto && cmd <= kCmdTangentTypeLinear)
                    {
                        const AnimTangentType type =
                            (cmd == kCmdTangentTypePlateau) ? AnimTangentType::Plateau :
                            (cmd == kCmdTangentTypeClamped) ? AnimTangentType::Clamped :
                            (cmd == kCmdTangentTypeFixed)   ? AnimTangentType::Fixed   :
                            (cmd == kCmdTangentTypeLinear)  ? AnimTangentType::Linear  :
                                                              AnimTangentType::Auto;
                        SetTangentTypeOnSelectedKeys(type);
                        return;
                    }
                    if (!EnsureEditableClip() || !m_CurrentClipAsset) return;

                    // Build the set of (channel, time) pairs to operate on.
                    // Use selected keyframes when available; otherwise use the right-clicked key.
                    using KeyTarget = std::pair<size_t, float>;
                    std::vector<KeyTarget> targets;
                    if (m_CurvesGraphView)
                    {
                        for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
                            if (sk.Channel >= 0) targets.push_back({static_cast<size_t>(sk.Channel), sk.KeyTime});
                    }
                    if (targets.empty() && m_ContextMenuKeyChannel >= 0)
                        targets.push_back({static_cast<size_t>(m_ContextMenuKeyChannel), m_ContextMenuKeyTime});

                    if (targets.empty()) return;

                    if (cmd == kCmdInterpLinear || cmd == kCmdInterpStep || cmd == kCmdInterpSpline)
                    {
                        const AnimInterp interp = (cmd == kCmdInterpLinear) ? AnimInterp::Linear
                                                : (cmd == kCmdInterpStep)   ? AnimInterp::Step
                                                                            : AnimInterp::CubicSpline;
                        const char* name = (cmd == kCmdInterpLinear) ? "Set Linear"
                                         : (cmd == kCmdInterpStep)   ? "Set Step" : "Set Spline";
                        std::unordered_set<size_t> channels;
                        for (const auto& [ch, t] : targets) channels.insert(ch);
                        (void)ExecuteClipEditWithUndo(name, [this, channels, interp]() {
                            bool changed = false;
                            for (size_t ch : channels)
                                changed |= m_CurrentClipAsset->SetChannelInterpolation(ch, interp);
                            return changed;
                        });
                    }
                    else
                    {
                        const bool easeIn  = (cmd == kCmdEaseIn  || cmd == kCmdEaseEase || cmd == kCmdTangentFlat);
                        const bool easeOut = (cmd == kCmdEaseOut || cmd == kCmdEaseEase || cmd == kCmdTangentFlat);
                        (void)ExecuteClipEditWithUndo("Set Tangent", [this, targets, easeIn, easeOut]() -> bool {
                            bool changed = false;
                            for (const auto& [chIdx, t] : targets)
                            {
                                m_CurrentClipAsset->SetChannelInterpolation(chIdx, AnimInterp::CubicSpline);
                                const AnimChannel& ch = m_CurrentClipAsset->GetChannels()[chIdx];
                                const uint32 compCount = (ch.path == AnimPath::Rotation) ? 4u : 3u;
                                for (uint32 c = 0; c < compCount; ++c)
                                {
                                    if (easeIn)  changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, true,  0.0f, 1.0f/3.0f);
                                    if (easeOut) changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, false, 0.0f, 1.0f/3.0f);
                                }
                            }
                            return changed;
                        });
                    }
                });
            }

            // Rebuild items each time to reflect current state (line width checkmarks, keyframe availability).
            {
                enum : uint32_t {
                    kCmdInterpLinear = 1, kCmdInterpStep, kCmdInterpSpline,
                    kCmdEaseIn, kCmdEaseEase, kCmdEaseOut,
                    kCmdTangentFlat,
                    kCmdLineWidth1, kCmdLineWidth2, kCmdLineWidth3,
                    kCmdTangentTypeAuto, kCmdTangentTypePlateau, kCmdTangentTypeClamped,
                    kCmdTangentTypeFixed, kCmdTangentTypeLinear,
                    kCmdBreakTangents, kCmdUnifyTangents,
                };
                m_KeyframeContextMenu->Clear();
                ContextMenuBuilder builder;
                const float curW = m_CurvesGraphView ? m_CurvesGraphView->GetCurveLineWidth() : 2.0f;
                auto wFlag = [&](float w) {
                    return MenuItemFlag_Radio |
                           (std::abs(curW - w) < 0.1f ? MenuItemFlag_Checked : MenuItemFlag_None);
                };
                builder.AddItem("Line Width", 0, MenuItemFlag_None, 0, EditorIcons::kSplineCurve);
                builder.AddItem("Line Width/1 px", kCmdLineWidth1, wFlag(1.0f), 0);
                builder.AddItem("Line Width/2 px", kCmdLineWidth2, wFlag(2.0f), 0);
                builder.AddItem("Line Width/3 px", kCmdLineWidth3, wFlag(3.0f), 0);
                if (channel >= 0)
                {
                    builder.AddItem("Interpolation", 0, MenuItemFlag_None, 0, EditorIcons::kSplineCurve);
                    builder.AddItem("Ease", 0, MenuItemFlag_None, 0, EditorIcons::kEase);
                    builder.AddItem("Tangent", 0, MenuItemFlag_None, 1, EditorIcons::kBreakTangents);
                    builder.AddItem("Tangent Type", 0, MenuItemFlag_None, 0, EditorIcons::kUnifyTangents);
                    builder.AddItem("Interpolation/Linear",  kCmdInterpLinear,       MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
                    builder.AddItem("Interpolation/Step",    kCmdInterpStep,         MenuItemFlag_None, 0, EditorIcons::kSteppedCurve);
                    builder.AddItem("Interpolation/Spline",  kCmdInterpSpline,       MenuItemFlag_None, 0, EditorIcons::kSplineCurve);
                    builder.AddItem("Ease/Ease In",          kCmdEaseIn,             MenuItemFlag_None, 0, EditorIcons::kEase);
                    builder.AddItem("Ease/Ease",             kCmdEaseEase,           MenuItemFlag_None, 0, EditorIcons::kEase);
                    builder.AddItem("Ease/Ease Out",         kCmdEaseOut,            MenuItemFlag_None, 0, EditorIcons::kEase);
                    builder.AddItem("Tangent/Flat",          kCmdTangentFlat,        MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
                    builder.AddItem("Tangent Type/Auto",     kCmdTangentTypeAuto,    MenuItemFlag_None, 0, EditorIcons::kDrawCurve);
                    builder.AddItem("Tangent Type/Plateau",  kCmdTangentTypePlateau, MenuItemFlag_None, 0, EditorIcons::kSplineCurve);
                    builder.AddItem("Tangent Type/Clamped",  kCmdTangentTypeClamped, MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
                    builder.AddItem("Tangent Type/Fixed",    kCmdTangentTypeFixed,   MenuItemFlag_None, 0, EditorIcons::kSteppedCurve);
                    builder.AddItem("Tangent Type/Linear",   kCmdTangentTypeLinear,  MenuItemFlag_None, 0, EditorIcons::kLinearCurve);
                    builder.AddItem("Tangent/Break",         kCmdBreakTangents,      MenuItemFlag_None, 0, EditorIcons::kBreakTangents);
                    builder.AddItem("Tangent/Unify",         kCmdUnifyTangents,      MenuItemFlag_None, 0, EditorIcons::kUnifyTangents);
                }
                builder.Build(m_KeyframeContextMenu.get());
            }
            ShowContextMenuKeepingFocus(m_KeyframeContextMenu.get(),
                                        static_cast<int>(screenX), static_cast<int>(screenY));
        });

        m_CurvesGraphView->SetIsChannelLocked(isChannelLocked);
        m_CurvesGraphView->SetOnRetimeApplied([this, isChannelLocked](const std::vector<CurvesGraphView::RetimeChange>& batch)
        {
            if (!m_CurrentClipAsset || batch.empty()) return;

            // Batch is pre-sorted by CurvesGraphView so SetKeyframeTime walks
            // in a direction that doesn't trample siblings mid-loop.
            std::unordered_set<size_t> touched;
            touched.reserve(batch.size());
            for (const auto& change : batch)
            {
                if (isChannelLocked(change.channel)) continue;
                const size_t ch = static_cast<size_t>(change.channel);
                if (m_CurrentClipAsset->SetKeyframeTime(ch, change.originalTime, change.newTime))
                    touched.insert(ch);
            }
            // Recompute tangents once per touched channel — the previous per-key
            // path did this O(N²) in the number of keys past the pivot.
            for (size_t ch : touched)
                RecomputeAutoTangents(ch);
            if (!touched.empty())
                RefreshEditedClipState();
        });
        m_CurvesGraphView->SetOnLatticeEdited([this](int channel, float keyTime, float newValue)
        {
            if (!m_CurrentClipAsset) return;
            (void)m_CurrentClipAsset->SetKeyframeComponentValue(
                static_cast<size_t>(channel), keyTime, m_SelectedComponent, newValue);
            RecomputeAutoTangents(static_cast<size_t>(channel));
            RefreshEditedClipState();
        });
        m_CurvesGraphView->SetOnInsertKeyAt([this](int channel, float time)
        {
            if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
            const size_t ch = static_cast<size_t>(channel);
            (void)ExecuteClipEditWithUndo("Insert Animation Key",
                [this, ch, time]()
                {
                    const bool added = m_CurrentClipAsset->AddKeyframe(ch, time);
                    if (added) RecomputeAutoTangents(ch);
                    return added;
                });
        });

        m_CurvesGraphView->SetOnDrawCurveCommitted(
            [this](int channel, uint32 component,
                   const std::vector<CurvesGraphView::DrawCurveSample>& samples)
        {
            if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
            if (channel < 0 || samples.size() < 2) return;
            const size_t ch = static_cast<size_t>(channel);
            const float t0 = samples.front().time;
            const float t1 = samples.back().time;
            std::vector<CurvesGraphView::DrawCurveSample> samplesCopy = samples;
            (void)ExecuteClipEditWithUndo("Draw Animation Curve",
                [this, ch, component, t0, t1, samplesCopy = std::move(samplesCopy)]() -> bool
                {
                    // Replace any keys inside the stroke's time range with
                    // one key per sample. Walk the existing keys first and
                    // remove ones in (t0, t1) (open interval — we let the
                    // sample insertion overwrite the boundary keys).
                    bool changed = false;
                    if (ch < m_CurrentClipAsset->GetChannels().size())
                    {
                        std::vector<float> doomed;
                        for (const auto& kf : m_CurrentClipAsset->GetChannels()[ch].keys)
                            if (kf.time > t0 && kf.time < t1)
                                doomed.push_back(kf.time);
                        for (float t : doomed)
                            if (m_CurrentClipAsset->RemoveKeyframe(ch, t))
                                changed = true;
                    }
                    for (const auto& s : samplesCopy)
                    {
                        if (m_CurrentClipAsset->AddKeyframe(ch, s.time))
                            changed = true;
                        if (m_CurrentClipAsset->SetKeyframeComponentValue(ch, s.time, component, s.value))
                            changed = true;
                    }
                    if (changed) RecomputeAutoTangents(ch);
                    return changed;
                });
        });
    }

    UIElement* timeCompEl = FindById("AnimationWindowTimeCompositePlaceholder");
    m_TimeCompositeView = timeCompEl ? dynamic_cast<TimeCompositeView*>(timeCompEl) : nullptr;
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (!m_CompositeModel)
        {
            m_CompositeModel = std::make_unique<TimeCompositeModel>();
        }
        RefreshSequencingModels();
        m_TimeCompositeView->SetModel(m_CompositeModel.get());
        m_TimeCompositeView->SetOnClipSelected([this](size_t trackIdx, size_t clipIdx)
                                              {
                                                  SelectCompositeClip(trackIdx, clipIdx);
                                              });
        m_TimeCompositeView->SetOnClipDuplicated([this](size_t trackIdx, size_t clipIdx, size_t& outNewClipIndex)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return false;
            auto& track = m_CompositeModel->tracks[trackIdx];
            if (clipIdx >= track.clips.size())
                return false;
            CompositeClip duplicate = track.clips[clipIdx];
            duplicate.name += " Copy";
            const size_t insertIdx = clipIdx + 1;
            track.clips.insert(track.clips.begin() + static_cast<std::ptrdiff_t>(insertIdx), std::move(duplicate));
            outNewClipIndex = insertIdx;
            SelectCompositeClip(trackIdx, insertIdx);
            if (m_TimeCompositeView)
            {
                m_TimeCompositeView->SetSelectedClip(trackIdx, insertIdx);
                m_TimeCompositeView->MarkDirty(VisualDirty);
            }
            m_TimelineDirty = true;
            UpdateTitle();
            if (m_UnsavedIndicator)
                m_UnsavedIndicator->RemoveClass("hidden");
            return true;
        });
        m_TimeCompositeView->SetOnClipDragStarted([this](size_t, size_t)
        {
            BeginPanelUndoGesture("Edit Composite Clip");
        });
        m_TimeCompositeView->SetOnClipDragEnded([this](size_t, size_t)
        {
            CommitPanelUndoGesture();
        });
        m_TimeCompositeView->SetOnClipOffsetChanged([this](size_t trackIdx, size_t clipIdx, float newOffset)
                                                    {
                                                        if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                                                            return;
                                                        auto applyLinkedOffset = [this](size_t sourceTrackIdx, size_t sourceClipIdx, float offset)
                                                        {
                                                            if (!m_CompositeModel || sourceTrackIdx >= m_CompositeModel->tracks.size())
                                                                return;
                                                            const CompositeTrack& sourceTrack = m_CompositeModel->tracks[sourceTrackIdx];
                                                            if (sourceClipIdx >= sourceTrack.clips.size())
                                                                return;
                                                            const CompositeClip& sourceClip = sourceTrack.clips[sourceClipIdx];
                                                            if (sourceClip.linkedMediaGroupGuid.IsNull())
                                                                return;
                                                            for (size_t tr = 0; tr < m_CompositeModel->tracks.size(); ++tr)
                                                            {
                                                                auto& track = m_CompositeModel->tracks[tr];
                                                                if (track.type != CompositeTrackType::Audio && track.type != CompositeTrackType::Video)
                                                                    continue;
                                                                if (track.type == CompositeTrackType::Audio && !track.syncLinkedVideo)
                                                                    continue;
                                                                for (size_t ci = 0; ci < track.clips.size(); ++ci)
                                                                {
                                                                    if (tr == sourceTrackIdx && ci == sourceClipIdx)
                                                                        continue;
                                                                    auto& candidate = track.clips[ci];
                                                                    if (candidate.linkedMediaGroupGuid == sourceClip.linkedMediaGroupGuid)
                                                                    {
                                                                        const CompositeTrack* audioTrack = nullptr;
                                                                        if (sourceTrack.type == CompositeTrackType::Audio)
                                                                            audioTrack = &sourceTrack;
                                                                        else if (track.type == CompositeTrackType::Audio)
                                                                            audioTrack = &track;
                                                                        if (!audioTrack || !audioTrack->syncLinkedVideo)
                                                                            continue;
                                                                        float targetOffset = offset;
                                                                        if (audioTrack->mediaSyncMode == CompositeMediaSyncMode::KeepOffset)
                                                                        {
                                                                            if (sourceTrack.type == CompositeTrackType::Audio)
                                                                                targetOffset = offset - sourceClip.linkedMediaOffsetSeconds;
                                                                            else if (track.type == CompositeTrackType::Audio)
                                                                                targetOffset = offset + candidate.linkedMediaOffsetSeconds;
                                                                        }
                                                                        candidate.offsetOnTimeline = std::max(0.0f, targetOffset);
                                                                    }
                                                                }
                                                            }
                                                        };
                                                        if (m_PendingPanelUndoActive)
                                                        {
                                                            auto& track = m_CompositeModel->tracks[trackIdx];
                                                            if (clipIdx >= track.clips.size())
                                                                return;
                                                            track.clips[clipIdx].offsetOnTimeline = std::max(0.0f, newOffset);
                                                            applyLinkedOffset(trackIdx, clipIdx, newOffset);
                                                            m_SelectedCompositeTrack = trackIdx;
                                                            m_SelectedCompositeClip = clipIdx;
                                                            if (m_TimeCompositeView)
                                                            {
                                                                m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                                                                m_TimeCompositeView->MarkDirty(VisualDirty);
                                                            }
                                                            RefreshTimelineInspector();
                                                            m_TimelineDirty = true;
                                                            UpdateTitle();
                                                            if (m_UnsavedIndicator)
                                                                m_UnsavedIndicator->RemoveClass("hidden");
                                                            return;
                                                        }
                                                        (void)ExecutePanelEditWithUndo("Move Composite Clip",
                                                                                      [this, trackIdx, clipIdx, newOffset, applyLinkedOffset]()
                                                                                      {
                                                        auto& track = m_CompositeModel->tracks[trackIdx];
                                                        if (clipIdx >= track.clips.size())
                                                                                              return false;
                                                        track.clips[clipIdx].offsetOnTimeline = std::max(0.0f, newOffset);
                                                        applyLinkedOffset(trackIdx, clipIdx, newOffset);
                                                                                          SelectCompositeClip(trackIdx, clipIdx);
                                                        if (m_TimeCompositeView)
                                                                                          {
                                                                                              if (m_TimeCompositeView->GetSelectedClips().size() <= 1)
                                                                                                  m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                                                            m_TimeCompositeView->MarkDirty(VisualDirty);
                                                                                          }
                                                                                          return true;
                                                                                      },
                                                                                      true);
                                                    });
        m_TimeCompositeView->SetOnClipEndChanged([this](size_t trackIdx, size_t clipIdx, float newOutTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            auto applyLinkedOutTime = [this](size_t sourceTrackIdx, size_t sourceClipIdx, float outTime)
            {
                if (!m_CompositeModel || sourceTrackIdx >= m_CompositeModel->tracks.size())
                    return;
                const CompositeTrack& sourceTrack = m_CompositeModel->tracks[sourceTrackIdx];
                if (sourceClipIdx >= sourceTrack.clips.size())
                    return;
                const CompositeClip& sourceClip = sourceTrack.clips[sourceClipIdx];
                if (sourceClip.linkedMediaGroupGuid.IsNull())
                    return;
                for (size_t tr = 0; tr < m_CompositeModel->tracks.size(); ++tr)
                {
                    auto& track = m_CompositeModel->tracks[tr];
                    if (track.type != CompositeTrackType::Audio && track.type != CompositeTrackType::Video)
                        continue;
                    if (track.type == CompositeTrackType::Audio && !track.syncLinkedVideo)
                        continue;
                    for (size_t ci = 0; ci < track.clips.size(); ++ci)
                    {
                        if (tr == sourceTrackIdx && ci == sourceClipIdx)
                            continue;
                        auto& candidate = track.clips[ci];
                        if (candidate.linkedMediaGroupGuid == sourceClip.linkedMediaGroupGuid)
                        {
                            const CompositeTrack* audioTrack = nullptr;
                            if (sourceTrack.type == CompositeTrackType::Audio)
                                audioTrack = &sourceTrack;
                            else if (track.type == CompositeTrackType::Audio)
                                audioTrack = &track;
                            if (!audioTrack || !audioTrack->syncLinkedVideo)
                                continue;
                            candidate.outTime = std::max(candidate.inTime + 1.0f / 30.0f, outTime);
                        }
                    }
                }
            };
            if (m_PendingPanelUndoActive)
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return;
                auto& clip = track.clips[clipIdx];
                clip.outTime = std::max(clip.inTime + 1.0f / 30.0f, newOutTime);
                applyLinkedOutTime(trackIdx, clipIdx, clip.outTime);
                m_SelectedCompositeTrack = trackIdx;
                m_SelectedCompositeClip = clipIdx;
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshTimelineInspector();
                m_TimelineDirty = true;
                UpdateTitle();
                if (m_UnsavedIndicator)
                    m_UnsavedIndicator->RemoveClass("hidden");
                return;
            }
            (void)ExecutePanelEditWithUndo("Resize Composite Clip",
                [this, trackIdx, clipIdx, newOutTime, applyLinkedOutTime]()
                {
                    auto& track = m_CompositeModel->tracks[trackIdx];
                    if (clipIdx >= track.clips.size()) return false;
                    auto& clip = track.clips[clipIdx];
                    clip.outTime = std::max(clip.inTime + 1.0f / 30.0f, newOutTime);
                    applyLinkedOutTime(trackIdx, clipIdx, clip.outTime);
                    if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_TimeCompositeView->SetOnClipStartChanged([this](size_t trackIdx, size_t clipIdx, float newOffset, float newInTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            auto applyLinkedStart = [this](size_t sourceTrackIdx, size_t sourceClipIdx, float offset, float inTime)
            {
                if (!m_CompositeModel || sourceTrackIdx >= m_CompositeModel->tracks.size())
                    return;
                const CompositeTrack& sourceTrack = m_CompositeModel->tracks[sourceTrackIdx];
                if (sourceClipIdx >= sourceTrack.clips.size())
                    return;
                const CompositeClip& sourceClip = sourceTrack.clips[sourceClipIdx];
                if (sourceClip.linkedMediaGroupGuid.IsNull())
                    return;
                for (size_t tr = 0; tr < m_CompositeModel->tracks.size(); ++tr)
                {
                    auto& track = m_CompositeModel->tracks[tr];
                    if (track.type != CompositeTrackType::Audio && track.type != CompositeTrackType::Video)
                        continue;
                    if (track.type == CompositeTrackType::Audio && !track.syncLinkedVideo)
                        continue;
                    for (size_t ci = 0; ci < track.clips.size(); ++ci)
                    {
                        if (tr == sourceTrackIdx && ci == sourceClipIdx)
                            continue;
                        auto& candidate = track.clips[ci];
                        if (candidate.linkedMediaGroupGuid == sourceClip.linkedMediaGroupGuid)
                        {
                            const CompositeTrack* audioTrack = nullptr;
                            if (sourceTrack.type == CompositeTrackType::Audio)
                                audioTrack = &sourceTrack;
                            else if (track.type == CompositeTrackType::Audio)
                                audioTrack = &track;
                            if (!audioTrack || !audioTrack->syncLinkedVideo)
                                continue;
                            float targetOffset = offset;
                            if (audioTrack->mediaSyncMode == CompositeMediaSyncMode::KeepOffset)
                            {
                                if (sourceTrack.type == CompositeTrackType::Audio)
                                    targetOffset = offset - sourceClip.linkedMediaOffsetSeconds;
                                else if (track.type == CompositeTrackType::Audio)
                                    targetOffset = offset + candidate.linkedMediaOffsetSeconds;
                            }
                            candidate.offsetOnTimeline = std::max(0.0f, targetOffset);
                            candidate.inTime = std::max(0.0f, inTime);
                        }
                    }
                }
            };
            if (m_PendingPanelUndoActive)
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return;
                auto& clip = track.clips[clipIdx];
                clip.offsetOnTimeline = std::max(0.0f, newOffset);
                clip.inTime = std::max(0.0f, newInTime);
                applyLinkedStart(trackIdx, clipIdx, clip.offsetOnTimeline, clip.inTime);
                m_SelectedCompositeTrack = trackIdx;
                m_SelectedCompositeClip = clipIdx;
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshTimelineInspector();
                m_TimelineDirty = true;
                UpdateTitle();
                if (m_UnsavedIndicator)
                    m_UnsavedIndicator->RemoveClass("hidden");
                return;
            }
            (void)ExecutePanelEditWithUndo("Resize Composite Clip Start",
                [this, trackIdx, clipIdx, newOffset, newInTime, applyLinkedStart]()
                {
                    auto& track = m_CompositeModel->tracks[trackIdx];
                    if (clipIdx >= track.clips.size()) return false;
                    auto& clip = track.clips[clipIdx];
                    clip.offsetOnTimeline = newOffset;
                    clip.inTime = newInTime;
                    applyLinkedStart(trackIdx, clipIdx, clip.offsetOnTimeline, clip.inTime);
                    if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_TimeCompositeView->SetOnClipFadeInChanged([this](size_t trackIdx, size_t clipIdx, float newFadeIn)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            if (m_PendingPanelUndoActive)
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return;
                auto& clip = track.clips[clipIdx];
                const float duration = std::max(0.0f, clip.outTime - clip.inTime);
                const float maxFade = std::max(0.0f, duration - clip.fadeOutDuration);
                clip.fadeInDuration = std::clamp(newFadeIn, 0.0f, maxFade);
                m_SelectedCompositeTrack = trackIdx;
                m_SelectedCompositeClip = clipIdx;
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshTimelineInspector();
                m_TimelineDirty = true;
                UpdateTitle();
                if (m_UnsavedIndicator)
                    m_UnsavedIndicator->RemoveClass("hidden");
                return;
            }
            (void)ExecutePanelEditWithUndo("Set Composite Clip Fade In",
                [this, trackIdx, clipIdx, newFadeIn]()
                {
                    auto& track = m_CompositeModel->tracks[trackIdx];
                    if (clipIdx >= track.clips.size()) return false;
                    auto& clip = track.clips[clipIdx];
                    const float duration = std::max(0.0f, clip.outTime - clip.inTime);
                    const float maxFade  = std::max(0.0f, duration - clip.fadeOutDuration);
                    clip.fadeInDuration  = std::clamp(newFadeIn, 0.0f, maxFade);
                    if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_TimeCompositeView->SetOnClipFadeOutChanged([this](size_t trackIdx, size_t clipIdx, float newFadeOut)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            if (m_PendingPanelUndoActive)
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return;
                auto& clip = track.clips[clipIdx];
                const float duration = std::max(0.0f, clip.outTime - clip.inTime);
                const float maxFade = std::max(0.0f, duration - clip.fadeInDuration);
                clip.fadeOutDuration = std::clamp(newFadeOut, 0.0f, maxFade);
                m_SelectedCompositeTrack = trackIdx;
                m_SelectedCompositeClip = clipIdx;
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetSelectedClip(trackIdx, clipIdx);
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshTimelineInspector();
                m_TimelineDirty = true;
                UpdateTitle();
                if (m_UnsavedIndicator)
                    m_UnsavedIndicator->RemoveClass("hidden");
                return;
            }
            (void)ExecutePanelEditWithUndo("Set Composite Clip Fade Out",
                [this, trackIdx, clipIdx, newFadeOut]()
                {
                    auto& track = m_CompositeModel->tracks[trackIdx];
                    if (clipIdx >= track.clips.size()) return false;
                    auto& clip = track.clips[clipIdx];
                    const float duration = std::max(0.0f, clip.outTime - clip.inTime);
                    const float maxFade  = std::max(0.0f, duration - clip.fadeInDuration);
                    clip.fadeOutDuration = std::clamp(newFadeOut, 0.0f, maxFade);
                    if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_TimeCompositeView->SetOnSeekToTime(seekToTime);
        m_TimeCompositeView->SetOnTrackContextMenu([this](size_t trackIdx, float time, float screenX, float screenY)
        {
            if (!m_Window || !m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size())
                return;

            m_ContextMenuTimelineTrackIdx = trackIdx;
            m_ContextMenuTimelineTime = std::max(0.0f, time);
            m_SelectedCompositeTrack = trackIdx;
            m_SelectedCompositeClip = static_cast<size_t>(-1);
            RefreshSequencerSidebar();
            RefreshTimelineInspector();

            if (!m_TimelineTrackContextMenu)
            {
                m_TimelineTrackContextMenu = CreateContextMenu();
                if (!m_TimelineTrackContextMenu)
                    return;

                m_TimelineTrackContextMenu->SetCommandHandler([this](uint32_t cmd)
                {
                    if (cmd == kCmdTimelineInsertKey)
                    {
                        (void)InsertTimelineTrackKeyAt(m_ContextMenuTimelineTrackIdx, m_ContextMenuTimelineTime);
                    }
                    else if (cmd == kCmdTimelineInsertMarker)
                    {
                        (void)InsertTimelineTrackMarkerAt(m_ContextMenuTimelineTrackIdx, m_ContextMenuTimelineTime);
                    }
                    else if (cmd == kCmdTimelineOpenCurveEditor)
                    {
                        if (m_CompositeModel && m_ContextMenuTimelineTrackIdx < m_CompositeModel->tracks.size())
                        {
                            m_SelectedCompositeTrack = m_ContextMenuTimelineTrackIdx;
                            m_SelectedCompositeClip = static_cast<size_t>(-1);
                            m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                            m_SelectedTimelineKey = static_cast<size_t>(-1);
                            RefreshSequencerSidebar();
                            RefreshTimelineInspector();
                        }
                        if (m_PanelKind == PanelKind::Animation)
                            SetActiveView(ActiveView::Curves);
                        else if (m_OnOpenAnimationPanel)
                            m_OnOpenAnimationPanel();
                    }
                    else if (cmd == kCmdTimelineRemoveKey)
                    {
                        DeleteSelectedTimelineKeys();
                    }
                    else if (cmd == kCmdTimelineRecomputeLinkedOffsets)
                    {
                        (void)ExecutePanelEditWithUndo("Recompute Linked Media Offsets", [this]()
                        {
                            if (!m_CompositeModel || m_ContextMenuTimelineTrackIdx >= m_CompositeModel->tracks.size())
                                return false;
                            auto& tracks = m_CompositeModel->tracks;
                            auto& audioTrack = tracks[m_ContextMenuTimelineTrackIdx];
                            if (audioTrack.type != CompositeTrackType::Audio)
                                return false;
                            for (auto& audioClip : audioTrack.clips)
                            {
                                if (audioClip.linkedMediaGroupGuid.IsNull())
                                    continue;
                                bool foundVideo = false;
                                for (auto& candidateTrack : tracks)
                                {
                                    if (candidateTrack.type != CompositeTrackType::Video)
                                        continue;
                                    for (const auto& videoClip : candidateTrack.clips)
                                    {
                                        if (videoClip.linkedMediaGroupGuid == audioClip.linkedMediaGroupGuid)
                                        {
                                            audioClip.linkedMediaOffsetSeconds = audioClip.offsetOnTimeline - videoClip.offsetOnTimeline;
                                            foundVideo = true;
                                            break;
                                        }
                                    }
                                    if (foundVideo)
                                        break;
                                }
                                if (!foundVideo)
                                    audioClip.linkedMediaOffsetSeconds = 0.0f;
                            }
                            if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                            RefreshSequencerSidebar();
                            RefreshTimelineInspector();
                            return true;
                        }, false);
                    }
                });
            }

            const auto& track = m_CompositeModel->tracks[trackIdx];
            const bool canInsertKey = CompositeTrackTypeUsesValueKeys(track.type) ||
                                      CompositeTrackTypeUsesMethodKeys(track.type) ||
                                      CompositeTrackTypeUsesAudioKeys(track.type) ||
                                      CompositeTrackTypeUsesAnimationKeys(track.type);
            const bool canOpenCurveEditor = canInsertKey;
            const bool hasTrackUtilities = canOpenCurveEditor || track.type == CompositeTrackType::Audio;
            const bool canRemoveKey = m_TimeCompositeView &&
                                      (!m_TimeCompositeView->GetSelectedTrackKeys().empty() ||
                                       (m_SelectedTimelineKeyTrack != static_cast<size_t>(-1) &&
                                        m_SelectedTimelineKey != static_cast<size_t>(-1)));

            m_TimelineTrackContextMenu->Clear();
            m_TimelineTrackContextMenu->AddItem(0, "Insert Key", kCmdTimelineInsertKey,
                                                canInsertKey ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_TimelineTrackContextMenu->SetItemIcon(kCmdTimelineInsertKey, EditorIcons::kPlus);
            m_TimelineTrackContextMenu->AddItem(0, "Remove Key", kCmdTimelineRemoveKey,
                                                canRemoveKey ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_TimelineTrackContextMenu->SetItemIcon(kCmdTimelineRemoveKey, EditorIcons::kTrash);
            m_TimelineTrackContextMenu->AddItem(0, "Insert Marker", kCmdTimelineInsertMarker);
            m_TimelineTrackContextMenu->SetItemIcon(kCmdTimelineInsertMarker, EditorIcons::kPlus);
            if (hasTrackUtilities)
                m_TimelineTrackContextMenu->AddSeparator(0);
            if (canOpenCurveEditor)
            {
                m_TimelineTrackContextMenu->AddItem(0, "Open in Curve Editor", kCmdTimelineOpenCurveEditor);
                m_TimelineTrackContextMenu->SetItemIcon(kCmdTimelineOpenCurveEditor, EditorIcons::kFolderOpen);
            }
            if (track.type == CompositeTrackType::Audio)
            {
                m_TimelineTrackContextMenu->AddItem(0, "Recompute Offsets Now", kCmdTimelineRecomputeLinkedOffsets);
                m_TimelineTrackContextMenu->SetItemIcon(kCmdTimelineRecomputeLinkedOffsets, EditorIcons::kReset);
            }
            ShowContextMenuKeepingFocus(m_TimelineTrackContextMenu.get(),
                                        static_cast<int>(screenX), static_cast<int>(screenY));
        });
        // Disable whole-track horizontal time drag; clip/key dragging remains enabled.
        m_TimeCompositeView->SetOnTrackTimeDragStarted({});
        m_TimeCompositeView->SetOnTrackTimeOffsetChanged({});
        m_TimeCompositeView->SetOnTrackTimeDragEnded({});
        m_TimeCompositeView->SetOnTrackMarkerAdded([this](size_t trackIdx, float time)
        {
            (void)InsertTimelineTrackMarkerAt(trackIdx, time);
        });
        m_TimeCompositeView->SetOnTrackMarkerMoved([this](size_t trackIdx, size_t markerIdx, float newTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            if (markerIdx >= m_CompositeModel->tracks[trackIdx].markers.size()) return;
            (void)ExecutePanelEditWithUndo("Move Track Marker", [this, trackIdx, markerIdx, newTime]()
            {
                if (trackIdx >= m_CompositeModel->tracks.size()) return false;
                auto& markers = m_CompositeModel->tracks[trackIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers[markerIdx].time = newTime;
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnTrackMarkerRemoved([this](size_t trackIdx, size_t markerIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Track Marker", [this, trackIdx, markerIdx]()
            {
                if (trackIdx >= m_CompositeModel->tracks.size()) return false;
                auto& markers = m_CompositeModel->tracks[trackIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(markerIdx));
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackValueKeyAdded([this](size_t trackIdx, float time) -> size_t
        {
            return InsertTimelineTrackKeyAt(trackIdx, time);
        });
        m_TimeCompositeView->SetOnTrackValueKeyMoved([this](size_t trackIdx, size_t keyIdx, float newTime)
        {
            // Keep key order stable while dragging so the captured key index remains undoable.
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Move Track Key", [this, trackIdx, keyIdx, newTime]()
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (keyIdx >= track.valueKeys.size()) return false;
                track.valueKeys[keyIdx].time = std::max(0.0f, newTime);
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                RefreshTimelineInspector();
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnTrackValueKeyRemoved([this](size_t trackIdx, size_t keyIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Track Key", [this, trackIdx, keyIdx]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].valueKeys;
                if (keyIdx >= keys.size()) return false;
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(keyIdx));
                m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                m_SelectedTimelineKey = static_cast<size_t>(-1);
                if (m_TimeCompositeView) m_TimeCompositeView->ClearMarkerInteractionState();
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackMethodKeyAdded([this](size_t trackIdx, float time) -> size_t
        {
            return InsertTimelineTrackKeyAt(trackIdx, time);
        });
        m_TimeCompositeView->SetOnTrackAudioKeyAdded([this](size_t trackIdx, float time) -> size_t
        {
            return InsertTimelineTrackKeyAt(trackIdx, time);
        });
        m_TimeCompositeView->SetOnTrackAudioKeyMoved([this](size_t trackIdx, size_t keyIdx, float newTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Move Audio Key", [this, trackIdx, keyIdx, newTime]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].audioKeys;
                if (keyIdx >= keys.size()) return false;
                keys[keyIdx].time = std::max(0.0f, newTime);
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                RefreshTimelineInspector();
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnTrackAudioKeyRemoved([this](size_t trackIdx, size_t keyIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Audio Key", [this, trackIdx, keyIdx]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].audioKeys;
                if (keyIdx >= keys.size()) return false;
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(keyIdx));
                m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                m_SelectedTimelineKey = static_cast<size_t>(-1);
                if (m_TimeCompositeView) m_TimeCompositeView->ClearMarkerInteractionState();
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackAnimationKeyAdded([this](size_t trackIdx, float time) -> size_t
        {
            return InsertTimelineTrackKeyAt(trackIdx, time);
        });
        m_TimeCompositeView->SetOnTrackAnimationKeyMoved([this](size_t trackIdx, size_t keyIdx, float newTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Move Animation Key", [this, trackIdx, keyIdx, newTime]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].animationKeys;
                if (keyIdx >= keys.size()) return false;
                keys[keyIdx].time = std::max(0.0f, newTime);
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                RefreshTimelineInspector();
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnTrackAnimationKeyRemoved([this](size_t trackIdx, size_t keyIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Animation Key", [this, trackIdx, keyIdx]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].animationKeys;
                if (keyIdx >= keys.size()) return false;
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(keyIdx));
                m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                m_SelectedTimelineKey = static_cast<size_t>(-1);
                if (m_TimeCompositeView) m_TimeCompositeView->ClearMarkerInteractionState();
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackMethodKeyMoved([this](size_t trackIdx, size_t keyIdx, float newTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Move Method Call", [this, trackIdx, keyIdx, newTime]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].methodKeys;
                if (keyIdx >= keys.size()) return false;
                keys[keyIdx].time = std::max(0.0f, newTime);
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                RefreshTimelineInspector();
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnTrackMethodKeyRemoved([this](size_t trackIdx, size_t keyIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Method Call", [this, trackIdx, keyIdx]()
            {
                auto& keys = m_CompositeModel->tracks[trackIdx].methodKeys;
                if (keyIdx >= keys.size()) return false;
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(keyIdx));
                m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                m_SelectedTimelineKey = static_cast<size_t>(-1);
                if (m_TimeCompositeView) m_TimeCompositeView->ClearMarkerInteractionState();
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackKeySelected([this](size_t trackIdx, CompositeTrackType type, size_t keyIdx)
        {
            m_SelectedCompositeTrack = trackIdx;
            m_SelectedCompositeClip = static_cast<size_t>(-1);
            m_SelectedTimelineKeyTrack = trackIdx;
            m_SelectedTimelineKeyType = type;
            m_SelectedTimelineKey = keyIdx;
            // Defer sidebar/inspector rebuild so a key press can capture and drag in the
            // same gesture without a layout rebuild clearing capture mid-press.
            PostSafeAction([this]()
            {
                RefreshSequencerSidebar();
                RefreshTimelineInspector();
            });
        });
        m_TimeCompositeView->SetOnClipMarkerAdded([this](size_t trackIdx, size_t clipIdx, float relTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Add Clip Marker", [this, trackIdx, clipIdx, relTime]()
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return false;
                track.clips[clipIdx].markers.push_back({relTime, ""});
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnClipMarkerMoved([this](size_t trackIdx, size_t clipIdx, size_t markerIdx, float newRelTime)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Move Clip Marker", [this, trackIdx, clipIdx, markerIdx, newRelTime]()
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return false;
                auto& markers = track.clips[clipIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers[markerIdx].time = newRelTime;
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                return true;
            }, true);
        });
        m_TimeCompositeView->SetOnClipMarkerRemoved([this](size_t trackIdx, size_t clipIdx, size_t markerIdx)
        {
            if (!m_CompositeModel || trackIdx >= m_CompositeModel->tracks.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Clip Marker", [this, trackIdx, clipIdx, markerIdx]()
            {
                auto& track = m_CompositeModel->tracks[trackIdx];
                if (clipIdx >= track.clips.size()) return false;
                auto& markers = track.clips[clipIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(markerIdx));
                if (m_TimeCompositeView) m_TimeCompositeView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnTrackReordered([this](size_t fromIdx, size_t toIdx)
        {
            if (!m_CompositeModel) return;
            (void)ExecutePanelEditWithUndo("Reorder Track", [this, fromIdx, toIdx]()
            {
                auto& tracks = m_CompositeModel->tracks;
                if (fromIdx >= tracks.size() || toIdx >= tracks.size()) return false;
                if (fromIdx < toIdx)
                {
                    std::rotate(tracks.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                                tracks.begin() + static_cast<std::ptrdiff_t>(fromIdx) + 1,
                                tracks.begin() + static_cast<std::ptrdiff_t>(toIdx) + 1);
                }
                else
                {
                    std::rotate(tracks.begin() + static_cast<std::ptrdiff_t>(toIdx),
                                tracks.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                                tracks.begin() + static_cast<std::ptrdiff_t>(fromIdx) + 1);
                }
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetModel(m_CompositeModel.get());
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                RefreshSequencerSidebar();
                return true;
            }, false);
        });
        m_TimeCompositeView->SetOnClipMovedToTrack([this](size_t fromTrack, size_t clipIdx, size_t toTrack, float newOffset)
        {
            if (!m_CompositeModel) return;
            auto moveClipToTrack = [this, fromTrack, clipIdx, toTrack, newOffset]() -> bool
            {
                auto& tracks = m_CompositeModel->tracks;
                if (fromTrack >= tracks.size() || toTrack > tracks.size()) return false;
                auto& srcClips = tracks[fromTrack].clips;
                if (clipIdx >= srcClips.size()) return false;
                CompositeClip clip = std::move(srcClips[clipIdx]);
                clip.offsetOnTimeline = newOffset;
                const CompositeTrackType sourceTrackType = tracks[fromTrack].type;
                srcClips.erase(srcClips.begin() + static_cast<std::ptrdiff_t>(clipIdx));
                size_t destinationTrack = toTrack;
                if (destinationTrack < tracks.size() && tracks[destinationTrack].type != sourceTrackType)
                {
                    CompositeTrack newTrack;
                    newTrack.type = sourceTrackType;
                    newTrack.name = DefaultTrackName(sourceTrackType, destinationTrack);
                    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(destinationTrack), std::move(newTrack));
                }
                else if (destinationTrack == tracks.size())
                {
                    CompositeTrack newTrack;
                    newTrack.type = sourceTrackType;
                    newTrack.name = DefaultTrackName(sourceTrackType, destinationTrack);
                    tracks.push_back(std::move(newTrack));
                }
                tracks[destinationTrack].clips.push_back(std::move(clip));
                CompositeClip& movedClip = tracks[destinationTrack].clips.back();
                if (!movedClip.linkedMediaGroupGuid.IsNull())
                {
                    for (size_t tr = 0; tr < tracks.size(); ++tr)
                    {
                        auto& track = tracks[tr];
                        if (track.type != CompositeTrackType::Audio && track.type != CompositeTrackType::Video)
                            continue;
                        for (size_t ci = 0; ci < track.clips.size(); ++ci)
                        {
                            auto& candidate = track.clips[ci];
                            if (&candidate == &movedClip)
                                continue;
                            if (candidate.linkedMediaGroupGuid != movedClip.linkedMediaGroupGuid)
                                continue;
                            CompositeTrack* audioTrack = nullptr;
                            if (tracks[destinationTrack].type == CompositeTrackType::Audio)
                                audioTrack = &tracks[destinationTrack];
                            else if (track.type == CompositeTrackType::Audio)
                                audioTrack = &track;
                            if (!audioTrack || !audioTrack->syncLinkedVideo)
                                continue;
                            float targetOffset = movedClip.offsetOnTimeline;
                            if (audioTrack->mediaSyncMode == CompositeMediaSyncMode::KeepOffset)
                            {
                                if (tracks[destinationTrack].type == CompositeTrackType::Audio)
                                    targetOffset = movedClip.offsetOnTimeline - movedClip.linkedMediaOffsetSeconds;
                                else if (track.type == CompositeTrackType::Audio)
                                    targetOffset = movedClip.offsetOnTimeline + candidate.linkedMediaOffsetSeconds;
                            }
                            candidate.offsetOnTimeline = std::max(0.0f, targetOffset);
                        }
                    }
                }
                m_SelectedCompositeTrack = destinationTrack;
                m_SelectedCompositeClip = tracks[destinationTrack].clips.empty()
                    ? static_cast<size_t>(-1)
                    : tracks[destinationTrack].clips.size() - 1u;
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetModel(m_CompositeModel.get());
                    if (m_SelectedCompositeClip != static_cast<size_t>(-1))
                        m_TimeCompositeView->SetSelectedClip(destinationTrack, m_SelectedCompositeClip);
                    else
                        m_TimeCompositeView->ClearSelection();
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                return true;
            };
            if (m_PendingPanelUndoActive)
            {
                if (moveClipToTrack())
                {
                    RefreshSequencerSidebar();
                    RefreshTimelineInspector();
                    m_TimelineDirty = true;
                    UpdateTitle();
                    if (m_UnsavedIndicator)
                        m_UnsavedIndicator->RemoveClass("hidden");
                }
                return;
            }
            (void)ExecutePanelEditWithUndo("Move Clip to Track", [this, fromTrack, clipIdx, toTrack, newOffset]()
            {
                auto& tracks = m_CompositeModel->tracks;
                if (fromTrack >= tracks.size() || toTrack > tracks.size()) return false;
                auto& srcClips = tracks[fromTrack].clips;
                if (clipIdx >= srcClips.size()) return false;
                CompositeClip clip = std::move(srcClips[clipIdx]);
                clip.offsetOnTimeline = newOffset;
                const CompositeTrackType sourceTrackType = tracks[fromTrack].type;
                srcClips.erase(srcClips.begin() + static_cast<std::ptrdiff_t>(clipIdx));
                size_t destinationTrack = toTrack;
                if (destinationTrack < tracks.size() && tracks[destinationTrack].type != sourceTrackType)
                {
                    CompositeTrack newTrack;
                    newTrack.type = sourceTrackType;
                    newTrack.name = DefaultTrackName(sourceTrackType, destinationTrack);
                    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(destinationTrack), std::move(newTrack));
                }
                else if (destinationTrack == tracks.size())
                {
                    CompositeTrack newTrack;
                    newTrack.type = sourceTrackType;
                    newTrack.name = DefaultTrackName(sourceTrackType, destinationTrack);
                    tracks.push_back(std::move(newTrack));
                }
                tracks[destinationTrack].clips.push_back(std::move(clip));
                CompositeClip& movedClip = tracks[destinationTrack].clips.back();
                if (!movedClip.linkedMediaGroupGuid.IsNull())
                {
                    for (size_t tr = 0; tr < tracks.size(); ++tr)
                    {
                        auto& track = tracks[tr];
                        if (track.type != CompositeTrackType::Audio && track.type != CompositeTrackType::Video)
                            continue;
                        for (size_t ci = 0; ci < track.clips.size(); ++ci)
                        {
                            auto& candidate = track.clips[ci];
                            if (&candidate == &movedClip)
                                continue;
                            if (candidate.linkedMediaGroupGuid != movedClip.linkedMediaGroupGuid)
                                continue;
                            CompositeTrack* audioTrack = nullptr;
                            if (tracks[destinationTrack].type == CompositeTrackType::Audio)
                                audioTrack = &tracks[destinationTrack];
                            else if (track.type == CompositeTrackType::Audio)
                                audioTrack = &track;
                            if (!audioTrack || !audioTrack->syncLinkedVideo)
                                continue;
                            float targetOffset = movedClip.offsetOnTimeline;
                            if (audioTrack->mediaSyncMode == CompositeMediaSyncMode::KeepOffset)
                            {
                                if (tracks[destinationTrack].type == CompositeTrackType::Audio)
                                    targetOffset = movedClip.offsetOnTimeline - movedClip.linkedMediaOffsetSeconds;
                                else if (track.type == CompositeTrackType::Audio)
                                    targetOffset = movedClip.offsetOnTimeline + candidate.linkedMediaOffsetSeconds;
                            }
                            candidate.offsetOnTimeline = std::max(0.0f, targetOffset);
                        }
                    }
                }
                if (m_TimeCompositeView)
                {
                    m_TimeCompositeView->SetModel(m_CompositeModel.get());
                    m_TimeCompositeView->ClearSelection();
                    m_TimeCompositeView->MarkDirty(VisualDirty);
                }
                return true;
            }, false);
        });
    }

    if (m_DopeSheetView)
    {
        m_DopeSheetView->SetOnKeyframeTimeChanged([this](int channel, float currentTime, float newTime)
                                                 {
                                                     if (!EnsureEditableClip() || !m_CurrentClipAsset)
                                                         return false;
                                                     if (m_CurrentClipAsset->SetKeyframeTime(static_cast<size_t>(channel), currentTime, newTime))
                                                     {
                                                         RefreshEditedClipState();
                                                         return true;
                                                     }
                                                     return false;
                                                 });
    }

    UIElement* laneEl = FindById("AnimationWindowClipEditorPlaceholder");
    m_LaneClipEditorView = laneEl ? dynamic_cast<LaneClipEditorView*>(laneEl) : nullptr;
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        m_LaneClipEditorView->SetOnSeekToTime(seekToTime);
        if (!m_LaneClipModel)
            m_LaneClipModel = std::make_unique<LaneClipModel>();
        RefreshSequencingModels();
        m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
        m_LaneClipEditorView->SetOnClipSelected([this](size_t laneIdx, size_t clipIdx)
                                               {
                                                   SelectLaneClip(laneIdx, clipIdx);
                                               });
        m_LaneClipEditorView->SetOnClipMoved([this](size_t laneIdx, size_t clipIdx, float newStartTime)
                                            {
                                                if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size())
                                                    return;
                                                (void)ExecutePanelEditWithUndo("Move Lane Clip",
                                                                              [this, laneIdx, clipIdx, newStartTime]()
                                                                              {
                                                                                  auto& lane = m_LaneClipModel->lanes[laneIdx];
                                                                                  if (clipIdx >= lane.clips.size())
                                                                                      return false;
                                                                                  lane.clips[clipIdx].startTimeOnLane = std::max(0.0f, newStartTime);
                                                                                  SelectLaneClip(laneIdx, clipIdx);
                                                                                  if (m_LaneClipEditorView)
                                                                                  {
                                                                                      if (m_LaneClipEditorView->GetSelectedClips().size() <= 1)
                                                                                          m_LaneClipEditorView->SetSelectedClip(laneIdx, clipIdx);
                                                                                      m_LaneClipEditorView->MarkDirty(VisualDirty);
                                                                                  }
                                                                                  return true;
                                                                              },
                                                                              true);
                                            });
        m_LaneClipEditorView->SetOnClipResized([this](size_t laneIdx, size_t clipIdx, float newDuration)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Resize Lane Clip",
                [this, laneIdx, clipIdx, newDuration]()
                {
                    auto& lane = m_LaneClipModel->lanes[laneIdx];
                    if (clipIdx >= lane.clips.size()) return false;
                    auto& inst = lane.clips[clipIdx];
                    const float baseDur = inst.sourceDuration * static_cast<float>(std::max(inst.loopCount, 1));
                    inst.scale = baseDur > 0.0f
                        ? std::max(1.0f / 30.0f / baseDur, newDuration / baseDur)
                        : 1.0f;
                    if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_LaneClipEditorView->SetOnClipStartResized([this](size_t laneIdx, size_t clipIdx, float newStartTime, float newDuration)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Resize Lane Clip Start",
                [this, laneIdx, clipIdx, newStartTime, newDuration]()
                {
                    auto& lane = m_LaneClipModel->lanes[laneIdx];
                    if (clipIdx >= lane.clips.size()) return false;
                    auto& inst = lane.clips[clipIdx];
                    inst.startTimeOnLane = newStartTime;
                    const float baseDur = inst.sourceDuration * static_cast<float>(std::max(inst.loopCount, 1));
                    inst.scale = baseDur > 0.0f
                        ? std::max(1.0f / 30.0f / baseDur, newDuration / baseDur)
                        : 1.0f;
                    if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_LaneClipEditorView->SetOnClipFadeInChanged([this](size_t laneIdx, size_t clipIdx, float newFadeIn)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Set Lane Clip Fade In",
                [this, laneIdx, clipIdx, newFadeIn]()
                {
                    auto& lane = m_LaneClipModel->lanes[laneIdx];
                    if (clipIdx >= lane.clips.size()) return false;
                    auto& inst = lane.clips[clipIdx];
                    const float duration = std::max(0.0f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
                    const float maxFade  = std::max(0.0f, duration - inst.fadeOutDuration);
                    inst.fadeInDuration  = std::clamp(newFadeIn, 0.0f, maxFade);
                    if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
        m_LaneClipEditorView->SetOnClipFadeOutChanged([this](size_t laneIdx, size_t clipIdx, float newFadeOut)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Set Lane Clip Fade Out",
                [this, laneIdx, clipIdx, newFadeOut]()
                {
                    auto& lane = m_LaneClipModel->lanes[laneIdx];
                    if (clipIdx >= lane.clips.size()) return false;
                    auto& inst = lane.clips[clipIdx];
                    const float duration = std::max(0.0f, inst.sourceDuration * inst.scale * static_cast<float>(std::max(inst.loopCount, 1)));
                    const float maxFade  = std::max(0.0f, duration - inst.fadeInDuration);
                    inst.fadeOutDuration = std::clamp(newFadeOut, 0.0f, maxFade);
                    if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                    return true;
                }, true);
        });
    }

    if (m_CurrentClip && (!m_CurvesGraphView || m_CurvesGraphView->GetClip() != m_CurrentClip))
        SetCurrentClip(m_CurrentClip);
    else if (m_CurrentClip)
        RefreshChannelSelection();

    RefreshLeftPane();

    auto onTimeChange = [this](float t)
    {
        m_TimelineState.currentTime = std::clamp(t, m_TimelineState.viewStart, m_TimelineState.viewEnd);
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
    };
    auto onZoom = [this](float scrollY, float mouseX) { ApplyTimelineZoom(scrollY, mouseX); };
    auto onPan = [this](float deltaTimeSeconds)
    {
        const float duration = m_TimelineState.viewEnd - m_TimelineState.viewStart;
        m_TimelineState.viewStart -= deltaTimeSeconds;
        m_TimelineState.viewEnd -= deltaTimeSeconds;
        if (m_TimelineState.viewStart < 0.0f)
        {
            m_TimelineState.viewStart = 0.0f;
            m_TimelineState.viewEnd = m_TimelineState.viewStart + duration;
        }
        if (m_TimelineState.viewEnd > m_TimelineState.fullEnd)
        {
            m_TimelineState.viewEnd = m_TimelineState.fullEnd;
            m_TimelineState.viewStart = m_TimelineState.viewEnd - duration;
            if (m_TimelineState.viewStart < 0.0f)
                m_TimelineState.viewStart = 0.0f;
        }
        if (m_DopeSheetView)
            m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_CurvesGraphView)
            m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_TimeCompositeView)
            m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_LaneClipEditorView)
            m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
    };

    auto onScrubBegin = [this]()
    {
        if (!m_IsSeekActive)
        {
            m_IsSeekActive = true;
            m_SeekBeforeTime = m_TimelineState.currentTime;
        }
    };
    auto onScrubEnd = [this]()
    {
        if (m_IsSeekActive)
        {
            m_IsSeekActive = false;
            const float afterTime = m_TimelineState.currentTime;
            if (m_UndoRedo && afterTime != m_SeekBeforeTime)
            {
                const float before = m_SeekBeforeTime;
                auto cmd = std::make_unique<AnimationSeekCommand>(before, afterTime,
                    [this](float t)
                    {
                        m_TimelineState.currentTime = std::clamp(t, m_TimelineState.viewStart, m_TimelineState.viewEnd);
                        UpdateTimelineRulerAndLabels();
                        UpdateCurrentFrameLabel();
                    });
                m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
            }
        }
    };

    if (m_TimelineBarElement)
    {
        m_TimelineBarElement->SetOnTimeChange(onTimeChange);
        m_TimelineBarElement->SetOnZoom(onZoom);
        m_TimelineBarElement->SetOnPan(onPan);
        m_TimelineBarElement->SetOnScrubBegin(onScrubBegin);
        m_TimelineBarElement->SetOnScrubEnd(onScrubEnd);
    }
    if (m_TimeLabelsElement)
    {
        m_TimeLabelsElement->SetOnTimeChange(onTimeChange);
        m_TimeLabelsElement->SetOnZoom(onZoom);
        m_TimeLabelsElement->SetOnPan(onPan);
        m_TimeLabelsElement->SetOnScrubBegin(onScrubBegin);
        m_TimeLabelsElement->SetOnScrubEnd(onScrubEnd);
    }
    if (m_FrameLabelsElement)
    {
        m_FrameLabelsElement->SetOnTimeChange(onTimeChange);
        m_FrameLabelsElement->SetOnZoom(onZoom);
        m_FrameLabelsElement->SetOnPan(onPan);
        m_FrameLabelsElement->SetOnScrubBegin(onScrubBegin);
        m_FrameLabelsElement->SetOnScrubEnd(onScrubEnd);
    }
    if (m_TimeRangeSlider)
        m_TimeRangeSlider->SetOnPan(onPan);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetOnPan(onPan);
    if (m_DopeSheetView)
        m_DopeSheetView->SetOnPan(onPan);
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->SetOnPan(onPan);
        m_TimeCompositeView->SetOnMediaDropped([this](size_t trackHint, float timeOffset,
                                                      const std::vector<std::filesystem::path>& paths)
        {
            bool hasVideo = false;
            for (const auto& p : paths)
            {
                const AssetType assetType = GetAssetTypeFromExtension(GetCompoundExtensionFromPath(p.string()));
                if (assetType == AssetType::Video)
                {
                    hasVideo = true;
                    break;
                }
            }
            // Video import requires a saved timeline asset path.
            if (hasVideo && m_CurrentTimelinePath.empty())
                return;
            if (!m_CompositeModel)
                m_CompositeModel = std::make_unique<TimeCompositeModel>();
            (void)ExecutePanelEditWithUndo("Add Media Track",
                [this, trackHint, timeOffset, paths]()
                {
                    size_t insertAt = std::min(trackHint, m_CompositeModel->tracks.size());
                    for (const auto& p : paths)
                    {
                        const AssetType assetType = GetAssetTypeFromExtension(GetCompoundExtensionFromPath(p.string()));
                        CompositeTrackType trackType = CompositeTrackType::Animation;
                        if (assetType == AssetType::Audio)
                            trackType = CompositeTrackType::Audio;
                        else if (assetType == AssetType::Video)
                            trackType = CompositeTrackType::Video;
                        const std::string stem = p.stem().string();
                        if (assetType == AssetType::Audio)
                        {
                            CompositeAudioKey key{};
                            key.time = std::max(0.0f, timeOffset);
                            key.name = stem.empty() ? "Audio" : stem;
                            key.sourcePath = p;
                            // Resolve through AssetManager (normalizes + registers): a raw
                            // registry lookup on the dropped absolute path returns null on
                            // web, and that null would be persisted into the timeline.
                            AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
                            key.audioGuid = assets.ResolveAssetGuid(p);

                            if (insertAt < m_CompositeModel->tracks.size() &&
                                m_CompositeModel->tracks[insertAt].type == CompositeTrackType::Audio)
                            {
                                auto& keys = m_CompositeModel->tracks[insertAt].audioKeys;
                                keys.push_back(std::move(key));
                                std::sort(keys.begin(), keys.end(),
                                          [](const CompositeAudioKey& a, const CompositeAudioKey& b) { return a.time < b.time; });
                                ++insertAt;
                                continue;
                            }

                            CompositeTrack track;
                            track.type = CompositeTrackType::Audio;
                            track.name = stem.empty() ? DefaultTrackName(trackType, insertAt) : stem;
                            track.propertyPath = "audio";
                            track.audioKeys.push_back(std::move(key));
                            m_CompositeModel->tracks.insert(
                                m_CompositeModel->tracks.begin() + static_cast<std::ptrdiff_t>(insertAt),
                                std::move(track));
                            ++insertAt;
                            continue;
                        }

                        CompositeClip clip{};
                        clip.name = stem;
                        clip.sourcePath = p;
                        const bool isVideoDrop = assetType == AssetType::Video;
                        GUID linkedMediaGroupGuid = GUID::Null();
                        if (isVideoDrop)
                            linkedMediaGroupGuid = GUID::Generate();
                        clip.linkedMediaGroupGuid = linkedMediaGroupGuid;
                        if (assetType == AssetType::Animation)
                        {
                            // Same contract as the audio key above: normalize + register, so
                            // the clip is never stored with a null GUID.
                            AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
                            clip.clipGuid = assets.ResolveAssetGuid(p);
                        }
                        clip.offsetOnTimeline = timeOffset;
                        clip.inTime = 0.0f;
                        clip.outTime = 5.0f;
                        size_t clipTrackIndex = insertAt;
                        if (insertAt < m_CompositeModel->tracks.size() &&
                            m_CompositeModel->tracks[insertAt].type == trackType)
                        {
                            m_CompositeModel->tracks[insertAt].clips.push_back(std::move(clip));
                            clipTrackIndex = insertAt;
                        }
                        else
                        {
                            CompositeTrack track;
                            track.type = trackType;
                            track.name = stem.empty() ? DefaultTrackName(trackType, insertAt) : stem;
                            track.clips.push_back(std::move(clip));
                            m_CompositeModel->tracks.insert(
                                m_CompositeModel->tracks.begin() + static_cast<std::ptrdiff_t>(insertAt),
                                std::move(track));
                            clipTrackIndex = insertAt;
                        }
                        insertAt = clipTrackIndex + 1u;

                        if (isVideoDrop)
                        {
                            CompositeClip audioClip{};
                            audioClip.name = stem.empty() ? "Audio" : stem + " Audio";
                            audioClip.sourcePath = p;
                            audioClip.linkedMediaGroupGuid = linkedMediaGroupGuid;
                            audioClip.offsetOnTimeline = timeOffset;
                            audioClip.inTime = 0.0f;
                            audioClip.outTime = 5.0f;

                            const size_t audioTrackIndex = clipTrackIndex + 1u;
                            if (audioTrackIndex < m_CompositeModel->tracks.size() &&
                                m_CompositeModel->tracks[audioTrackIndex].type == CompositeTrackType::Audio)
                            {
                                m_CompositeModel->tracks[audioTrackIndex].clips.push_back(std::move(audioClip));
                            }
                            else
                            {
                                CompositeTrack audioTrack;
                                audioTrack.type = CompositeTrackType::Audio;
                                audioTrack.name = stem.empty()
                                    ? DefaultTrackName(CompositeTrackType::Audio, audioTrackIndex)
                                    : stem + " Audio";
                                audioTrack.propertyPath = "audio";
                                audioTrack.syncLinkedVideo = true;
                                audioTrack.clips.push_back(std::move(audioClip));
                                m_CompositeModel->tracks.insert(
                                    m_CompositeModel->tracks.begin() + static_cast<std::ptrdiff_t>(audioTrackIndex),
                                    std::move(audioTrack));
                            }
                            insertAt = audioTrackIndex + 1u;
                        }
                    }
                    if (m_TimeCompositeView)
                    {
                        m_TimeCompositeView->SetModel(m_CompositeModel.get());
                        m_TimeCompositeView->MarkDirty(VisualDirty);
                    }
                    RefreshSequencerSidebar();
                    return true;
                },
                false);
        });
    }
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->SetOnPan(onPan);
        m_LaneClipEditorView->SetOnLaneMarkerAdded([this](size_t laneIdx, float time)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Add Lane Marker", [this, laneIdx, time]()
            {
                m_LaneClipModel->lanes[laneIdx].markers.push_back({time, ""});
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_LaneClipEditorView->SetOnLaneMarkerMoved([this](size_t laneIdx, size_t markerIdx, float newTime)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            if (markerIdx >= m_LaneClipModel->lanes[laneIdx].markers.size()) return;
            (void)ExecutePanelEditWithUndo("Move Lane Marker", [this, laneIdx, markerIdx, newTime]()
            {
                if (laneIdx >= m_LaneClipModel->lanes.size()) return false;
                auto& markers = m_LaneClipModel->lanes[laneIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers[markerIdx].time = newTime;
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, true);
        });
        m_LaneClipEditorView->SetOnLaneMarkerRemoved([this](size_t laneIdx, size_t markerIdx)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Lane Marker", [this, laneIdx, markerIdx]()
            {
                if (laneIdx >= m_LaneClipModel->lanes.size()) return false;
                auto& markers = m_LaneClipModel->lanes[laneIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(markerIdx));
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_LaneClipEditorView->SetOnClipMarkerAdded([this](size_t laneIdx, size_t clipIdx, float relTime)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Add Clip Marker", [this, laneIdx, clipIdx, relTime]()
            {
                auto& lane = m_LaneClipModel->lanes[laneIdx];
                if (clipIdx >= lane.clips.size()) return false;
                lane.clips[clipIdx].markers.push_back({relTime, ""});
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
        m_LaneClipEditorView->SetOnClipMarkerMoved([this](size_t laneIdx, size_t clipIdx, size_t markerIdx, float newRelTime)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Move Clip Marker", [this, laneIdx, clipIdx, markerIdx, newRelTime]()
            {
                auto& lane = m_LaneClipModel->lanes[laneIdx];
                if (clipIdx >= lane.clips.size()) return false;
                auto& markers = lane.clips[clipIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers[markerIdx].time = newRelTime;
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, true);
        });
        m_LaneClipEditorView->SetOnClipMarkerRemoved([this](size_t laneIdx, size_t clipIdx, size_t markerIdx)
        {
            if (!m_LaneClipModel || laneIdx >= m_LaneClipModel->lanes.size()) return;
            (void)ExecutePanelEditWithUndo("Remove Clip Marker", [this, laneIdx, clipIdx, markerIdx]()
            {
                auto& lane = m_LaneClipModel->lanes[laneIdx];
                if (clipIdx >= lane.clips.size()) return false;
                auto& markers = lane.clips[clipIdx].markers;
                if (markerIdx >= markers.size()) return false;
                markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(markerIdx));
                if (m_LaneClipEditorView) m_LaneClipEditorView->MarkDirty(VisualDirty);
                return true;
            }, false);
        });
    }

    SetActiveView(m_ActiveView);
    UpdatePlayButtonState();
    UpdateTitle();
    RefreshChannelSelection();

    SetupToolbarDragDrop();
    WireToolbarRightClickToggles();

    // Create unsaved-changes modal (once). Attach to the UIManager root so the
    // backdrop covers the full window, not just the animation panel.
    if (!m_UnsavedChangesModal)
    {
        auto modal = std::make_unique<SaveSceneChangesModal>();
        m_UnsavedChangesModal = modal.get();
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal)); // fallback

        m_UnsavedChangesModal->SetOnSave([this]()
        {
            SaveCurrentClip();
            if (!m_PendingOpenPath.empty())
            {
                const auto path = std::move(m_PendingOpenPath);
                const uint32 idx = m_PendingOpenIndex;
                m_PendingOpenPath.clear();
                DoOpenAnimation(path, idx);
            }
        });
        m_UnsavedChangesModal->SetOnDontSave([this]()
        {
            if (!m_PendingOpenPath.empty())
            {
                const auto path = std::move(m_PendingOpenPath);
                const uint32 idx = m_PendingOpenIndex;
                m_PendingOpenPath.clear();
                DoOpenAnimation(path, idx);
            }
        });
        m_UnsavedChangesModal->SetOnCancel([this]()
        {
            m_PendingOpenPath.clear();
            // Restore dropdown to current selection so it doesn't show the cancelled choice.
            RefreshClipDropdown();
        });
    }

    if (!m_GltfInterpWarningModal)
    {
        auto modal = std::make_unique<ConfirmActionModal>();
        m_GltfInterpWarningModal = modal.get();
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal));
    }

    // Create the overwrite confirmation modal (once; re-runnable guard via pointer check).
    if (!m_OverwriteConfirmModal)
    {
        auto modal = std::make_unique<ConfirmActionModal>();
        m_OverwriteConfirmModal = modal.get();
        m_OverwriteConfirmModal->SetOnConfirm([this]()
        {
            if (!m_PendingNewAnimationPath.empty())
                CreateNewAnimationAtPath(m_PendingNewAnimationPath);
            m_PendingNewAnimationPath.clear();
        });
        m_OverwriteConfirmModal->SetOnCancel([this]()
        {
            m_PendingNewAnimationPath.clear();
        });
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal));
    }

    if (m_TimeCompositeView)
        m_TimeCompositeView->SetFocusProxy(this);
    if (m_DopeSheetView)
        m_DopeSheetView->SetFocusProxy(this);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetFocusProxy(this);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->SetFocusProxy(this);
}

void AnimationWindowPanel::ApplyAppearanceSettings()
{
    const AnimationWindowSettings& s = AnimationWindowSettings::Get();
    m_CurveColorOverrides[0] = s.ColorX;
    m_CurveColorOverrides[1] = s.ColorY;
    m_CurveColorOverrides[2] = s.ColorZ;
    m_CurveColorOverrides[3] = s.ColorW;
    if (m_CurvesGraphView)
    {
        m_CurvesGraphView->SetChannelColors(m_CurveColorOverrides);
        m_CurvesGraphView->SetCurveLineWidth(s.CurveLineWidth);
        m_CurvesGraphView->SetGridHLineColor(s.GridHLineColor);
        m_CurvesGraphView->SetGridVLineColor(s.GridVLineColor);
        m_CurvesGraphView->SetGridLineThickness(s.GridLineThickness);
        m_CurvesGraphView->SetBaselineColor(s.BaselineColor);
        m_CurvesGraphView->SetBaselineThickness(s.BaselineThickness);
        m_CurvesGraphView->SetKeyframeColor(s.KeyframeColor);
        m_CurvesGraphView->SetKeyframeSelectedColor(s.KeyframeSelectedColor);
    }
    if (auto* mainSplit = FindById("AnimationWindowMainSplit"))
    {
        if (s.PropertiesPaneOnRight)
            mainSplit->AddClass("properties-on-right");
        else
            mainSplit->RemoveClass("properties-on-right");
    }
    if (auto* rightCol = FindById("AnimationWindowRightColumn"))
    {
        if (s.RulerAtTop)
            rightCol->AddClass("animationwindow-ruler-top");
        else
            rightCol->RemoveClass("animationwindow-ruler-top");
    }
}

void AnimationWindowPanel::RestoreTimelineKeyboardFocus()
{
    if (UIManager* ui = GetOwnerManager())
        ui->FocusElement(this);
}

void AnimationWindowPanel::ShowContextMenuKeepingFocus(INativeContextMenu* menu, int x, int y)
{
    if (!menu || !m_Window)
        return;
    RestoreTimelineKeyboardFocus();
    menu->Show(m_Window, x, y);
    RestoreTimelineKeyboardFocus();
    PostSafeAction([this]() { RestoreTimelineKeyboardFocus(); });
}

void AnimationWindowPanel::SetupToolbarDragDrop()
{
    UIManager* ui = GetOwnerManager();
    if (!ui || !m_Toolbar)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    static const std::vector<Editor::ToolbarDragDrop::ContainerConfig> kConfig = {
        {"anim-toolbar-edit",            "ui.animwindow.toolbar.edit.order"},
        {"anim-toolbar-view",            "ui.animwindow.toolbar.view.order"},
        {"anim-toolbar-frame",           "ui.animwindow.toolbar.frame.order"},
        {"anim-toolbar-snap",            "ui.animwindow.toolbar.snap.order"},
        {"anim-toolbar-curve",           "ui.animwindow.toolbar.curve.order"},
        {"anim-toolbar-ease",            "ui.animwindow.toolbar.ease.order"},
        {"anim-toolbar-tools",           "ui.animwindow.toolbar.tools.order"},
        {"animationwindow-toolbar-right","ui.animwindow.toolbar.right.order"},
    };

    if (!m_ToolbarDragDrop)
        m_ToolbarDragDrop = std::make_unique<Editor::ToolbarDragDrop>();

    if (!m_ToolbarDragDropOrderLoaded)
        RestoreToolbarGapsFromPrefs();

    // Re-runnable: ToolbarDragDrop guards against duplicate registration via
    // the "toolbar-dragdrop-initialized" class on each button.
    m_ToolbarDragDrop->Setup(root, kConfig, m_Toolbar);

    if (!m_ToolbarDragDropOrderLoaded)
    {
        m_ToolbarDragDrop->LoadButtonOrder(m_Toolbar, kConfig);
        m_ToolbarDragDropOrderLoaded = true;
    }
}

namespace
{
constexpr const char* kAnimToolbarGapIdPrefix = "anim-gap-";

const std::vector<std::pair<std::string, std::string>>& AnimToolbarContainerOrderKeys()
{
    static const std::vector<std::pair<std::string, std::string>> kKeys = {
        {"anim-toolbar-edit",             "ui.animwindow.toolbar.edit.order"},
        {"anim-toolbar-view",             "ui.animwindow.toolbar.view.order"},
        {"anim-toolbar-frame",            "ui.animwindow.toolbar.frame.order"},
        {"anim-toolbar-snap",             "ui.animwindow.toolbar.snap.order"},
        {"anim-toolbar-curve",            "ui.animwindow.toolbar.curve.order"},
        {"anim-toolbar-ease",             "ui.animwindow.toolbar.ease.order"},
        {"anim-toolbar-tools",            "ui.animwindow.toolbar.tools.order"},
        {"animationwindow-toolbar-right", "ui.animwindow.toolbar.right.order"},
    };
    return kKeys;
}

UIElement* FindAnimToolbarContainerByClass(UIElement* root, const std::string& className)
{
    if (!root) return nullptr;
    if (root->HasClass(className)) return root;
    for (const auto& child : root->GetChildren())
        if (auto* found = FindAnimToolbarContainerByClass(child.get(), className))
            return found;
    return nullptr;
}

void SaveAnimToolbarContainerOrder(UIElement* container, const std::string& key)
{
    if (!container || key.empty()) return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    nlohmann::json ids = nlohmann::json::array();
    for (const auto& child : container->GetChildren())
        if (!child->GetId().empty()) ids.push_back(child->GetId());
    prefs.SetJson(key, ids);
    (void)prefs.Save(&err);
}

float GetKeyComponentByPath(const AnimKeyframe& key, AnimPath path, uint32 component)
{
    switch (path)
    {
    case AnimPath::Translation: return key.translation[std::min<uint32>(component, 2u)];
    case AnimPath::Scale:       return key.scale[std::min<uint32>(component, 2u)];
    case AnimPath::Rotation:    return key.rotation[std::min<uint32>(component, 3u)];
    case AnimPath::MorphWeight: return key.translation[0];
    }
    return 0.0f;
}

uint32 GetComponentCountByPath(AnimPath path)
{
    if (path == AnimPath::MorphWeight)
        return 1u;
    return path == AnimPath::Rotation ? 4u : 3u;
}

// Single-component curve evaluator mirroring the runtime sampler (see EvalCubicHermite
// in Engine/Source/Engine/Rendering/AnimationSampling.cpp). Operates directly on a
// keyframe vector so the curve-reduction loop can simulate "what would playback look
// like if we removed key K" without round-tripping through the asset.
float EvalKeyframeCurveValue(const std::vector<AnimKeyframe>& keys,
                             AnimPath path,
                             AnimInterp baseInterp,
                             uint32 component,
                             float time)
{
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1 || time <= keys.front().time)
        return GetKeyComponentByPath(keys.front(), path, component);
    if (time >= keys.back().time)
        return GetKeyComponentByPath(keys.back(), path, component);

    size_t hi = 0;
    while (hi < keys.size() && keys[hi].time < time) ++hi;
    const size_t lo = (hi == 0) ? 0 : (hi - 1);
    if (hi >= keys.size()) hi = keys.size() - 1;

    const AnimKeyframe& k0 = keys[lo];
    const AnimKeyframe& k1 = keys[hi];
    const float dt = k1.time - k0.time;
    if (dt <= 0.0f) return GetKeyComponentByPath(k0, path, component);

    const float alpha = std::clamp((time - k0.time) / dt, 0.0f, 1.0f);
    const AnimInterp segInterp = k0.hasSegmentInterpOverride ? k0.segmentInterp : baseInterp;

    const float v0 = GetKeyComponentByPath(k0, path, component);
    const float v1 = GetKeyComponentByPath(k1, path, component);

    if (segInterp == AnimInterp::Step)
        return v0;
    if (segInterp == AnimInterp::Linear)
        return v0 + (v1 - v0) * alpha;

    // CubicSpline — value-only Bezier with the standard [0.05, 0.49] weight clamp.
    const float c0dt = dt * std::clamp(k0.outWeight[component], 0.05f, 0.49f);
    const float c1dt = dt * std::clamp(k1.inWeight[component],  0.05f, 0.49f);
    const float c0 = v0 + k0.outTangent[component] * c0dt;
    const float c1 = v1 - k1.inTangent[component]  * c1dt;
    const float u2 = alpha * alpha;
    const float u3 = u2 * alpha;
    const float inv = 1.0f - alpha;
    const float inv2 = inv * inv;
    const float inv3 = inv2 * inv;
    return inv3 * v0 + 3.0f * inv2 * alpha * c0 + 3.0f * inv * u2 * c1 + u3 * v1;
}
} // namespace

void AnimationWindowPanel::RestoreToolbarGapsFromPrefs()
{
    if (!m_Toolbar) return;

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    int maxSerial = 0;
    for (const auto& [cssClass, key] : AnimToolbarContainerOrderKeys())
    {
        UIElement* container = FindAnimToolbarContainerByClass(m_Toolbar, cssClass);
        if (!container) continue;

        const auto& json = prefs.Json();
        auto it = json.find(key);
        if (it == json.end() || !it->is_array()) continue;

        for (const auto& item : *it)
        {
            if (!item.is_string()) continue;
            const std::string id = item.get<std::string>();
            if (id.rfind(kAnimToolbarGapIdPrefix, 0) != 0) continue;

            // Skip if already present (from previous restore).
            bool exists = false;
            for (const auto& child : container->GetChildren())
                if (child->GetId() == id) { exists = true; break; }
            if (exists) continue;

            auto gap = std::make_unique<Button>();
            gap->SetId(id);
            gap->AddClass("toolbar-gap");
            container->AddChild(std::move(gap));

            try
            {
                int serial = std::stoi(id.substr(std::strlen(kAnimToolbarGapIdPrefix)));
                if (serial > maxSerial) maxSerial = serial;
            }
            catch (...) {}
        }
    }
    if (maxSerial >= m_NextToolbarGapSerial)
        m_NextToolbarGapSerial = maxSerial + 1;
}

void AnimationWindowPanel::CreateToolbarGap(UIElement* container, int insertIndex)
{
    if (!container) return;

    std::string id = std::string(kAnimToolbarGapIdPrefix) + std::to_string(m_NextToolbarGapSerial++);
    auto gap = std::make_unique<Button>();
    gap->SetId(id);
    gap->AddClass("toolbar-gap");

    const int childCount = static_cast<int>(container->GetChildren().size());
    if (insertIndex < 0 || insertIndex > childCount)
        container->AddChild(std::move(gap));
    else
        container->InsertChild(static_cast<size_t>(insertIndex), std::move(gap));

    // Re-register drag-drop so the new gap participates in reordering.
    SetupToolbarDragDrop();

    for (const auto& [cssClass, key] : AnimToolbarContainerOrderKeys())
    {
        if (container->HasClass(cssClass))
        {
            SaveAnimToolbarContainerOrder(container, key);
            break;
        }
    }
}

void AnimationWindowPanel::RemoveToolbarGap(Button* gap)
{
    if (!gap) return;
    UIElement* parent = gap->GetParent();
    if (!parent) return;

    std::string saveKey;
    for (const auto& [cssClass, key] : AnimToolbarContainerOrderKeys())
    {
        if (parent->HasClass(cssClass)) { saveKey = key; break; }
    }

    parent->RemoveChild(gap);

    if (!saveKey.empty())
        SaveAnimToolbarContainerOrder(parent, saveKey);
}

namespace
{
struct AnimToolbarToggleEntry
{
    const char* id;
    const char* title;
};

constexpr AnimToolbarToggleEntry kAnimToolbarToggles[] = {
    {"AnimationWindowNewAnimation",         "New Animation"},
    {"AnimationWindowSave",                 "Save"},
    {"AnimationWindowAnimationMenu",        "Animation Menu"},
    {"AnimationWindowCopy",                 "Copy"},
    {"AnimationWindowPaste",                "Paste"},
    {"AnimationWindowOpenAnimation",        "Folder Open"},
    {"AnimationWindowFolderOpen",           "Reveal in Assets / Explorer"},
    {"AnimationWindowFrameAll",             "Frame All"},
    {"AnimationWindowFrameSelectedCurves",  "Frame Selected Curves"},
    {"AnimationWindowFitHeight",            "Fit Height"},
    {"AnimationWindowCenterPlayhead",       "Center Playhead"},
    {"AnimationWindowTimelineZoomSlider",   "Timeline Zoom"},
    {"AnimationWindowShowGrid",             "Show Grid"},
    {"AnimationWindowSnapTime",             "Snap Time"},
    {"AnimationWindowSnapModeDropdown",     "Snap Mode"},
    {"AnimationWindowSnapTimeStep",         "Snap Time Step"},
    {"AnimationWindowSnapValue",            "Snap Value"},
    {"AnimationWindowCurveLinear",          "Linear Curve"},
    {"AnimationWindowCurveStepped",         "Stepped Curve"},
    {"AnimationWindowCurveSpline",          "Spline Curve"},
    {"AnimationWindowDefaultTangentDropdown","Default Tangent Mode"},
    {"AnimationWindowSmoothLowpass",        "Smooth (Low-Pass)"},
    {"AnimationWindowSmoothPeak",           "Smooth (Peak-Preserving)"},
    {"AnimationWindowCurveSimplify",        "Simplify Curve"},
    {"AnimationWindowCurveDraw",            "Retime Tool"},
    {"AnimationWindowEaseIn",               "Ease In"},
    {"AnimationWindowEaseEase",             "Ease"},
    {"AnimationWindowEaseOut",              "Ease Out"},
    {"AnimationWindowSelectAllKeys",        "Select All Curve Keys"},
    {"AnimationWindowDeselectKeys",         "Deselect Curve Keys"},
    {"AnimationWindowToggleKeytool",        "Toggle Key Tool"},
    {"AnimationWindowPreviousKeyframe",     "Previous Keyframe"},
    {"AnimationWindowAddKeyframe",          "Insert keyframe at current time"},
    {"AnimationWindowDuplicateKeyframe",    "Duplicate Selected Keyframes"},
    {"AnimationWindowDeleteKeyframe",       "Delete Selected Keyframes"},
    {"AnimationWindowNextKeyframe",         "Next Keyframe"},
    {"AnimationWindowAddMarker",            "Add timeline marker"},
    {"AnimationWindowAddNoise",             "Add Noise"},
    {"AnimationWindowEulerFilter",          "Euler Filter"},
    {"AnimationWindowLattice",              "Lattice"},
    {"AnimationWindowLoop",                 "Loop"},
    {"AnimationWindowSyncPlayhead",         "Sync Playhead"},
    {"AnimationWindowRecord",               "Record"},
    {"AnimationWindowPreviousFrame",        "Previous Frame"},
    {"AnimationWindowNextFrame",            "Next Frame"},
    {"AnimationWindowStop",                 "Stop"},
    {"AnimationWindowPlayBackward",         "Play Backward"},
    {"AnimationWindowPause",                "Pause"},
    {"AnimationWindowPlay",                 "Play"},
};

std::string MakeAnimToolbarVisibilityPrefKey(const char* buttonId)
{
    std::string key = "ui.animwindow.toolbar.hidden.";
    key += buttonId;
    return key;
}

bool IsAnimToolbarButtonHiddenInPrefs(const char* buttonId)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    bool hidden = false;
    prefs.TryGetBool(MakeAnimToolbarVisibilityPrefKey(buttonId), hidden);
    return hidden;
}

void SetAnimToolbarButtonHiddenInPrefs(const char* buttonId, bool hidden)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(MakeAnimToolbarVisibilityPrefKey(buttonId), hidden);
    (void)prefs.Save(&err);
}

void ApplyAnimToolbarHiddenClass(UIElement* el, bool hidden)
{
    if (!el)
        return;
    if (hidden)
    {
        if (!el->HasClass("hidden"))
            el->AddClass("hidden");
    }
    else if (el->HasClass("hidden"))
    {
        el->RemoveClass("hidden");
    }
    el->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}
} // namespace

void AnimationWindowPanel::WireToolbarRightClickToggles()
{
    if (m_ToolbarRightClickWired || !m_Toolbar)
        return;

    // Apply persisted visibility before wiring (so hidden buttons start hidden).
    for (const auto& entry : kAnimToolbarToggles)
        ApplyAnimToolbarHiddenClass(m_Toolbar->FindById(entry.id),
                                    IsAnimToolbarButtonHiddenInPrefs(entry.id));

    UIElement* toolbar = m_Toolbar;
    Platform::Window* window = m_Window;
    m_Toolbar->RegisterEventHandler(kEventMouseUp, [this, toolbar, window](UIEvent& e)
    {
        if (e.Button != 1)
            return;
        if (!window)
            return;

        // Right-click on a .toolbar-gap: show a small "Remove Gap" menu.
        Button* gapButton = nullptr;
        for (UIElement* node = e.Target; node && node != toolbar; node = node->GetParent())
        {
            if (auto* btn = dynamic_cast<Button*>(node))
            {
                if (btn->HasClass("toolbar-gap")) { gapButton = btn; break; }
                return; // clicked a non-gap button — do nothing
            }
        }

        if (gapButton)
        {
            auto menu = CreateContextMenu();
            if (!menu) return;
            menu->AddItem(0, "Remove Gap", 1, MenuItemFlag_None);
            menu->SetItemIcon(1, EditorIcons::kTrash);
            Button* gapPtr = gapButton;
            menu->SetCommandHandler([this, gapPtr](uint32_t cmd)
            {
                if (cmd == 1) RemoveToolbarGap(gapPtr);
            });
            menu->Show(window, static_cast<int>(e.X), static_cast<int>(e.Y));
            e.Stop();
            return;
        }

        // Empty toolbar background: visibility toggles + "Add Gap".
        for (UIElement* node = e.Target; node && node != toolbar; node = node->GetParent())
        {
            if (node->HasClass("animationwindow-toolbar-group"))
                return;
        }

        auto menu = CreateContextMenu();
        if (!menu)
            return;

        for (uint32_t i = 0; i < std::size(kAnimToolbarToggles); ++i)
        {
            const auto& entry = kAnimToolbarToggles[i];
            const bool hidden = IsAnimToolbarButtonHiddenInPrefs(entry.id);
            const uint32_t cmd = i + 1;
            menu->AddItem(0, entry.title, cmd, hidden ? MenuItemFlag_None : MenuItemFlag_Checked);
            menu->SetItemIcon(cmd, EditorIcons::kEye);
        }

        const uint32_t kCmdAddGap = static_cast<uint32_t>(std::size(kAnimToolbarToggles)) + 1u;
        menu->AddSeparator(0);
        menu->AddItem(0, "Add Gap", kCmdAddGap, MenuItemFlag_None);
        menu->SetItemIcon(kCmdAddGap, EditorIcons::kPlus);

        const uint32_t kCmdThicknessSubmenu = menu->AddSubMenu(0, "Curve Thickness");
        menu->SetSubMenuIcon(kCmdThicknessSubmenu, EditorIcons::kSplineCurve);
        const uint32_t kCmdThickness1 = kCmdAddGap + 1;
        const uint32_t kCmdThickness2 = kCmdAddGap + 2;
        const uint32_t kCmdThickness3 = kCmdAddGap + 3;
        const uint32_t kCmdThickness4 = kCmdAddGap + 4;
        menu->AddItem(kCmdThicknessSubmenu, "1 px", kCmdThickness1, MenuItemFlag_None);
        menu->AddItem(kCmdThicknessSubmenu, "2 px", kCmdThickness2, MenuItemFlag_None);
        menu->AddItem(kCmdThicknessSubmenu, "3 px", kCmdThickness3, MenuItemFlag_None);
        menu->AddItem(kCmdThicknessSubmenu, "4 px", kCmdThickness4, MenuItemFlag_None);

        const uint32_t kCmdSettings = kCmdAddGap + 10;
        const uint32_t kCmdResetLayout = kCmdAddGap + 11;
        menu->AddSeparator(0);
        menu->AddItem(0, "Reset Icon Layout to Default", kCmdResetLayout, MenuItemFlag_None);
        menu->SetItemIcon(kCmdResetLayout, EditorIcons::kReset);
        menu->AddItem(0, "Animation Settings...", kCmdSettings, MenuItemFlag_None);
        menu->SetItemIcon(kCmdSettings, EditorIcons::kSettings);

        const float clickX = e.X;
        menu->SetCommandHandler([this, toolbar, clickX](uint32_t cmd)
        {
            if (cmd == 0) return;
            if (cmd == kCmdSettings)
            {
                if (m_OnOpenSettings) m_OnOpenSettings();
                return;
            }
            if (cmd == kCmdResetLayout)
            {
                if (m_ToolbarDragDrop)
                {
                    std::vector<Editor::ToolbarDragDrop::ContainerConfig> cfg;
                    for (const auto& [cssClass, key] : AnimToolbarContainerOrderKeys())
                        cfg.push_back({cssClass, key});
                    m_ToolbarDragDrop->ResetButtonOrder(toolbar, cfg);
                }
                return;
            }
            if (cmd >= kCmdThickness1 && cmd <= kCmdThickness4)
            {
                const float w = static_cast<float>(cmd - kCmdThickness1 + 1);
                if (m_CurvesGraphView) m_CurvesGraphView->SetCurveLineWidth(w);
                return;
            }
            if (cmd == kCmdAddGap)
            {
                // Pick the container nearest to the click X (default to first
                // group if none is laid out yet). Append to its end — user can
                // drag the gap to its desired final position.
                UIElement* best = nullptr;
                float bestDist = std::numeric_limits<float>::infinity();
                for (const auto& [cssClass, key] : AnimToolbarContainerOrderKeys())
                {
                    UIElement* c = FindAnimToolbarContainerByClass(toolbar, cssClass);
                    if (!c) continue;
                    const float cx = c->GetLayoutX();
                    const float cw = c->GetLayoutWidth();
                    const float center = cx + cw * 0.5f;
                    const float dist = std::abs(clickX - center);
                    if (dist < bestDist) { bestDist = dist; best = c; }
                }
                if (best) CreateToolbarGap(best, -1);
                return;
            }
            if (cmd > std::size(kAnimToolbarToggles) || cmd < 1) return;
            const auto& entry = kAnimToolbarToggles[cmd - 1];
            const bool nowHidden = !IsAnimToolbarButtonHiddenInPrefs(entry.id);
            SetAnimToolbarButtonHiddenInPrefs(entry.id, nowHidden);
            ApplyAnimToolbarHiddenClass(toolbar->FindById(entry.id), nowHidden);
        });

        menu->Show(window, static_cast<int>(e.X), static_cast<int>(e.Y));
        e.Stop();
    });

    m_ToolbarRightClickWired = true;
}

void AnimationWindowPanel::WireMarkersElementCallbacks()
{
    if (!m_MarkersElement)
        return;
    m_MarkersElement->SetOnMarkerTimeChanged([this](size_t index, float newTime)
    {
        if (index >= m_MarkerTimes.size())
            return;
        (void)ExecutePanelEditWithUndo("Move Animation Marker",
                                       [this, index, newTime]()
                                       {
                                           float snappedTime = std::round(newTime * m_TimelineState.fps) / m_TimelineState.fps;
                                           m_MarkerTimes[index] = std::clamp(snappedTime, m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
        if (m_MarkersElement)
            m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
                                           return true;
                                       },
                                       true);
    });
    m_MarkersElement->SetOnMarkerRemoved([this](size_t index)
    {
        if (index >= m_MarkerTimes.size())
            return;
        (void)ExecutePanelEditWithUndo("Remove Animation Marker",
                                       [this, index]()
                                       {
                                           m_MarkerTimes.erase(m_MarkerTimes.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_MarkersElement)
            m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
                                           return true;
                                       },
                                       false);
    });
}

void AnimationWindowPanel::WireAnimationControls()
{
    if (auto* newBtn = dynamic_cast<Button*>(FindById("AnimationWindowNewAnimation")))
    {
        newBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                          {
                              // Dispatch by active view, mirroring the Open button:
                              //   TimeComposite → new .timeline   (composite multi-track)
                              //   ClipEditor    → new .clipset    (lane-based clip arrangement)
                              //   DopeSheet / Curves → new .anim  (animation clip)
                              auto& am = EngineCore::GetInstance().GetAssetManager();
                              const std::filesystem::path assets = am.GetAssetRoot() / "Assets";

                              if (m_ActiveView == ActiveView::TimeComposite)
                              {
                                  std::filesystem::path savePath = Platform::SaveFile(
                                      assets / "NewTimeline.timeline", "Timeline", "*.timeline");
                                  if (savePath.empty()) return;
                                  if (savePath.extension() != ".timeline")
                                      savePath += ".timeline";
                                  CreateNewTimelineAtPath(savePath);
                                  return;
                              }
                              if (m_ActiveView == ActiveView::ClipEditor)
                              {
                                  std::filesystem::path savePath = Platform::SaveFile(
                                      assets / "NewClipSet.clipset", "Clip Set", "*.clipset");
                                  if (savePath.empty()) return;
                                  if (savePath.extension() != ".clipset")
                                      savePath += ".clipset";
                                  CreateNewClipSetAtPath(savePath);
                                  return;
                              }

                              std::filesystem::path savePath = Platform::SaveFile(
                                  assets / "NewAnimation.anim", "Animation Asset", "*.anim");
                              if (savePath.empty()) return;
                              if (savePath.extension() != ".anim")
                                  savePath += ".anim";
                              CreateNewAnimationAtPath(savePath);
                          });
    }
    if (auto* saveBtn = dynamic_cast<Button*>(FindById("AnimationWindowSave")))
    {
        saveBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                            {
                                auto& am = EngineCore::GetInstance().GetAssetManager();
                                const std::filesystem::path assets = am.GetAssetRoot() / "Assets";

                                if (m_ActiveView == ActiveView::TimeComposite)
                                {
                                    if (!m_CurrentTimelineAsset)
                                    {
                                        std::filesystem::path savePath = Platform::SaveFile(
                                            assets / "NewTimeline.timeline", "Timeline", "*.timeline");
                                        if (savePath.empty()) return;
                                        if (savePath.extension() != ".timeline")
                                            savePath += ".timeline";
                                        CreateNewTimelineAtPath(savePath);
                                        return;
                                    }
                                    (void)SaveCurrentTimeline();
                                    return;
                                }

                                if (m_ActiveView == ActiveView::ClipEditor)
                                {
                                    if (!m_CurrentClipSetAsset)
                                    {
                                        std::filesystem::path savePath = Platform::SaveFile(
                                            assets / "NewClipSet.clipset", "Clip Set", "*.clipset");
                                        if (savePath.empty()) return;
                                        if (savePath.extension() != ".clipset")
                                            savePath += ".clipset";
                                        CreateNewClipSetAtPath(savePath);
                                        return;
                                    }
                                    (void)SaveCurrentClipSet();
                                    return;
                                }

                                if (!m_CurrentClipAsset)
                                {
                                    // No clip loaded - trigger "create new" flow same as + button
                                    std::filesystem::path initialPath = assets / "NewAnimation.anim";
                                    std::filesystem::path savePath = Platform::SaveFile(
                                        initialPath,
                                        "Animation Asset",
                                        "*.anim");
                                    if (savePath.empty()) return;

                                    if (savePath.extension() != ".anim")
                                        savePath += ".anim";

                                    // Native save dialog already confirmed overwrite if file exists
                                    CreateNewAnimationAtPath(savePath);
                                    return;
                                }
                                (void)SaveCurrentClip();
                          });
    }
    if (auto* animMenuBtn = dynamic_cast<Button*>(FindById("AnimationWindowAnimationMenu")))
    {
        animMenuBtn->SetText(GetPanelBaseTitle());
        animMenuBtn->RegisterEventHandler(kEventButtonClick, [this, animMenuBtn](UIEvent&)
        {
            if (!m_Window) return;
            if (!m_AnimationMenuContextMenu)
            {
                m_AnimationMenuContextMenu = CreateContextMenu();
                if (!m_AnimationMenuContextMenu) return;
                m_AnimationMenuContextMenu->SetCommandHandler([this](uint32_t cmd)
                {
                    enum : uint32_t
                    {
                        kCmdNew = 1,
                        kCmdOpenAnimationPanel,
                        kCmdOpenTimelinePanel,
                        kCmdOpenClipEditorPanel,
                        kCmdManage,
                        kCmdDuplicate,
                        kCmdRename,
                        kCmdTransitions,
                        kCmdOpenInspector,
                        kCmdRemove
                    };
                    if (cmd == kCmdNew)
                    {
                        if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowNewAnimation")))
                            btn->TriggerClick();
                        return;
                    }
                    if (cmd == kCmdOpenTimelinePanel)
                    {
                        if (m_OnOpenTimelinePanel)
                            m_OnOpenTimelinePanel();
                        return;
                    }
                    if (cmd == kCmdOpenAnimationPanel)
                    {
                        if (m_OnOpenAnimationPanel)
                            m_OnOpenAnimationPanel();
                        return;
                    }
                    if (cmd == kCmdOpenClipEditorPanel)
                    {
                        if (m_OnOpenClipEditorPanel)
                            m_OnOpenClipEditorPanel();
                        return;
                    }
                    if (cmd == kCmdManage)
                    {
                        ShowAnimationManagerInspector();
                        return;
                    }
                    if (cmd == kCmdDuplicate)
                    {
                        DuplicateCurrentAnimationAsset();
                        return;
                    }
                    if (cmd == kCmdRename)
                    {
                        RenameCurrentAnimationAsset();
                        return;
                    }
                    if (cmd == kCmdTransitions)
                    {
                        ShowAnimationTransitionsInspector();
                        return;
                    }
                    if (cmd == kCmdOpenInspector)
                    {
                        const std::filesystem::path inspectPath = GetCurrentMenuAnimationAssetPath();
                        if (m_OnRevealInAssetsPanel && !inspectPath.empty())
                            m_OnRevealInAssetsPanel(inspectPath);
                        return;
                    }
                    if (cmd == kCmdRemove)
                    {
                        RemoveCurrentAnimationAsset();
                        return;
                    }
                });
            }
            enum : uint32_t
            {
                kCmdNew = 1,
                kCmdOpenAnimationPanel,
                kCmdOpenTimelinePanel,
                kCmdOpenClipEditorPanel,
                kCmdManage,
                kCmdDuplicate,
                kCmdRename,
                kCmdTransitions,
                kCmdOpenInspector,
                kCmdRemove
            };
            const std::filesystem::path inspectPath = GetCurrentMenuAnimationAssetPath();
            std::error_code ec;
            const bool hasAssetFile = !inspectPath.empty() && std::filesystem::is_regular_file(inspectPath, ec);
            m_AnimationMenuContextMenu->Clear();
            m_AnimationMenuContextMenu->AddItem(0, "New...", kCmdNew);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdNew, EditorIcons::kPlus);
            m_AnimationMenuContextMenu->AddSeparator(0);
            bool hasPanelSwitchAction = false;
            if (m_PanelKind != PanelKind::Animation)
            {
                m_AnimationMenuContextMenu->AddItem(0, "Open Animation Panel", kCmdOpenAnimationPanel,
                                                    m_OnOpenAnimationPanel ? MenuItemFlag_None : MenuItemFlag_Disabled);
                m_AnimationMenuContextMenu->SetItemIcon(kCmdOpenAnimationPanel, EditorIcons::kFolderOpen);
                hasPanelSwitchAction = true;
            }
            if (m_PanelKind != PanelKind::Timeline)
            {
                m_AnimationMenuContextMenu->AddItem(0, "Open Timeline Panel", kCmdOpenTimelinePanel,
                                                    m_OnOpenTimelinePanel ? MenuItemFlag_None : MenuItemFlag_Disabled);
                m_AnimationMenuContextMenu->SetItemIcon(kCmdOpenTimelinePanel, EditorIcons::kFolderOpen);
                hasPanelSwitchAction = true;
            }
            if (m_PanelKind != PanelKind::ClipEditor)
            {
                m_AnimationMenuContextMenu->AddItem(0, "Open Clip Editor Panel", kCmdOpenClipEditorPanel,
                                                    m_OnOpenClipEditorPanel ? MenuItemFlag_None : MenuItemFlag_Disabled);
                m_AnimationMenuContextMenu->SetItemIcon(kCmdOpenClipEditorPanel, EditorIcons::kFolderOpen);
                hasPanelSwitchAction = true;
            }
            if (hasPanelSwitchAction)
                m_AnimationMenuContextMenu->AddSeparator(0);
            m_AnimationMenuContextMenu->AddItem(0, "Manage Animations...", kCmdManage);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdManage, EditorIcons::kSettings);
            m_AnimationMenuContextMenu->AddSeparator(0);
            m_AnimationMenuContextMenu->AddItem(0, "Duplicate...", kCmdDuplicate, hasAssetFile ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdDuplicate, EditorIcons::kCopy);
            m_AnimationMenuContextMenu->AddItem(0, "Rename...", kCmdRename, hasAssetFile ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdRename, EditorIcons::kBrush);
            m_AnimationMenuContextMenu->AddItem(0, "Edit Transitions...", kCmdTransitions);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdTransitions, EditorIcons::kBrush);
            m_AnimationMenuContextMenu->AddItem(0, "Open in Inspector", kCmdOpenInspector,
                                                (!inspectPath.empty() && m_OnRevealInAssetsPanel) ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdOpenInspector, EditorIcons::kFolderOpen);
            m_AnimationMenuContextMenu->AddSeparator(0);
            m_AnimationMenuContextMenu->AddItem(0, "Remove", kCmdRemove, hasAssetFile ? MenuItemFlag_None : MenuItemFlag_Disabled);
            m_AnimationMenuContextMenu->SetItemIcon(kCmdRemove, EditorIcons::kTrash);
            const float x = animMenuBtn->GetLayoutX();
            const float y = animMenuBtn->GetLayoutY() + animMenuBtn->GetLayoutHeight();
            ShowContextMenuKeepingFocus(m_AnimationMenuContextMenu.get(), static_cast<int>(x), static_cast<int>(y));
        });
    }
    if (auto* playBtn = dynamic_cast<Button*>(FindById("AnimationWindowPlay")))
    {
        playBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                            {
                                const bool playingForward = m_TimelineState.playing && !m_TimelineState.reverse;
                                if (playingForward)
                                {
                                    m_TimelineState.paused = true;
                                    m_TimelineState.playing = false;
                                }
                                else
                                {
                                    m_TimelineState.playing = true;
                                    m_TimelineState.paused = false;
                                    m_TimelineState.reverse = false;
                                }
                                UpdateTimelineRulerAndLabels();
                                UpdateCurrentFrameLabel();
                                UpdatePlayButtonState();
                                UpdatePlayBackwardButtonState();
                            });
    }
    if (auto* playBackBtn = dynamic_cast<Button*>(FindById("AnimationWindowPlayBackward")))
    {
        playBackBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                                {
                                    const bool playingReverse = m_TimelineState.playing && m_TimelineState.reverse;
                                    if (playingReverse)
                                    {
                                        m_TimelineState.paused = true;
                                        m_TimelineState.playing = false;
                                    }
                                    else
                                    {
                                        m_TimelineState.playing = true;
                                        m_TimelineState.paused = false;
                                        m_TimelineState.reverse = true;
                                    }
                                    UpdateTimelineRulerAndLabels();
                                    UpdateCurrentFrameLabel();
                                    UpdatePlayButtonState();
                                    UpdatePlayBackwardButtonState();
                                });
    }
    if (auto* stopBtn = dynamic_cast<Button*>(FindById("AnimationWindowStop")))
    {
        stopBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                            {
                                m_TimelineState.playing = false;
                                m_TimelineState.paused = false;
                                m_TimelineState.reverse = false;
                                m_TimelineState.currentTime = 0.0f;
                                // Pan visible range so playhead (0) is in view
                                const float duration = m_TimelineState.viewEnd - m_TimelineState.viewStart;
                                m_TimelineState.viewStart = 0.0f;
                                m_TimelineState.viewEnd = std::max(duration, 0.1f);
                                UpdateTimelineRulerAndLabels();
                                UpdateCurrentFrameLabel();
                                UpdatePlayButtonState();
                                UpdatePlayBackwardButtonState();
                                if (m_DopeSheetView)
                                    m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
                                if (m_CurvesGraphView)
                                    m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
                                if (m_TimeCompositeView)
                                    m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
                                if (m_LaneClipEditorView)
                                    m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
                            });
    }
    if (auto* pauseBtn = dynamic_cast<Button*>(FindById("AnimationWindowPause")))
    {
        pauseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                             {
                                 m_TimelineState.paused = true;
                                 m_TimelineState.playing = false;
                                 UpdateTimelineRulerAndLabels();
                                 UpdateCurrentFrameLabel();
                                 UpdatePlayButtonState();
                                 UpdatePlayBackwardButtonState();
                             });
    }
    if (auto* loopBtn = dynamic_cast<Button*>(FindById("AnimationWindowLoop")))
    {
        loopBtn->SetText(""); // icon-only, same as play/stop/pause
        loopBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                            {
                                m_TimelineState.loop = !m_TimelineState.loop;
                                UpdateLoopButtonState();
                            });
        UpdateLoopButtonState();
    }
    if (auto* syncPlayheadBtn = dynamic_cast<Button*>(FindById("AnimationWindowSyncPlayhead")))
    {
        syncPlayheadBtn->SetText("");
        syncPlayheadBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                                   {
                                       m_ScrollWithPlayhead = !m_ScrollWithPlayhead;
                                       UpdateSyncPlayheadButtonState();
                                   });
        UpdateSyncPlayheadButtonState();
    }
    if (auto* recordBtn = dynamic_cast<Button*>(FindById("AnimationWindowRecord")))
    {
        recordBtn->SetText("");
        recordBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                             {
                                 m_Recording = !m_Recording;
                                 if (m_Recording)
                                     InsertKeyAtCurrentTime();
                                 UpdateRecordButtonState();
                             });
        UpdateRecordButtonState();
    }
    auto jumpToTimelineTime = [this](float time)
    {
        m_TimelineState.currentTime = std::clamp(time, m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
        if (m_TimelineState.fps > 0.0f)
            m_TimelineState.currentTime = std::round(m_TimelineState.currentTime * m_TimelineState.fps) / m_TimelineState.fps;
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
        UpdatePreviewBinding();
        if (m_Recording)
            InsertKeyAtCurrentTime();
    };
    if (auto* goStartBtn = dynamic_cast<Button*>(FindById("AnimationWindowGoToStart")))
        goStartBtn->RegisterEventHandler(kEventButtonClick, [jumpToTimelineTime, this](UIEvent&) { jumpToTimelineTime(m_TimelineState.rangeStart); });
    if (auto* goEndBtn = dynamic_cast<Button*>(FindById("AnimationWindowGoToEnd")))
        goEndBtn->RegisterEventHandler(kEventButtonClick, [jumpToTimelineTime, this](UIEvent&) { jumpToTimelineTime(m_TimelineState.rangeEnd); });
    auto stepTimelineFrame = [jumpToTimelineTime, this](int direction)
    {
        const float fps = std::max(m_TimelineState.fps, 1.0f);
        jumpToTimelineTime(m_TimelineState.currentTime + static_cast<float>(direction) / fps);
    };
    if (auto* prevFrameBtn = dynamic_cast<Button*>(FindById("AnimationWindowPreviousFrame")))
        prevFrameBtn->RegisterEventHandler(kEventButtonClick, [stepTimelineFrame](UIEvent&) { stepTimelineFrame(-1); });
    if (auto* nextFrameBtn = dynamic_cast<Button*>(FindById("AnimationWindowNextFrame")))
        nextFrameBtn->RegisterEventHandler(kEventButtonClick, [stepTimelineFrame](UIEvent&) { stepTimelineFrame(1); });

    m_PreviewToggleBtn = dynamic_cast<Button*>(FindById("AnimationWindowPreviewToggle"));
    if (m_PreviewToggleBtn)
    {
        if (m_PreviewEnabled) m_PreviewToggleBtn->AddClass("active");
        m_PreviewToggleBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetPreviewEnabled(!m_PreviewEnabled); });
    }

    m_ClipDropdown = dynamic_cast<Dropdown*>(FindById("AnimationWindowClipDropdown"));
    if (m_ClipDropdown)
    {
        m_ClipDropdown->SetOptions({{"", "No Clip"}}, 0);
        m_ClipDropdown->SetOnValueChanged([this](const std::string& value)
        {
            if (value.empty()) return;
            // Values are tagged "anim:N", "comp:T:C", or "lane:L:C" so the handler
            // can route to the right per-mode action regardless of which view set them.
            auto parseTwoIndices = [](const std::string& tail, size_t& a, size_t& b) -> bool
            {
                const size_t sep = tail.find(':');
                if (sep == std::string::npos) return false;
                try
                {
                    a = static_cast<size_t>(std::stoull(tail.substr(0, sep)));
                    b = static_cast<size_t>(std::stoull(tail.substr(sep + 1)));
                    return true;
                }
                catch (...) { return false; }
            };
            if (value.rfind("anim:", 0) == 0)
            {
                if (m_CurrentClipPath.empty()) return;
                try
                {
                    const uint32 idx = static_cast<uint32>(std::stoul(value.substr(5)));
                    if (m_CurrentClipAsset && idx == m_CurrentClipAsset->GetSelectedAnimationIndex())
                        return;
                    OpenAnimation(m_CurrentClipPath, idx);
                }
                catch (...) {}
                return;
            }
            if (value.rfind("comp:", 0) == 0)
            {
                size_t t = 0, c = 0;
                if (parseTwoIndices(value.substr(5), t, c))
                    SelectCompositeClip(t, c);
                return;
            }
            if (value.rfind("lane:", 0) == 0)
            {
                size_t l = 0, c = 0;
                if (parseTwoIndices(value.substr(5), l, c))
                    SelectLaneClip(l, c);
                return;
            }
        });
        RefreshClipDropdown();
    }
    m_ViewDropdown = dynamic_cast<Dropdown*>(FindById("AnimationWindowViewDropdown"));
    if (m_ViewDropdown)
    {
        m_ViewDropdown->AddClass("hidden");
        m_ViewDropdown->SetOnValueChanged([this](const std::string& value)
        {
            if      (value == "dope_sheet") SetActiveView(ActiveView::DopeSheet);
            else if (value == "curves")     SetActiveView(ActiveView::Curves);
            else if (value == "composite")  SetActiveView(ActiveView::TimeComposite);
            else if (value == "clip_editor")SetActiveView(ActiveView::ClipEditor);
        });
    }
    m_ViewButtons = FindById("AnimationWindowViewButtons");
    m_DopeSheetViewButton = dynamic_cast<Button*>(FindById("AnimationWindowDopeSheetView"));
    m_CurvesViewButton = dynamic_cast<Button*>(FindById("AnimationWindowCurvesView"));
    if (m_ViewButtons)
    {
        if (m_PanelKind == PanelKind::Animation)
            m_ViewButtons->RemoveClass("hidden");
        else
            m_ViewButtons->AddClass("hidden");
    }
    if (m_DopeSheetViewButton)
        m_DopeSheetViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetActiveView(ActiveView::DopeSheet); });
    if (m_CurvesViewButton)
        m_CurvesViewButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { SetActiveView(ActiveView::Curves); });
    SetActiveView(m_ActiveView);
    UpdateInterpolationButtonState();

    auto setChannelInterp = [this](AnimInterp interp)
    {
        if (!EnsureEditableClip() || !m_CurrentClipAsset) return;

        // Collect channels from the active view's selected keys only.
        // Mixing both views risks applying to channels the user never intended to touch.
        std::unordered_set<int> targetChannels;
        const bool curvesActive = (m_ActiveView == ActiveView::Curves);
        if (curvesActive && m_CurvesGraphView)
        {
            for (const auto& sel : m_CurvesGraphView->GetSelectedKeyframes())
                if (sel.Channel >= 0) targetChannels.insert(sel.Channel);
        }
        if (!curvesActive && m_DopeSheetView)
        {
            for (const auto& sel : m_DopeSheetView->GetSelectedKeyframes())
                if (sel.Channel >= 0) targetChannels.insert(sel.Channel);
        }
        // Fall back: try the other view, then the active channel.
        if (targetChannels.empty() && m_CurvesGraphView)
        {
            for (const auto& sel : m_CurvesGraphView->GetSelectedKeyframes())
                if (sel.Channel >= 0) targetChannels.insert(sel.Channel);
        }
        if (targetChannels.empty() && m_DopeSheetView)
        {
            for (const auto& sel : m_DopeSheetView->GetSelectedKeyframes())
                if (sel.Channel >= 0) targetChannels.insert(sel.Channel);
        }
        if (targetChannels.empty())
        {
            if (m_SelectedChannel < 0) return;
            targetChannels.insert(m_SelectedChannel);
        }

        // Collect per-channel selected key times so we can do per-keyframe interp.
        // Use the same logic as targetChannels to ensure they match.
        std::unordered_map<int, std::unordered_set<float>> selectedKeysByChannel;
        auto collectKeys = [&](const auto& selKeys)
        {
            for (const auto& sk : selKeys)
                if (sk.Channel >= 0 && targetChannels.count(sk.Channel))
                    selectedKeysByChannel[sk.Channel].insert(sk.KeyTime);
        };
        
        // Collect from active view first
        if (curvesActive && m_CurvesGraphView) collectKeys(m_CurvesGraphView->GetSelectedKeyframes());
        else if (!curvesActive && m_DopeSheetView) collectKeys(m_DopeSheetView->GetSelectedKeyframes());
        
        // If no keys collected, try the other view (matches targetChannels fallback logic)
        if (selectedKeysByChannel.empty() && m_CurvesGraphView)
            collectKeys(m_CurvesGraphView->GetSelectedKeyframes());
        if (selectedKeysByChannel.empty() && m_DopeSheetView)
            collectKeys(m_DopeSheetView->GetSelectedKeyframes());
        
        const bool hasKeySelection = !selectedKeysByChannel.empty();

        (void)ExecuteClipEditWithUndo("Set Animation Interpolation",
            [this, interp, targetChannels, selectedKeysByChannel, hasKeySelection]()
            {
                bool changed = false;

                auto getComp = [](const AnimKeyframe& k, AnimPath path, uint32 c) -> float {
                    if (path == AnimPath::Translation) return c < 3u ? k.translation[c] : 0.0f;
                    if (path == AnimPath::Rotation)    return c < 4u ? k.rotation[c]    : 0.0f;
                    return c < 3u ? k.scale[c] : 0.0f;
                };

                // Linear tangents for a key at ki (in/out separate slopes, matching linear segments).
                auto computeLinearTangents = [&](const AnimChannel& ch, size_t ki, uint32 c,
                                                 float& outInT, float& outOutT)
                {
                    const size_t nk = ch.keys.size();
                    outInT = 0.0f; outOutT = 0.0f;
                    if (ki > 0)
                    {
                        const float dt = ch.keys[ki].time - ch.keys[ki - 1].time;
                        if (dt > 0.0001f)
                            outInT = (getComp(ch.keys[ki], ch.path, c) - getComp(ch.keys[ki - 1], ch.path, c)) / dt;
                    }
                    if (ki + 1 < nk)
                    {
                        const float dt = ch.keys[ki + 1].time - ch.keys[ki].time;
                        if (dt > 0.0001f)
                            outOutT = (getComp(ch.keys[ki + 1], ch.path, c) - getComp(ch.keys[ki], ch.path, c)) / dt;
                    }
                    if (ki == 0)        outInT  = outOutT;
                    if (ki + 1 == nk)  outOutT = outInT;
                };

                // Catmull-Rom tangent for a key at ki (inT == outT, smooth through keyframe).
                auto computeSplineTangent = [&](const AnimChannel& ch, size_t ki, uint32 c) -> float
                {
                    const size_t nk = ch.keys.size();
                    if (nk < 2) return 0.0f;
                    if (ki == 0)
                    {
                        const float dt = ch.keys[1].time - ch.keys[0].time;
                        return dt > 0.0001f ? (getComp(ch.keys[1], ch.path, c) - getComp(ch.keys[0], ch.path, c)) / dt : 0.0f;
                    }
                    if (ki + 1 == nk)
                    {
                        const float dt = ch.keys[ki].time - ch.keys[ki - 1].time;
                        return dt > 0.0001f ? (getComp(ch.keys[ki], ch.path, c) - getComp(ch.keys[ki - 1], ch.path, c)) / dt : 0.0f;
                    }
                    const float dt = ch.keys[ki + 1].time - ch.keys[ki - 1].time;
                    return dt > 0.0001f ? (getComp(ch.keys[ki + 1], ch.path, c) - getComp(ch.keys[ki - 1], ch.path, c)) / dt : 0.0f;
                };

                if (!hasKeySelection)
                {
                    // No keys selected — apply interpolation to entire channel.
                    for (int ch : targetChannels)
                        changed |= m_CurrentClipAsset->SetChannelInterpolation(static_cast<size_t>(ch), interp);

                    // For CubicSpline, also initialize all keyframe tangents.
                    if (interp == AnimInterp::CubicSpline)
                    {
                        for (int ch : targetChannels)
                        {
                            const size_t chIdx = static_cast<size_t>(ch);
                            const AnimChannel& channel = m_CurrentClipAsset->GetChannels()[chIdx];
                            const uint32 compCount = (channel.path == AnimPath::Rotation) ? 4u : 3u;
                            for (size_t ki = 0; ki < channel.keys.size(); ++ki)
                            {
                                for (uint32 c = 0; c < compCount; ++c)
                                {
                                    const float t = computeSplineTangent(channel, ki, c);
                                    constexpr float kW = 1.0f / 3.0f;
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, channel.keys[ki].time, c, true,  t, kW);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, channel.keys[ki].time, c, false, t, kW);
                                }
                            }
                        }
                    }
                }
                else
                {
                    // Keys selected — apply interpolation only to selected keys using segment overrides.
                    // This allows mixing interpolation types within the same channel.
                    constexpr float kW         = 1.0f / 3.0f;
                    constexpr float kStepWeight = 0.05f; // matches kMinHandleWeight in CurvesGraphView

                    for (int ch : targetChannels)
                    {
                        const size_t chIdx = static_cast<size_t>(ch);
                        const auto selIt = selectedKeysByChannel.find(ch);
                        const bool hasSel = (selIt != selectedKeysByChannel.end() && !selIt->second.empty());
                        if (!hasSel) continue;

                        const AnimChannel& channel = m_CurrentClipAsset->GetChannels()[chIdx];
                        const uint32 compCount = (channel.path == AnimPath::Rotation) ? 4u : 3u;

                        // For CubicSpline, ensure the channel is in spline mode.
                        if (interp == AnimInterp::CubicSpline && channel.interp != AnimInterp::CubicSpline)
                            changed |= m_CurrentClipAsset->SetChannelInterpolation(chIdx, AnimInterp::CubicSpline);

                        for (float keyTime : selIt->second)
                        {
                            // Find the key index for this time.
                            const size_t ki = FindKeyIndex(channel.keys, keyTime);
                            if (ki >= channel.keys.size()) continue;

                            // Apply per-keyframe segment interpolation.
                            changed |= m_CurrentClipAsset->SetKeyframeSegmentInterp(chIdx, keyTime, interp);

                            // Also set appropriate tangents based on interpolation type.
                            for (uint32 c = 0; c < compCount; ++c)
                            {
                                if (interp == AnimInterp::CubicSpline)
                                {
                                    const float t = computeSplineTangent(channel, ki, c);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, true,  t, kW);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, false, t, kW);
                                }
                                else if (interp == AnimInterp::Linear)
                                {
                                    float inT = 0.0f, outT = 0.0f;
                                    computeLinearTangents(channel, ki, c, inT, outT);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, true,  inT,  kW);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, false, outT, kW);
                                }
                                else if (interp == AnimInterp::Step)
                                {
                                    // Approximate step: outgoing handle is flat with minimum weight
                                    // so the curve stays constant until just before the next key.
                                    float inT = 0.0f, unused = 0.0f;
                                    computeLinearTangents(channel, ki, c, inT, unused);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, true,  inT,  kW);
                                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, keyTime, c, false, 0.0f, kStepWeight);
                                }
                            }
                        }
                    }
                }
                return changed;
            });
    };

    // Helper: extract per-component value from a keyframe.
    auto keyCompValue = [](const AnimKeyframe& key, AnimPath path, uint32 comp) -> float
    {
        if (path == AnimPath::Translation) return comp < 3u ? key.translation[comp] : 0.0f;
        if (path == AnimPath::Rotation)   return comp < 4u ? key.rotation[comp]    : 0.0f;
        return comp < 3u ? key.scale[comp] : 0.0f;
    };

    // Helper: push time+value bounds of a keyframe into running min/max.
    auto accumKey = [keyCompValue](const AnimKeyframe& key, AnimPath path, uint32 comp,
                                   float& tMin, float& tMax, float& vMin, float& vMax)
    {
        tMin = std::min(tMin, key.time);
        tMax = std::max(tMax, key.time);
        const uint32 maxComp = (path == AnimPath::Rotation) ? 4u : 3u;
        if (comp < maxComp)
        {
            const float v = keyCompValue(key, path, comp);
            vMin = std::min(vMin, v);
            vMax = std::max(vMax, v);
        }
    };

    // Helper: apply computed ranges to all views + timeline.
    auto applyFrameRanges = [this](float tMin, float tMax, float vMin, float vMax, bool applyTime, bool applyValue)
    {
        if (applyTime)
        {
            const float tp = (tMax - tMin) * 0.08f;
            m_TimelineState.viewStart = std::max(0.0f, tMin - tp);
            m_TimelineState.viewEnd   = tMax + tp;
            UpdateTimelineRulerAndLabels();
            UpdateCurrentFrameLabel();
            if (m_DopeSheetView)    m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
            if (m_CurvesGraphView)  m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
            if (m_TimeCompositeView) m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
            if (m_LaneClipEditorView) m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        }
        if (applyValue && m_CurvesGraphView && std::isfinite(vMin) && std::isfinite(vMax))
        {
            if (vMax - vMin < 1e-4f) { vMin -= 1.0f; vMax += 1.0f; }
            const float vp = (vMax - vMin) * 0.12f;
            m_CurvesGraphView->SetValueRange(vMin - vp, vMax + vp);
        }
    };

    // Frame All: fit all visible-channel keyframes into view (both axes).
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowFrameAll")))
        btn->RegisterEventHandler(kEventButtonClick, [this, accumKey, applyFrameRanges](UIEvent&)
        {
            if (!m_CurrentClip || m_VisibleChannels.empty()) { FrameAll(); return; }
            const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
            float tMin =  std::numeric_limits<float>::infinity();
            float tMax = -std::numeric_limits<float>::infinity();
            float vMin =  std::numeric_limits<float>::infinity();
            float vMax = -std::numeric_limits<float>::infinity();
            for (int chIdx : m_VisibleChannels)
            {
                if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
                const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
                for (const AnimKeyframe& key : ch.keys)
                    accumKey(key, ch.path, m_SelectedComponent, tMin, tMax, vMin, vMax);
            }
            if (!std::isfinite(tMin)) { FrameAll(); return; }
            applyFrameRanges(tMin, tMax, vMin, vMax, true, m_ActiveView == ActiveView::Curves);
        });

    // Frame Selected: fit selected keyframes in both axes; fall back to Frame All.
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowFrameSelectedCurves")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (!FrameSelectionBounds())
                FrameSelected();
        });

    // Fit Height: fit value axis to all visible curves, time axis unchanged.
    // Fit Height toggle: a one-press now both fits the value range AND
    // latches into auto-fit mode so subsequent edits / playback keep the
    // curve framed. Toggling off leaves the current range untouched.
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowFitHeight")))
        btn->RegisterEventHandler(kEventButtonClick, [this, accumKey, applyFrameRanges](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            m_AutoFitHeight = !m_AutoFitHeight;
            if (m_AutoFitHeight)
                self.AddClass("active");
            else
                self.RemoveClass("active");

            if (!m_AutoFitHeight) return;
            if (!m_CurvesGraphView || !m_CurrentClip || m_VisibleChannels.empty()) return;
            const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
            float tDummy = 0.0f, vMin = std::numeric_limits<float>::infinity(), vMax = -std::numeric_limits<float>::infinity();
            for (int chIdx : m_VisibleChannels)
            {
                if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
                const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
                for (const AnimKeyframe& key : ch.keys)
                    accumKey(key, ch.path, m_SelectedComponent, tDummy, tDummy, vMin, vMax);
            }
            applyFrameRanges(0, 0, vMin, vMax, false, true);
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSelectAllKeys")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_CurvesGraphView)
                m_CurvesGraphView->SelectAllVisibleKeys();
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowDeselectKeys")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_CurvesGraphView)
                m_CurvesGraphView->ClearSelection();
            if (m_DopeSheetView)
                m_DopeSheetView->ClearSelection();
            UpdateStatsBar();
            UpdateInterpolationButtonState();
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowPreviousKeyframe")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { GoToPreviousKey(); });

    // Add Keyframe button - inserts a keyframe at current time on selected channel
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowAddKeyframe")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            InsertKeyAtCurrentTime();
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowDuplicateKeyframe")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { DuplicateSelectedKey(); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowDeleteKeyframe")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_ActiveView == ActiveView::TimeComposite)
                DeleteSelectedTimelineKeys();
            else
                DeleteSelectedKey();
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowNextKeyframe")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { GoToNextKey(); });

    // Add Marker button - adds a timeline marker at current time
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowAddMarker")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            (void)ExecutePanelEditWithUndo("Add Animation Marker",
                                           [this]()
                                           {
                                               const float t = std::round(m_TimelineState.currentTime * m_TimelineState.fps) / m_TimelineState.fps;
                                               m_MarkerTimes.push_back(t);
                                               std::sort(m_MarkerTimes.begin(), m_MarkerTimes.end());
                                               if (m_MarkersElement)
                                                   m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
                                               return true;
                                           },
                                           false);
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurveLinear")))
        btn->RegisterEventHandler(kEventButtonClick, [setChannelInterp](UIEvent&) { setChannelInterp(AnimInterp::Linear); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurveStepped")))
        btn->RegisterEventHandler(kEventButtonClick, [setChannelInterp](UIEvent&) { setChannelInterp(AnimInterp::Step); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurveSpline")))
        btn->RegisterEventHandler(kEventButtonClick, [setChannelInterp](UIEvent&) { setChannelInterp(AnimInterp::CubicSpline); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSmoothLowpass")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { EnterCurveOptionsMode(CurveOptionsMode::SmoothLowpass); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSmoothPeak")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { EnterCurveOptionsMode(CurveOptionsMode::SmoothPeak); });

    auto applyEaseTangents = [this](bool easeIn, bool easeOut)
    {
        if (!EnsureEditableClip() || !m_CurrentClipAsset) return;

        // Use selected keyframes when available; otherwise apply to the active channel at current time.
        using KeyTarget = std::pair<size_t, float>;
        std::vector<KeyTarget> targets;
        if (m_CurvesGraphView)
            for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
                if (sk.Channel >= 0) targets.push_back({static_cast<size_t>(sk.Channel), sk.KeyTime});
        if (targets.empty() && m_DopeSheetView)
            for (const auto& sk : m_DopeSheetView->GetSelectedKeyframes())
                if (sk.Channel >= 0) targets.push_back({static_cast<size_t>(sk.Channel), sk.KeyTime});
        if (targets.empty() && m_SelectedChannel >= 0)
            targets.push_back({static_cast<size_t>(m_SelectedChannel), m_TimelineState.currentTime});

        if (targets.empty()) return;

        (void)ExecuteClipEditWithUndo("Apply Ease Tangent",
            [this, targets, easeIn, easeOut]()
            {
                bool changed = false;
                for (const auto& [chIdx, t] : targets)
                {
                    if (chIdx >= m_CurrentClipAsset->GetChannels().size()) continue;
                    m_CurrentClipAsset->SetChannelInterpolation(chIdx, AnimInterp::CubicSpline);
                    const AnimChannel& ch = m_CurrentClipAsset->GetChannels()[chIdx];
                    const uint32 compCount = (ch.path == AnimPath::Rotation) ? 4u : 3u;
                    for (uint32 c = 0; c < compCount; ++c)
                    {
                        if (easeIn)  changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, true,  0.0f, 1.0f / 3.0f);
                        if (easeOut) changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, false, 0.0f, 1.0f / 3.0f);
                    }
                }
                return changed;
            });
    };

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowEaseIn")))
        btn->RegisterEventHandler(kEventButtonClick, [applyEaseTangents](UIEvent&) { applyEaseTangents(true, false); });
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowEaseEase")))
        btn->RegisterEventHandler(kEventButtonClick, [applyEaseTangents](UIEvent&) { applyEaseTangents(true, true); });
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowEaseOut")))
        btn->RegisterEventHandler(kEventButtonClick, [applyEaseTangents](UIEvent&) { applyEaseTangents(false, true); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurveSimplify")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { EnterCurveOptionsMode(CurveOptionsMode::Simplify); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowAddNoise")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { EnterCurveOptionsMode(CurveOptionsMode::Noise); });

    WireCurveOptions();

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowEulerFilter")))
    {
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (!EnsureEditableClip() || !m_CurrentClipAsset) return;

            std::unordered_map<int, std::unordered_set<float>> selByChannel;
            auto collectSel = [&](const auto& selKeys) {
                for (const auto& sk : selKeys)
                    if (sk.Channel >= 0) selByChannel[sk.Channel].insert(sk.KeyTime);
            };
            if (m_CurvesGraphView)  collectSel(m_CurvesGraphView->GetSelectedKeyframes());
            if (m_DopeSheetView)    collectSel(m_DopeSheetView->GetSelectedKeyframes());

            const bool hasAnySelection = !selByChannel.empty();
            if (!hasAnySelection && m_SelectedChannel < 0) return;

            (void)ExecuteClipEditWithUndo("Euler Filter",
                [this, selByChannel, hasAnySelection]()
                {
                    bool changed = false;
                    const auto& channels = m_CurrentClipAsset->GetChannels();

                    auto applyFilter = [&](size_t chIdx, const std::unordered_set<float>* filter) {
                        if (chIdx >= channels.size()) return;
                        const AnimChannel& ch = channels[chIdx];
                        if (ch.path != AnimPath::Rotation || ch.keys.size() < 2) return;
                        for (size_t i = 1; i < ch.keys.size(); ++i)
                        {
                            if (filter && !filter->count(ch.keys[i].time)) continue;
                            const float* prev = ch.keys[i - 1].rotation;
                            const float* cur  = ch.keys[i].rotation;
                            const float dot = prev[0]*cur[0] + prev[1]*cur[1] + prev[2]*cur[2] + prev[3]*cur[3];
                            if (dot < 0.0f)
                                for (uint32 c = 0; c < 4u; ++c)
                                    changed |= m_CurrentClipAsset->SetKeyframeComponentValue(chIdx, ch.keys[i].time, c, -cur[c]);
                        }
                    };

                    if (hasAnySelection)
                        for (const auto& kv : selByChannel)
                            applyFilter(static_cast<size_t>(kv.first), &kv.second);
                    else
                        applyFilter(static_cast<size_t>(m_SelectedChannel), nullptr);
                    return changed;
                });
        });
    }

    auto stubBtn = [this](const char* id, const char* label)
    {
        auto* btn = dynamic_cast<Button*>(FindById(id));
        if (!btn) return;
        btn->RegisterEventHandler(kEventButtonClick, [label](UIEvent&)
        {
            Logger::Log::Info("AnimationWindow: {} not yet implemented", label);
        });
    };
    // Retime tool toggle.
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurveDraw")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            if (!m_CurvesGraphView) return;
            const bool wasRetime = m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Retime;
            m_CurvesGraphView->SetActiveTool(wasRetime ? CurvesGraphView::CurveTool::Select : CurvesGraphView::CurveTool::Retime);
            if (!wasRetime) self.AddClass("active"); else self.RemoveClass("active");
            if (auto* lb = dynamic_cast<Button*>(FindById("AnimationWindowLattice"))) lb->RemoveClass("active");
            if (auto* db = dynamic_cast<Button*>(FindById("AnimationWindowDrawCurve"))) db->RemoveClass("active");
        });

    // Draw-curve (freehand) tool toggle.
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowDrawCurve")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            if (!m_CurvesGraphView) return;
            const bool wasDraw = m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::DrawCurve;
            m_CurvesGraphView->SetActiveTool(wasDraw ? CurvesGraphView::CurveTool::Select : CurvesGraphView::CurveTool::DrawCurve);
            if (!wasDraw) self.AddClass("active"); else self.RemoveClass("active");
            if (auto* rb = dynamic_cast<Button*>(FindById("AnimationWindowCurveDraw"))) rb->RemoveClass("active");
            if (auto* lb = dynamic_cast<Button*>(FindById("AnimationWindowLattice"))) lb->RemoveClass("active");
        });

    // Lattice deform toggle.
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowLattice")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            if (!m_CurvesGraphView) return;
            const bool isLattice = m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice;
            // Toggling Lattice on closes any open curve-options mode (Smooth /
            // Simplify / Add Noise) so the options bar isn't asked to render
            // two competing panels at once.
            if (!isLattice && m_CurveOptionsMode != CurveOptionsMode::None)
                ExitCurveOptionsMode(false);
            m_CurvesGraphView->SetActiveTool(isLattice ? CurvesGraphView::CurveTool::Select : CurvesGraphView::CurveTool::Lattice);
            if (!isLattice) self.AddClass("active"); else self.RemoveClass("active");
            UpdateLatticeOptionsPanel();
            if (auto* db = dynamic_cast<Button*>(FindById("AnimationWindowCurveDraw"))) db->RemoveClass("active");
            if (auto* dr = dynamic_cast<Button*>(FindById("AnimationWindowDrawCurve"))) dr->RemoveClass("active");
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowBreakTangents")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { BreakTangents(); });
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowUnifyTangents")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { UnifyTangents(); });
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowFramePlaybackRange")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { FramePlaybackRange(); });
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCenterPlayhead")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { CenterViewOnCurrentTime(); });
    if (auto* zoomSlider = dynamic_cast<Slider*>(FindById("AnimationWindowTimelineZoomSlider")))
    {
        zoomSlider->SetValueWithoutNotify(ComputeTimelineZoomSliderValue());
        zoomSlider->SetOnValueChanging([this](float value) { ApplyTimelineZoomSliderValue(value, false); });
        zoomSlider->SetOnValueChanged([this](float value) { ApplyTimelineZoomSliderValue(value, true); });
    }

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurvesStacked")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            if (!m_CurvesGraphView) return;
            const bool wasStacked = m_CurvesGraphView->GetCurveViewMode() == CurvesGraphView::CurveViewMode::Stacked;
            m_CurvesGraphView->SetCurveViewMode(wasStacked ? CurvesGraphView::CurveViewMode::Absolute : CurvesGraphView::CurveViewMode::Stacked);
            if (!wasStacked) self.AddClass("active"); else self.RemoveClass("active");
            if (auto* nb = dynamic_cast<Button*>(FindById("AnimationWindowCurvesNormalized")))
                nb->RemoveClass("active");
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCurvesNormalized")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e)
        {
            UIElement& self = *e.CurrentTarget;
            if (!m_CurvesGraphView) return;
            const bool wasNorm = m_CurvesGraphView->GetCurveViewMode() == CurvesGraphView::CurveViewMode::Normalized;
            m_CurvesGraphView->SetCurveViewMode(wasNorm ? CurvesGraphView::CurveViewMode::Absolute : CurvesGraphView::CurveViewMode::Normalized);
            if (!wasNorm) self.AddClass("active"); else self.RemoveClass("active");
            if (auto* sb = dynamic_cast<Button*>(FindById("AnimationWindowCurvesStacked")))
                sb->RemoveClass("active");
        });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowBufferSnapshot")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_CurvesGraphView) m_CurvesGraphView->TakeBufferSnapshot(); });

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSwapBuffer")))
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (!m_CurvesGraphView || !m_CurrentClipAsset || !m_CurvesGraphView->HasBufferSnapshot())
                return;
            const auto& bufVals = m_CurvesGraphView->GetBufferValues();
            (void)ExecuteClipEditWithUndo("Swap Buffer Curve", [this, bufVals]() -> bool
            {
                if (!m_CurrentClip) return false;
                const auto& channels = m_CurrentClip->GetChannels();
                std::vector<std::vector<float>> newBuffer;
                newBuffer.resize(channels.size());
                bool changed = false;
                for (size_t ci = 0; ci < channels.size(); ++ci)
                {
                    if (ci >= bufVals.size()) continue;
                    const AnimChannel& ch = channels[ci];
                    const uint32 numComp = AnimChannelComponentCount(ch);
                    const std::vector<float>& src = bufVals[ci];
                    if (ch.keys.size() * numComp != src.size()) continue;
                    newBuffer[ci].reserve(src.size());
                    for (size_t ki = 0; ki < ch.keys.size(); ++ki)
                    {
                        for (uint32 c = 0; c < numComp; ++c)
                            newBuffer[ci].push_back(GetKeyComponentValue(ch.keys[ki], ch.path, c));
                        for (uint32 c = 0; c < numComp; ++c)
                            changed |= m_CurrentClipAsset->SetKeyframeComponentValue(
                                static_cast<uint32>(ci), ch.keys[ki].time, c, src[ki * numComp + c]);
                    }
                }
                if (changed && m_CurvesGraphView)
                    m_CurvesGraphView->SetBufferValues(std::move(newBuffer));
                return changed;
            });
        });

    // Stats bar: commit T/V fields to edit selected keyframe with arithmetic support.
    if (m_StatsTimeField)
    {
        m_StatsTimeField->SetOnCommit([this]()
        {
            if (!m_CurvesGraphView) return;
            const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
            if (sel.size() != 1u) return;
            const int ch = sel[0].Channel;
            const float oldTime = sel[0].KeyTime;
            const float newTime = ApplyArithmetic(oldTime, m_StatsTimeField->GetValue());
            if (std::abs(newTime - oldTime) < 1e-6f) return;
            if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
            (void)ExecuteClipEditWithUndo("Edit Keyframe Time",
                [this, ch, oldTime, newTime]() -> bool
                {
                    return m_CurrentClipAsset->SetKeyframeTime(static_cast<size_t>(ch), oldTime, newTime);
                });
        });
        // Live preview: update keyframe position as user types
        m_StatsTimeField->SetOnValueChanging([this](const std::string& value)
        {
            if (!m_CurvesGraphView || !m_CurrentClipAsset) return;
            const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
            if (sel.size() != 1u) return;
            const int ch = sel[0].Channel;
            const float oldTime = sel[0].KeyTime;
            // Parse value directly without arithmetic for live preview
            float newTime = 0.0f;
            try { newTime = std::stof(value); } catch (...) { return; }
            if (std::abs(newTime - oldTime) < 1e-6f) return;
            // Live update without undo
            m_CurrentClipAsset->SetKeyframeTime(static_cast<size_t>(ch), oldTime, newTime);
            // Update CurvesGraphView selection to match the new time
            m_CurvesGraphView->RestoreSelection({{ch, newTime}}, m_SelectedChannel, m_SelectedComponent);
            if (m_CurvesGraphView)
                m_CurvesGraphView->MarkDirty(UIElement::VisualDirty);
            // Don't call RefreshChannelSelection here - it will call SetSelectedCurve which resets selection
        });
        // Drag support on the T: label
        if (m_StatsTimeLabel)
        {
            m_StatsTimeLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
            {
                if (e.Button != 0) return;
                if (!m_CurvesGraphView) return;
                const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
                if (sel.size() != 1u) return;
                if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
                m_StatsTimeDragging = true;
                m_StatsDragLastX = e.X;
                m_StatsDragStartValue = sel[0].KeyTime;
                BeginClipUndoGesture("Drag Keyframe Time");
                e.Capture(m_StatsTimeLabel);
                e.Stop();
            });
            m_StatsTimeLabel->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
            {
                if (!m_StatsTimeDragging || !m_CurvesGraphView || !m_CurrentClipAsset) return;
                const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
                if (sel.size() != 1u) return;
                const int ch = sel[0].Channel;
                const float oldTime = m_StatsDragStartValue;
                const float dx = e.X - m_StatsDragLastX;
                m_StatsDragLastX = e.X;
                constexpr float kSensitivity = 0.01f;
                float newTime = oldTime + dx * kSensitivity;
                // Apply snapping if enabled
                if (m_SnapTime && m_TimelineState.fps > 0.0f)
                    newTime = std::round(newTime * m_TimelineState.fps) / m_TimelineState.fps;
                // Clamp to visible timerange to prevent key from going off-screen
                newTime = std::clamp(newTime, m_TimelineState.viewStart, m_TimelineState.viewEnd);
                // Live update
                m_CurrentClipAsset->SetKeyframeTime(static_cast<size_t>(ch), oldTime, newTime);
                // Update drag start value to the new time so subsequent lookups work
                m_StatsDragStartValue = newTime;
                // Update CurvesGraphView selection to match the new time
                m_CurvesGraphView->RestoreSelection({{ch, newTime}}, m_SelectedChannel, m_SelectedComponent);
                RefreshEditedClipState();
                // Update the text field in real-time
                if (m_StatsTimeField)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4g", newTime);
                    m_StatsTimeField->SetValue(std::string(buf));
                }
                // Don't call RefreshChannelSelection here - it will call SetSelectedCurve which resets selection
                e.Stop();
            });
            m_StatsTimeLabel->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
            {
                if (!m_StatsTimeDragging) return;
                m_StatsTimeDragging = false;
                // Commit the undo gesture (no per-edit undos needed - we used a gesture)
                CommitClipUndoGesture();
                if (m_CurvesGraphView)
                    m_CurvesGraphView->MarkDirty(UIElement::VisualDirty);
                e.Stop();
            });
        }
    }
    if (m_StatsValueField)
    {
        m_StatsValueField->SetOnCommit([this]()
        {
            if (!m_CurvesGraphView) return;
            const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
            if (sel.size() != 1u) return;
            const int ch = sel[0].Channel;
            const float keyTime = sel[0].KeyTime;
            if (!m_CurrentClip || ch < 0 ||
                static_cast<size_t>(ch) >= m_CurrentClip->GetChannels().size()) return;
            const AnimChannel& channel = m_CurrentClip->GetChannels()[static_cast<size_t>(ch)];
            float oldVal = 0.0f;
            for (const AnimKeyframe& key : channel.keys)
                if (std::abs(key.time - keyTime) < 1e-4f)
                    { oldVal = GetKeyComponentValue(key, channel.path, m_SelectedComponent); break; }
            const float newVal = ApplyArithmetic(oldVal, m_StatsValueField->GetValue());
            if (std::abs(newVal - oldVal) < 1e-7f) return;
            if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
            (void)ExecuteClipEditWithUndo("Edit Keyframe Value",
                [this, ch, keyTime, newVal]() -> bool
                {
                    return m_CurrentClipAsset->SetKeyframeComponentValue(
                        static_cast<size_t>(ch), keyTime, m_SelectedComponent, newVal);
                });
        });
        // Live preview: update keyframe value as user types
        m_StatsValueField->SetOnValueChanging([this](const std::string& value)
        {
            if (!m_CurvesGraphView || !m_CurrentClipAsset) return;
            const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
            if (sel.size() != 1u) return;
            const int ch = sel[0].Channel;
            const float keyTime = sel[0].KeyTime;
            // Parse value directly without arithmetic for live preview
            float newVal = 0.0f;
            try { newVal = std::stof(value); } catch (...) { return; }
            // Live update without undo
            m_CurrentClipAsset->SetKeyframeComponentValue(static_cast<size_t>(ch), keyTime, m_SelectedComponent, newVal);
            if (m_CurvesGraphView)
                m_CurvesGraphView->MarkDirty(UIElement::VisualDirty);
            RefreshChannelSelection();
        });
        // Drag support on the V: label
        if (m_StatsValueLabel)
        {
            m_StatsValueLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
            {
                if (e.Button != 0) return;
                if (!m_CurvesGraphView) return;
                const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
                if (sel.size() != 1u) return;
                if (!EnsureEditableClip() || !m_CurrentClipAsset) return;
                m_StatsValueDragging = true;
                m_StatsDragLastX = e.X;
                // Get current value
                const int ch = sel[0].Channel;
                const float kt = sel[0].KeyTime;
                if (m_CurrentClip && ch >= 0 && static_cast<size_t>(ch) < m_CurrentClip->GetChannels().size())
                {
                    const AnimChannel& channel = m_CurvesGraphView->GetClip()->GetChannels()[static_cast<size_t>(ch)];
                    for (const AnimKeyframe& key : channel.keys)
                        if (std::abs(key.time - kt) < 1e-4f)
                        {
                            m_StatsDragStartValue = GetKeyComponentValue(key, channel.path, m_SelectedComponent);
                            break;
                        }
                }
                BeginClipUndoGesture("Drag Keyframe Value");
                e.Capture(m_StatsValueLabel);
                e.Stop();
            });
            m_StatsValueLabel->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
            {
                if (!m_StatsValueDragging || !m_CurvesGraphView || !m_CurrentClipAsset) return;
                const auto& sel = m_CurvesGraphView->GetSelectedKeyframes();
                if (sel.size() != 1u) return;
                const int ch = sel[0].Channel;
                const float keyTime = sel[0].KeyTime;
                const float dx = e.X - m_StatsDragLastX;
                m_StatsDragLastX = e.X;
                constexpr float kSensitivity = 0.01f;
                float newVal = m_StatsDragStartValue + dx * kSensitivity;
                // Apply snapping if enabled
                if (m_SnapValue)
                    newVal = std::round(newVal * 10.0f) / 10.0f;
                // Clamp to visible value range to prevent key from going off-screen
                float vMin = 0.0f, vMax = 0.0f;
                m_CurvesGraphView->GetValueRange(vMin, vMax);
                newVal = std::clamp(newVal, vMin, vMax);
                // Live update
                m_CurrentClipAsset->SetKeyframeComponentValue(static_cast<size_t>(ch), keyTime, m_SelectedComponent, newVal);
                // Update drag start value so next delta is calculated from current position
                m_StatsDragStartValue = newVal;
                RefreshEditedClipState();
                // Update the text field in real-time
                if (m_StatsValueField)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4g", newVal);
                    m_StatsValueField->SetValue(std::string(buf));
                }
                // Don't call RefreshChannelSelection here - it will call SetSelectedCurve which resets selection
                e.Stop();
            });
            m_StatsValueLabel->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
            {
                if (!m_StatsValueDragging) return;
                m_StatsValueDragging = false;
                // Commit the undo gesture (no per-edit undos needed - we used a gesture)
                CommitClipUndoGesture();
                if (m_CurvesGraphView)
                    m_CurvesGraphView->MarkDirty(UIElement::VisualDirty);
                e.Stop();
            });
        }
    }

    if (auto* fpsField = dynamic_cast<FloatField*>(FindById("AnimationWindowFpsField")))
    {
        fpsField->SetValue(m_TimelineState.fps);
        fpsField->SetOnValueChanged([this](const float& v)
                                   {
                                       const float fps = std::clamp(v, 1.0f, 240.0f);
                                       m_TimelineState.fps = fps;
                                       UpdateTimelineRulerAndLabels();
                                       UpdateCurrentFrameLabel();
                                   });
    }

    if (m_PropertiesSearchField)
    {
        auto* searchRow = m_PropertiesSearchField->GetParent();
        EditorSearchBars::RegisterFocusTarget(searchRow);
        auto* searchClear = dynamic_cast<Button*>(FindById("AnimationWindowPropertiesSearchClear"));
        if (searchClear)
        {
            searchClear->SetFocusable(false);
            searchClear->SetTooltip("Clear search");
        }
        auto updateSearchVisualState = [searchRow, searchClear](const std::string& value)
        {
            if (!searchRow || !searchClear)
                return;
            if (value.empty())
            {
                searchRow->RemoveClass("has-query");
                searchClear->AddClass("hidden");
            }
            else
            {
                searchRow->AddClass("has-query");
                searchClear->RemoveClass("hidden");
            }
        };
        m_PropertiesSearchField->SetOnValueChanging([this, updateSearchVisualState](const std::string& v)
        {
            m_PropertiesSearchQuery = v;
            updateSearchVisualState(v);
            RefreshPropertyTree();
        });
        if (searchClear)
        {
            searchClear->RegisterEventHandler(kEventButtonClick, [this, updateSearchVisualState](UIEvent&)
            {
                if (!m_PropertiesSearchField || m_PropertiesSearchField->GetValue().empty())
                    return;
                m_PropertiesSearchField->SetValue("");
                m_PropertiesSearchQuery.clear();
                updateSearchVisualState("");
                RefreshPropertyTree();
            });
        }
    }
    auto syncFilterButtons = [this]()
    {
        auto set = [](Button* btn, bool active)
        {
            if (!btn) return;
            if (active) btn->AddClass("active"); else btn->RemoveClass("active");
        };
        set(m_FilterAnimatedButton,    m_PropertyFilter == PropertyFilter::Animated);
        set(m_FilterHierarchyButton,   m_PropertyFilter == PropertyFilter::Hierarchy);
        set(m_FilterChannelsOnlyButton, m_PropertyFilter == PropertyFilter::ChannelsOnly);
        set(m_FilterFlatButton,        m_PropertyFilter == PropertyFilter::Flat);
    };
    syncFilterButtons();
    if (m_FilterAnimatedButton)
    {
        m_FilterAnimatedButton->RegisterEventHandler(kEventButtonClick, [this, syncFilterButtons](UIEvent&)
        {
            m_PropertyFilter = PropertyFilter::Animated;
            syncFilterButtons();
            RefreshPropertyTree();
        });
    }
    if (m_FilterHierarchyButton)
    {
        m_FilterHierarchyButton->RegisterEventHandler(kEventButtonClick, [this, syncFilterButtons](UIEvent&)
        {
            m_PropertyFilter = PropertyFilter::Hierarchy;
            syncFilterButtons();
            RefreshPropertyTree();
        });
    }
    if (m_FilterChannelsOnlyButton)
    {
        m_FilterChannelsOnlyButton->RegisterEventHandler(kEventButtonClick, [this, syncFilterButtons](UIEvent&)
        {
            m_PropertyFilter = PropertyFilter::ChannelsOnly;
            syncFilterButtons();
            RefreshPropertyTree();
        });
    }
    if (m_FilterFlatButton)
    {
        m_FilterFlatButton->RegisterEventHandler(kEventButtonClick, [this, syncFilterButtons](UIEvent&)
        {
            m_PropertyFilter = PropertyFilter::Flat;
            syncFilterButtons();
            RefreshPropertyTree();
        });
    }

    auto wireToggle = [this](const char* id, bool AnimationWindowPanel::* member)
    {
        auto* btn = dynamic_cast<Button*>(FindById(id));
        if (!btn) return;
        if (this->*member) btn->AddClass("pressed");
        btn->RegisterEventHandler(kEventButtonClick, [this, btn, member](UIEvent&)
        {
            this->*member = !(this->*member);
            if (this->*member) btn->AddClass("pressed");
            else btn->RemoveClass("pressed");
        });
    };
    // Wire snap toggles to also propagate to CurvesGraphView
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSnapTime")))
    {
        if (m_SnapTime) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
        btn->RegisterEventHandler(kEventButtonClick, [this, btn](UIEvent&)
        {
            m_SnapTime = !m_SnapTime;
            if (m_SnapTime) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
            if (m_CurvesGraphView) m_CurvesGraphView->SetSnapTime(m_SnapTime);
        });
    }
    if (auto* snapMode = dynamic_cast<Dropdown*>(FindById("AnimationWindowSnapModeDropdown")))
    {
        snapMode->SetOptions({{"FPS", "FPS"}, {"Seconds", "Seconds"}}, m_SnapTimeStep > 0.0f ? 1 : 0);
        snapMode->SetOnValueChanged([this](const std::string& value)
        {
            if (value == "FPS")
            {
                m_SnapTimeStep = 0.0f;
            }
            else if (m_SnapTimeStep <= 0.0f)
            {
                m_SnapTimeStep = 1.0f / std::max(m_TimelineState.fps, 1.0f);
            }
            if (auto* field = dynamic_cast<FloatField*>(FindById("AnimationWindowSnapTimeStep")))
                field->SetValue(m_SnapTimeStep);
            if (m_CurvesGraphView)
                m_CurvesGraphView->SetSnapTimeStep(m_SnapTimeStep);
        });
    }
    if (auto* snapTimeStepField = dynamic_cast<FloatField*>(FindById("AnimationWindowSnapTimeStep")))
    {
        snapTimeStepField->SetValue(m_SnapTimeStep);
        snapTimeStepField->SetOnValueChanged([this](const float& v)
        {
            m_SnapTimeStep = std::max(0.0f, v);
            if (m_CurvesGraphView) m_CurvesGraphView->SetSnapTimeStep(m_SnapTimeStep);
        });
    }
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowSnapValue")))
    {
        if (m_SnapValue) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
        btn->RegisterEventHandler(kEventButtonClick, [this, btn](UIEvent&)
        {
            m_SnapValue = !m_SnapValue;
            if (m_SnapValue) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
            if (m_CurvesGraphView) m_CurvesGraphView->SetSnapValue(m_SnapValue);
        });
    }
    if (auto* snapStepField = dynamic_cast<FloatField*>(FindById("AnimationWindowSnapValueStep")))
    {
        snapStepField->SetValue(m_SnapValueStep);
        snapStepField->SetOnValueChanged([this](const float& v)
        {
            m_SnapValueStep = std::max(0.0f, v);
            if (m_CurvesGraphView) m_CurvesGraphView->SetSnapValueStep(m_SnapValueStep);
        });
    }
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowToggleKeytool")))
    {
        if (m_KeytoolEnabled) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
        btn->RegisterEventHandler(kEventButtonClick, [this, btn](UIEvent&)
        {
            m_KeytoolEnabled = !m_KeytoolEnabled;
            if (m_KeytoolEnabled) btn->AddClass("pressed"); else btn->RemoveClass("pressed");
            if (m_CurvesGraphView) m_CurvesGraphView->SetKeyInsertionEnabled(m_KeytoolEnabled);
        });
    }
    if (auto* tangentMode = dynamic_cast<Dropdown*>(FindById("AnimationWindowDefaultTangentDropdown")))
    {
        tangentMode->SetOptions({
            {"Auto", "Auto"},
            {"Flat", "Flat"},
            {"Linear", "Linear"},
            {"Plateau", "Plateau"},
            {"Clamped", "Clamped"},
            {"Fixed", "Fixed"}
        }, 0);
        tangentMode->SetOnValueChanged([this](const std::string& value)
        {
            AnimTangentType type = AnimTangentType::Auto;
            if (value == "Flat") type = AnimTangentType::Flat;
            else if (value == "Linear") type = AnimTangentType::Linear;
            else if (value == "Plateau") type = AnimTangentType::Plateau;
            else if (value == "Clamped") type = AnimTangentType::Clamped;
            else if (value == "Fixed") type = AnimTangentType::Fixed;
            SetTangentTypeOnSelectedKeys(type);
        });
    }

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowShowGrid")))
    {
        if (m_ShowGrid) btn->AddClass("icon-active"); else btn->RemoveClass("icon-active");
        btn->RegisterEventHandler(kEventButtonClick, [this, btn](UIEvent&)
        {
            m_ShowGrid = !m_ShowGrid;
            if (m_ShowGrid) btn->AddClass("icon-active"); else btn->RemoveClass("icon-active");
            if (m_CurvesGraphView) m_CurvesGraphView->SetShowGrid(m_ShowGrid);
        });
    }

    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowCopy")))
    {
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            m_KeyClipboard.clear();
            if (!m_CurrentClip) return;
            const auto& channels = m_CurrentClip->GetChannels();
            if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
            {
                for (const CurvesGraphView::SelectedKey& k : m_CurvesGraphView->GetSelectedKeyframes())
                {
                    if (k.Channel < 0 || static_cast<size_t>(k.Channel) >= channels.size()) continue;
                    const auto& channel = channels[k.Channel];
                    for (const auto& key : channel.keys)
                    {
                        if (std::abs(key.time - k.KeyTime) < 0.0001f)
                        {
                            m_KeyClipboard.push_back({k.Channel, key});
                            break;
                        }
                    }
                }
            }
            else if (m_DopeSheetView)
            {
                for (const DopeSheetView::SelectedKey& k : m_DopeSheetView->GetSelectedKeyframes())
                {
                    if (k.Channel < 0 || static_cast<size_t>(k.Channel) >= channels.size()) continue;
                    const auto& channel = channels[k.Channel];
                    for (const auto& key : channel.keys)
                    {
                        if (std::abs(key.time - k.KeyTime) < 0.0001f)
                        {
                            m_KeyClipboard.push_back({k.Channel, key});
                            break;
                        }
                    }
                }
            }
        });
    }
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowPaste")))
    {
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_KeyClipboard.empty() || !m_CurrentClipAsset) return;
            const float t = m_TimelineState.currentTime;
            float anchor = std::numeric_limits<float>::infinity();
            for (const auto& item : m_KeyClipboard) anchor = std::min(anchor, item.Keyframe.time);
            if (!std::isfinite(anchor)) return;
            (void)ExecuteClipEditWithUndo("Paste Animation Keys",
                [this, anchor, t]()
                {
                    bool changed = false;
                    for (const auto& item : m_KeyClipboard)
                    {
                        AnimKeyframe keyCopy = item.Keyframe;
                        keyCopy.time = t + (item.Keyframe.time - anchor);
                        if (m_CurrentClipAsset->AddKeyframeWithData(static_cast<size_t>(item.Channel), keyCopy))
                            changed = true;
                    }
                    return changed;
                });
        });
    }
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowOpenAnimation")))
    {
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            // Filter the open dialog to whatever asset type the active view
            // can edit, mirroring the Save button's per-view dispatch:
            //   TimeComposite → .timeline   (composite multi-track timeline)
            //   ClipEditor    → .clipset    (lane-based clip arrangement)
            //   DopeSheet / Curves → .anim / .fbx / .glb / .gltf (animation)
            auto& am = EngineCore::GetInstance().GetAssetManager();
            std::filesystem::path initialPath = am.GetAssetRoot() / "Assets";
            if (m_ActiveView == ActiveView::TimeComposite)
            {
                std::filesystem::path selected = Platform::SelectFile(initialPath, "Timeline", "*.timeline");
                if (selected.empty()) return;
                (void)OpenTimeline(selected);
                return;
            }
            if (m_ActiveView == ActiveView::ClipEditor)
            {
                std::filesystem::path selected = Platform::SelectFile(initialPath, "Clip Set", "*.clipset");
                if (selected.empty()) return;
                (void)OpenClipSet(selected);
                return;
            }
            std::filesystem::path selected = Platform::SelectFile(
                initialPath,
                "Animation Files",
                "*.anim;*.fbx;*.glb;*.gltf");
            if (selected.empty()) return;

            // Route directly through OpenAnimation. AssetManager.LoadAsset
            // would resolve .fbx/.glb/.gltf to AssetType::Model and produce
            // a ModelAsset, which a Model→Animation type filter would
            // silently reject. OpenAnimation constructs an AnimationClip
            // directly and works for both .anim and source-file extensions.
            (void)OpenAnimation(selected, 0u);
        });
    }
    if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowFolderOpen")))
    {
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_CurrentClipPath.empty()) return;
            const std::filesystem::path dir = m_CurrentClipPath.parent_path();
            if (dir.empty()) return;
            (void)Platform::OpenPath(dir);

            // Also select the file in the assets panel
            if (m_OnRevealInAssetsPanel)
                m_OnRevealInAssetsPanel(m_CurrentClipPath);
        });
    }

    static constexpr struct { const char* id; const char* tip; } kTooltips[] = {
        { "AnimationWindowNewAnimation",       "New Animation Clip" },
        { "AnimationWindowSave",               "Save Animation Clip" },
        { "AnimationWindowAnimationMenu",      "Animation asset tools" },
        { "AnimationWindowClipDropdown",       "Select animation clip" },
        { "AnimationWindowDopeSheetView",      "Show dope sheet" },
        { "AnimationWindowCurvesView",         "Show curves" },
        { "AnimationWindowUnsavedIndicator",   "Current animation has unsaved changes" },
        { "AnimationWindowPreviewToggle",      "Toggle animated preview in scene view" },
        { "AnimationWindowCopy",               "Copy selected keyframes" },
        { "AnimationWindowPaste",              "Paste keyframes at current time" },
        { "AnimationWindowOpenAnimation",      "Open animation file (.anim, .fbx, .glb, .gltf)" },
        { "AnimationWindowFolderOpen",         "Reveal animation file in explorer" },
        { "AnimationWindowPropertiesSearch",   "Filter animated properties and hierarchy rows" },
        { "AnimationWindowPropertiesTree",     "Animated property hierarchy; select rows to edit curves and keys" },
        { "AnimationWindowFrameAll",           "Frame all keyframes (A)" },
        { "AnimationWindowFrameSelectedCurves","Frame selected keyframes" },
        { "AnimationWindowFitHeight",          "Fit value range to visible curves" },
        { "AnimationWindowShowGrid",           "Toggle grid overlay" },
        { "AnimationWindowSnapTime",           "Snap keyframes to frame grid" },
        { "AnimationWindowSnapTimeStep",       "Time snap step in seconds (0 / blank = snap to frame grid). Used by Snap Time." },
        { "AnimationWindowSnapValue",          "Snap keyframes to value grid" },
        { "AnimationWindowSnapValueStep",      "Value snap step (0 / blank = auto from grid). Used by Snap Value." },
        { "AnimationWindowCurveLinear",        "Set interpolation: Linear" },
        { "AnimationWindowCurveStepped",       "Set interpolation: Step / Constant" },
        { "AnimationWindowCurveSpline",        "Set interpolation: Cubic Spline" },
        { "AnimationWindowSmoothLowpass",      "Smooth (Low-Pass): Gaussian-weighted neighbor averaging" },
        { "AnimationWindowSmoothPeak",         "Smooth (Peak-Preserving): smooth between extrema, keep peaks" },
        { "AnimationWindowCurveSimplify",      "Simplify: remove redundant keyframes" },
        { "AnimationWindowCurveDraw",          "Retime tool: drag to stretch/compress key timing" },
        { "AnimationWindowDrawCurve",          "Draw tool: freehand a curve; release commits a simplified set of keyframes on the selected channel" },
        { "AnimationWindowEaseIn",             "Ease In: flat incoming tangent" },
        { "AnimationWindowEaseEase",           "Ease In+Out: smooth enter and exit" },
        { "AnimationWindowEaseOut",            "Ease Out: flat outgoing tangent" },
        { "AnimationWindowSelectAllKeys",      "Select all visible curve keys" },
        { "AnimationWindowDeselectKeys",       "Clear curve key selection" },
        { "AnimationWindowBreakTangents",      "Break tangents: make in/out handles independent" },
        { "AnimationWindowUnifyTangents",      "Unify tangents: mirror in/out handles" },
        { "AnimationWindowCurvesStacked",      "Stacked view: one lane per channel" },
        { "AnimationWindowCurvesNormalized",   "Normalized view: scale each curve to −1…1" },
        { "AnimationWindowBufferSnapshot",     "Take buffer snapshot of current curves" },
        { "AnimationWindowSwapBuffer",         "Swap curves with buffer snapshot" },
        { "AnimationWindowFramePlaybackRange", "Frame view to playback range (P)" },
        { "AnimationWindowCenterPlayhead",     "Center the current playhead in the view (C)" },
        { "AnimationWindowTimelineZoomSlider", "Zoom timeline around the current view center" },
        { "AnimationWindowToggleKeytool",      "Toggle key insertion on timeline click" },
        { "AnimationWindowSnapModeDropdown",   "Choose whether time snapping uses FPS frames or seconds" },
        { "AnimationWindowDefaultTangentDropdown", "Apply a default tangent mode to selected curve keys" },
        { "AnimationWindowPreviousKeyframe",   "Go to previous visible keyframe (,)" },
        { "AnimationWindowAddKeyframe",        "Insert keyframe at current time" },
        { "AnimationWindowDuplicateKeyframe",  "Duplicate selected keyframes (D)" },
        { "AnimationWindowDeleteKeyframe",     "Delete selected keyframes (Delete)" },
        { "AnimationWindowNextKeyframe",       "Go to next visible keyframe (.)" },
        { "AnimationWindowAddMarker",          "Add timeline marker at current time" },
        { "AnimationWindowAddNoise",           "Add random noise to selected channel" },
        { "AnimationWindowEulerFilter",        "Euler filter: fix quaternion sign flips on rotation channel" },
        { "AnimationWindowLattice",            "Lattice deform: reshape selected keys with control points" },
        { "AnimationWindowLoop",               "Toggle loop playback" },
        { "AnimationWindowSyncPlayhead",       "Scroll timeline to follow the playhead (S)" },
        { "AnimationWindowRecord",             "Record mode: auto-insert keys on property change" },
        { "AnimationWindowGoToStart",          "Go to playback range start" },
        { "AnimationWindowPreviousFrame",      "Step one frame backward" },
        { "AnimationWindowNextFrame",          "Step one frame forward" },
        { "AnimationWindowGoToEnd",            "Go to playback range end" },
        { "AnimationWindowStop",               "Stop playback and return to frame 0" },
        { "AnimationWindowPlayBackward",       "Play / Pause animation in reverse" },
        { "AnimationWindowPause",              "Pause playback" },
        { "AnimationWindowPlay",               "Play / Pause animation" },
        { "AnimationWindowFilterAnimated",     "Show only animated properties" },
        { "AnimationWindowFilterHierarchy",    "Show full hierarchy" },
        { "AnimationWindowFilterChannelsOnly", "Show animated channels only" },
        { "AnimationWindowFilterFlat",         "Show each channel as a flat row with full path label" },
        { "AnimationWindowRangeStart",         "Playback range start (frame)" },
        { "AnimationWindowRangeEnd",           "Playback range end (frame)" },
        { "AnimationWindowFpsField",           "Playback frames per second" },
        { "AnimationWindowStatsTime",          "Selected key time (editable)" },
        { "AnimationWindowStatsValue",         "Selected key value (editable)" },
        { "AnimationWindowTimelineMarkers",    "Timeline markers; drag markers to adjust timing" },
        { "AnimationWindowTimelineTimeLabels", "Time labels for the visible timeline range" },
        { "AnimationWindowTimelineRuler",      "Scrub and select time on the timeline ruler" },
        { "AnimationWindowTimelineFrameLabels", "Frame labels for the visible timeline range" },
        { "AnimationWindowOptionsNoise",       "Noise tool options for selected keys" },
        { "AnimationWindowNoiseFreqMinLabel",  "Minimum noise frequency" },
        { "AnimationWindowNoiseFreqMin",       "Minimum random noise frequency per second" },
        { "AnimationWindowNoiseFreqMaxLabel",  "Maximum noise frequency" },
        { "AnimationWindowNoiseFreqMax",       "Maximum random noise frequency per second" },
        { "AnimationWindowNoiseMagnitudeLabel", "Noise magnitude" },
        { "AnimationWindowNoiseMagnitude",     "Maximum value offset generated by the noise tool" },
        { "AnimationWindowNoiseConfirm",       "Apply noise to the selected keys" },
        { "AnimationWindowOptionsSimplify",    "Simplify tool options for selected keys" },
        { "AnimationWindowSimplifyMethod",     "Simplification algorithm for reducing keys" },
        { "AnimationWindowSimplifyTimeTolLabel", "Maximum gap between retained keys" },
        { "AnimationWindowSimplifyTimeTol",    "Maximum gap in seconds between retained keys; zero disables this limit. Value tolerance always applies." },
        { "AnimationWindowSimplifyValueTolLabel", "Value tolerance" },
        { "AnimationWindowSimplifyValueTol",   "Maximum value error allowed while simplifying keys" },
        { "AnimationWindowSimplifyConfirm",    "Apply simplification to the selected keys" },
        { "AnimationWindowOptionsSmooth",      "Smoothing tool options for selected keys" },
        { "AnimationWindowSmoothTitle",        "Smoothing tool options" },
        { "AnimationWindowSmoothFilterWidthLabel", "Filter width" },
        { "AnimationWindowSmoothFilterWidth",  "Neighbor key radius used by the smoothing filter" },
        { "AnimationWindowSmoothSampleCountLabel", "Sample count" },
        { "AnimationWindowSmoothSampleCount",  "Number of smoothing samples to evaluate" },
        { "AnimationWindowSmoothConfirm",      "Apply smoothing to the selected keys" },
        { "AnimationWindowOptionsLattice",     "Lattice deformation options for selected keys" },
        { "AnimationWindowLatticePointCountLabel", "Lattice points" },
        { "AnimationWindowLatticePointCount",  "Number of lattice control points" },
        { "AnimationWindowLatticeBasis",       "Lattice basis used to distribute deformation" },
        { "AnimationWindowDopeSheetPlaceholder", "Dope sheet view" },
        { "AnimationWindowClipEditorPlaceholder", "Clip lane editor view" },
    };
    for (const auto& entry : kTooltips)
        if (UIElement* el = FindById(entry.id))
            el->SetTooltip(entry.tip);

    if (UIElement* saveButton = FindById("AnimationWindowSave"))
    {
        const char* saveTooltip = "Save Animation Clip";
        if (m_ActiveView == ActiveView::TimeComposite)
            saveTooltip = "Save Timeline";
        else if (m_ActiveView == ActiveView::ClipEditor)
            saveTooltip = "Save Clip Set";
        saveButton->SetTooltip(saveTooltip);
    }
}

void AnimationWindowPanel::UpdateCurrentFrameLabel()
{
    if (!m_CurrentFrameLabel)
        return;
    const int frame = static_cast<int>(std::round(m_TimelineState.currentTime * m_TimelineState.fps));
    m_CurrentFrameLabel->SetText(std::to_string(frame));
}

void AnimationWindowPanel::RefreshClipDropdown()
{
    if (!m_ClipDropdown)
        return;

    auto setClipDropdownTooltip = [this](const std::string& fullLabel, const std::string& displayLabel)
    {
        m_ClipDropdown->SetTooltip(displayLabel == fullLabel ? std::string() : fullLabel);
    };

    // The dropdown's contents and selection track the active view's notion of "current clip":
    //   DopeSheet/Curves -> animation index inside the loaded AnimationClip asset.
    //   TimeComposite    -> composite clip on the timeline (track:clip).
    //   ClipEditor       -> lane clip instance (lane:clip).
    // Each mode's state is persisted in its own member, so switching views naturally
    // restores the last-picked entry for that mode.
    if (m_ActiveView == ActiveView::TimeComposite)
    {
        if (!m_CompositeModel || m_CompositeModel->tracks.empty())
        {
            m_ClipDropdown->SetOptions({{"", "No Clips"}}, 0);
            m_ClipDropdown->SetTooltip(std::string());
            return;
        }
        std::vector<Dropdown::Option> options;
        int selected = 0;
        std::string selectedFullLabel;
        std::string selectedDisplayLabel;
        for (size_t t = 0; t < m_CompositeModel->tracks.size(); ++t)
        {
            const CompositeTrack& track = m_CompositeModel->tracks[t];
            for (size_t c = 0; c < track.clips.size(); ++c)
            {
                const std::string clipName = track.clips[c].name.empty()
                                                 ? ("Clip " + std::to_string(c + 1))
                                                 : track.clips[c].name;
                const std::string trackName = track.name.empty()
                                                  ? ("Track " + std::to_string(t + 1))
                                                  : track.name;
                std::string key = "comp:" + std::to_string(t) + ":" + std::to_string(c);
                const std::string fullLabel = trackName + " / " + clipName;
                const std::string displayLabel = ClipDropdownDisplayName(fullLabel);
                if (t == m_SelectedCompositeTrack && c == m_SelectedCompositeClip)
                {
                    selected = static_cast<int>(options.size());
                    selectedFullLabel = fullLabel;
                    selectedDisplayLabel = displayLabel;
                }
                options.push_back({std::move(key), displayLabel});
            }
        }
        if (options.empty())
        {
            m_ClipDropdown->SetOptions({{"", "No Clips"}}, 0);
            m_ClipDropdown->SetTooltip(std::string());
            return;
        }
        m_ClipDropdown->SetOptions(options, selected);
        if (selectedFullLabel.empty() && selected >= 0 && selected < static_cast<int>(options.size()))
        {
            selectedFullLabel = options[static_cast<size_t>(selected)].label;
            selectedDisplayLabel = selectedFullLabel;
        }
        setClipDropdownTooltip(selectedFullLabel, selectedDisplayLabel);
        return;
    }

    if (m_ActiveView == ActiveView::ClipEditor)
    {
        if (!m_LaneClipModel || m_LaneClipModel->lanes.empty())
        {
            m_ClipDropdown->SetOptions({{"", "No Clips"}}, 0);
            m_ClipDropdown->SetTooltip(std::string());
            return;
        }
        std::vector<Dropdown::Option> options;
        int selected = 0;
        std::string selectedFullLabel;
        std::string selectedDisplayLabel;
        for (size_t l = 0; l < m_LaneClipModel->lanes.size(); ++l)
        {
            const LaneClipLane& lane = m_LaneClipModel->lanes[l];
            for (size_t c = 0; c < lane.clips.size(); ++c)
            {
                const std::string clipName = lane.clips[c].name.empty()
                                                 ? ("Clip " + std::to_string(c + 1))
                                                 : lane.clips[c].name;
                const std::string laneName = lane.name.empty()
                                                 ? ("Lane " + std::to_string(l + 1))
                                                 : lane.name;
                std::string key = "lane:" + std::to_string(l) + ":" + std::to_string(c);
                const std::string fullLabel = laneName + " / " + clipName;
                const std::string displayLabel = ClipDropdownDisplayName(fullLabel);
                if (l == m_SelectedLane && c == m_SelectedLaneClip)
                {
                    selected = static_cast<int>(options.size());
                    selectedFullLabel = fullLabel;
                    selectedDisplayLabel = displayLabel;
                }
                options.push_back({std::move(key), displayLabel});
            }
        }
        if (options.empty())
        {
            m_ClipDropdown->SetOptions({{"", "No Clips"}}, 0);
            m_ClipDropdown->SetTooltip(std::string());
            return;
        }
        m_ClipDropdown->SetOptions(options, selected);
        if (selectedFullLabel.empty() && selected >= 0 && selected < static_cast<int>(options.size()))
        {
            selectedFullLabel = options[static_cast<size_t>(selected)].label;
            selectedDisplayLabel = selectedFullLabel;
        }
        setClipDropdownTooltip(selectedFullLabel, selectedDisplayLabel);
        return;
    }

    // DopeSheet / Curves: animations within the loaded AnimationClip asset.
    if (!m_CurrentClipAsset || m_CurrentClipPath.empty())
    {
        const std::string label = m_CurrentClipAsset ? "New Clip" : "No Clip";
        m_ClipDropdown->SetOptions({{"", label}}, 0);
        m_ClipDropdown->SetTooltip(std::string());
        return;
    }

    const std::vector<std::string> names = m_CurrentClipAsset->EnumerateAnimationNames();
    if (names.size() <= 1)
    {
        const std::string label = names.empty() ? m_CurrentClipPath.stem().string() : names[0];
        const std::string displayLabel = ClipDropdownDisplayName(label);
        m_ClipDropdown->SetOptions({{"anim:0", displayLabel}}, 0);
        setClipDropdownTooltip(label, displayLabel);
        return;
    }

    std::vector<Dropdown::Option> options;
    options.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        options.push_back({"anim:" + std::to_string(i), ClipDropdownDisplayName(names[i])});

    const int selected = static_cast<int>(m_CurrentClipAsset->GetSelectedAnimationIndex());
    m_ClipDropdown->SetOptions(options, selected);
    if (selected >= 0 && selected < static_cast<int>(names.size()))
        setClipDropdownTooltip(names[static_cast<size_t>(selected)], options[static_cast<size_t>(selected)].label);
    else
        m_ClipDropdown->SetTooltip(std::string());
}

void AnimationWindowPanel::UpdatePlayButtonState()
{
    if (UIElement* playBtn = FindById("AnimationWindowPlay"))
    {
        if (m_TimelineState.playing && !m_TimelineState.reverse)
            playBtn->AddClass("pressed");
        else
            playBtn->RemoveClass("pressed");
    }
}

void AnimationWindowPanel::UpdatePlayBackwardButtonState()
{
    if (UIElement* btn = FindById("AnimationWindowPlayBackward"))
    {
        if (m_TimelineState.playing && m_TimelineState.reverse)
            btn->AddClass("pressed");
        else
            btn->RemoveClass("pressed");
    }
}

void AnimationWindowPanel::UpdateLoopButtonState()
{
    if (UIElement* loopBtn = FindById("AnimationWindowLoop"))
    {
        if (m_TimelineState.loop)
            loopBtn->AddClass("active");
        else
            loopBtn->RemoveClass("active");
    }
}

void AnimationWindowPanel::UpdateSyncPlayheadButtonState()
{
    if (UIElement* syncBtn = FindById("AnimationWindowSyncPlayhead"))
    {
        if (m_ScrollWithPlayhead)
            syncBtn->AddClass("active");
        else
            syncBtn->RemoveClass("active");
    }
}

void AnimationWindowPanel::UpdateRecordButtonState()
{
    if (UIElement* recordBtn = FindById("AnimationWindowRecord"))
    {
        if (m_Recording)
            recordBtn->AddClass("active");
        else
            recordBtn->RemoveClass("active");
    }
}

void AnimationWindowPanel::UpdateInterpolationButtonState()
{
    auto updateButton = [&](const char* id, bool active)
    {
        if (UIElement* button = FindById(id))
        {
            if (active)
                button->AddClass("active");
            else
                button->RemoveClass("active");
        }
    };

    // Helper to get interpolation mode for a specific key.
    auto getKeyInterp = [this](int channel, float keyTime) -> AnimInterp
    {
        if (!m_CurrentClip || channel < 0 || static_cast<size_t>(channel) >= m_CurrentClip->GetChannels().size())
            return AnimInterp::Linear;
        const AnimChannel& ch = m_CurrentClip->GetChannels()[static_cast<size_t>(channel)];
        const size_t ki = FindKeyIndex(ch.keys, keyTime);
        if (ki >= ch.keys.size()) return ch.interp;
        return ch.keys[ki].hasSegmentInterpOverride ? ch.keys[ki].segmentInterp : ch.interp;
    };

    // Check if we have selected keyframes - if so, determine the dominant interpolation mode.
    // Otherwise fall back to the selected channel's interpolation mode.
    AnimInterp currentInterpolation = AnimInterp::Linear;
    bool hasInterpolation = false;

    // First, check for selected keyframes in Curves view.
    if (m_CurvesGraphView && !m_CurvesGraphView->GetSelectedKeyframes().empty())
    {
        const auto& selectedKeys = m_CurvesGraphView->GetSelectedKeyframes();
        // Count interpolation modes across all selected keys.
        size_t linearCount = 0, stepCount = 0, splineCount = 0;
        for (const auto& sk : selectedKeys)
        {
            const AnimInterp interp = getKeyInterp(sk.Channel, sk.KeyTime);
            if (interp == AnimInterp::Linear)      linearCount++;
            else if (interp == AnimInterp::Step)   stepCount++;
            else if (interp == AnimInterp::CubicSpline) splineCount++;
        }
        // Use the most common interpolation mode.
        if (linearCount >= stepCount && linearCount >= splineCount)
            currentInterpolation = AnimInterp::Linear;
        else if (stepCount >= splineCount)
            currentInterpolation = AnimInterp::Step;
        else
            currentInterpolation = AnimInterp::CubicSpline;
        hasInterpolation = true;
    }
    // Check DopeSheet view for selected keyframes.
    else if (m_DopeSheetView && !m_DopeSheetView->GetSelectedKeyframes().empty())
    {
        const auto& selectedKeys = m_DopeSheetView->GetSelectedKeyframes();
        size_t linearCount = 0, stepCount = 0, splineCount = 0;
        for (const auto& sk : selectedKeys)
        {
            const AnimInterp interp = getKeyInterp(sk.Channel, sk.KeyTime);
            if (interp == AnimInterp::Linear)      linearCount++;
            else if (interp == AnimInterp::Step)   stepCount++;
            else if (interp == AnimInterp::CubicSpline) splineCount++;
        }
        if (linearCount >= stepCount && linearCount >= splineCount)
            currentInterpolation = AnimInterp::Linear;
        else if (stepCount >= splineCount)
            currentInterpolation = AnimInterp::Step;
        else
            currentInterpolation = AnimInterp::CubicSpline;
        hasInterpolation = true;
    }
    // Fall back to selected channel.
    else if (m_CurrentClip && m_SelectedChannel >= 0 &&
        static_cast<size_t>(m_SelectedChannel) < m_CurrentClip->GetChannels().size())
    {
        currentInterpolation = m_CurrentClip->GetChannels()[static_cast<size_t>(m_SelectedChannel)].interp;
        hasInterpolation = true;
    }

    updateButton("AnimationWindowCurveLinear",  hasInterpolation && currentInterpolation == AnimInterp::Linear);
    updateButton("AnimationWindowCurveStepped", hasInterpolation && currentInterpolation == AnimInterp::Step);
    updateButton("AnimationWindowCurveSpline",  hasInterpolation && currentInterpolation == AnimInterp::CubicSpline);
}

void AnimationWindowPanel::UpdateStatsBar()
{
    if (!m_StatsBar) return;

    const std::vector<CurvesGraphView::SelectedKey>* selKeys = m_CurvesGraphView
        ? &m_CurvesGraphView->GetSelectedKeyframes() : nullptr;
    if (!selKeys || selKeys->empty())
    {
        m_StatsBar->AddClass("animationwindow-stats-hidden");
        return;
    }

    m_StatsBar->RemoveClass("animationwindow-stats-hidden");

    // During an active drag use live drag positions to avoid the one-frame delay
    // caused by RefreshEditedClipState() updating the clip asynchronously.
    int dragCh = -1; float dragTime = 0.0f, dragValue = 0.0f;
    const bool dragging = m_CurvesGraphView &&
                          m_CurvesGraphView->GetActiveDragTimeValue(dragCh, dragTime, dragValue);

    const float keyTime = dragging ? dragTime : (*selKeys)[0].KeyTime;
    const int   ch      = dragging ? dragCh   : (*selKeys)[0].Channel;

    if (m_StatsTimeField)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", keyTime);
        m_StatsTimeField->SetValue(std::string(buf));
    }

    if (m_StatsValueField)
    {
        if (dragging)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4g", dragValue);
            m_StatsValueField->SetValue(std::string(buf));
        }
        else if (m_CurrentClip && ch >= 0 &&
                 static_cast<size_t>(ch) < m_CurrentClip->GetChannels().size())
        {
            const AnimChannel& channel = m_CurrentClip->GetChannels()[static_cast<size_t>(ch)];
            for (const AnimKeyframe& key : channel.keys)
            {
                if (std::abs(key.time - keyTime) < 1e-4f)
                {
                    const float val = GetKeyComponentValue(key, channel.path, m_SelectedComponent);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4g", val);
                    m_StatsValueField->SetValue(std::string(buf));
                    break;
                }
            }
        }
    }
}

void AnimationWindowPanel::BuildDopeSheetTrackRows()
{
    if (m_SuppressDSRowRebuild || !m_DopeSheetView || !m_PropertiesTreeProvider || !m_PropertiesTree)
        return;

    std::vector<DopeSheetView::TrackRow> rows;

    const bool noSelection = !m_PropertiesTreeSelection || m_PropertiesTreeSelection->GetSelection().empty();

    // Dimming should only affect unselected component leaves of channels that have a
    // selected component leaf. Selecting a channel root or other tree node should not
    // restrict interaction with unrelated channels in the dope sheet.
    std::unordered_set<int> selectedComponentLeafChannels;
    bool hasSelectedComponentLeaf = false;
    if (!noSelection)
    {
        for (UI::Interaction::ItemId selId : m_PropertiesTreeSelection->GetSelection())
        {
            const TreeId treeId = static_cast<TreeId>(selId);
            if (m_ComponentLeafIndex.count(treeId) > 0)
            {
                const auto bindIt = m_ChannelBindings.find(treeId);
                if (bindIt != m_ChannelBindings.end())
                {
                    selectedComponentLeafChannels.insert(bindIt->second.first);
                    hasSelectedComponentLeaf = true;
                }
            }
        }
    }

    std::function<void(TreeId, bool)> visit = [&](TreeId id, bool inSelectedSubtree)
    {
        if (id == 0) return;
        const bool isSelected = !noSelection && m_PropertiesTreeSelection->IsSelected(static_cast<UI::Interaction::ItemId>(id));

        const auto bindIt = m_ChannelBindings.find(id);
        const int channelIndex = (bindIt != m_ChannelBindings.end()) ? bindIt->second.first : -1;
        const bool isComponentLeaf = m_ComponentLeafIndex.count(id) > 0;

        bool selectable;
        if (noSelection)
        {
            selectable = true;
        }
        else if (hasSelectedComponentLeaf && isComponentLeaf && channelIndex >= 0 &&
                 selectedComponentLeafChannels.count(channelIndex) > 0)
        {
            selectable = inSelectedSubtree || isSelected;
        }
        else
        {
            selectable = true;
        }

        DopeSheetView::TrackRow row;
        if (bindIt != m_ChannelBindings.end())
            row.ChannelIndex = bindIt->second.first;
        row.Selectable = selectable;
        rows.push_back(row);

        if (!m_PropertiesTree->IsExpanded(id))
            return;

        const bool childInSelected = inSelectedSubtree || isSelected;
        const int childCount = m_PropertiesTreeProvider->GetChildCount(id);
        for (int i = 0; i < childCount; ++i)
            visit(m_PropertiesTreeProvider->GetChildId(id, i), childInSelected);
    };

    const int rootCount = m_PropertiesTreeProvider->GetRootCount();
    for (int i = 0; i < rootCount; ++i)
        visit(m_PropertiesTreeProvider->GetRootId(i), false);

    m_DopeSheetView->SetTrackRows(rows);
}

void AnimationWindowPanel::SetActiveView(ActiveView view)
{
    view = NormalizeViewForPanel(view);
    m_ActiveView = view;
    if (m_ViewDropdown)
        m_ViewDropdown->SetSelectedIndex(GetViewDropdownIndex());
    if (m_DopeSheetViewButton)
    {
        if (view == ActiveView::DopeSheet) m_DopeSheetViewButton->AddClass("active");
        else                               m_DopeSheetViewButton->RemoveClass("active");
    }
    if (m_CurvesViewButton)
    {
        if (view == ActiveView::Curves) m_CurvesViewButton->AddClass("active");
        else                            m_CurvesViewButton->RemoveClass("active");
    }
    const char* activeClass = "animationwindow-view-active";
    if (m_DopeSheetView)
        m_DopeSheetView->RemoveClass(activeClass);
    if (m_CurvesGraphView)
        m_CurvesGraphView->RemoveClass(activeClass);
    if (m_TimeCompositeView)
        m_TimeCompositeView->RemoveClass(activeClass);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->RemoveClass(activeClass);
    switch (view)
    {
    case ActiveView::DopeSheet:
        if (m_DopeSheetView) m_DopeSheetView->AddClass(activeClass);
        break;
    case ActiveView::Curves:
        if (m_CurvesGraphView) m_CurvesGraphView->AddClass(activeClass);
        break;
    case ActiveView::TimeComposite:
        if (m_TimeCompositeView) m_TimeCompositeView->AddClass(activeClass);
        break;
    case ActiveView::ClipEditor:
        if (m_LaneClipEditorView) m_LaneClipEditorView->AddClass(activeClass);
        break;
    }

    // Hide toolbar groups that only make sense in the Curves view.
    const bool curvesActive = (view == ActiveView::Curves);
    const bool clipKeyEditingActive = (view == ActiveView::DopeSheet || view == ActiveView::Curves);
    const char* groupHidden = "animationwindow-toolbar-group-hidden";
    static const char* kCurveOnlyGroups[] = {
        "AnimationWindowCurveGroup",
        "AnimationWindowEaseGroup",
        "AnimationWindowCurveViewGroup",
        "AnimationWindowCurveToolsGroup",
    };
    for (const char* id : kCurveOnlyGroups)
    {
        if (UIElement* el = FindById(id))
        {
            if (curvesActive) el->RemoveClass(groupHidden);
            else              el->AddClass(groupHidden);
        }
    }
    // Hide individual curve-only buttons that live in shared groups.
    static const char* kCurveOnlyButtons[] = {
        "AnimationWindowFrameSelectedCurves",
        "AnimationWindowFitHeight",
    };
    for (const char* id : kCurveOnlyButtons)
    {
        if (UIElement* el = FindById(id))
        {
            if (curvesActive) el->RemoveClass(groupHidden);
            else              el->AddClass(groupHidden);
        }
    }
    static const char* kClipKeyEditingOnlyControls[] = {
        "AnimationWindowFrameAll",
    };
    for (const char* id : kClipKeyEditingOnlyControls)
    {
        if (UIElement* el = FindById(id))
        {
            if (clipKeyEditingActive) el->RemoveClass(groupHidden);
            else                      el->AddClass(groupHidden);
        }
    }

    // Clip dropdown is only relevant when editing clip curves/dope sheet.
    if (m_ClipDropdown)
    {
        if (view == ActiveView::DopeSheet || view == ActiveView::Curves)
            m_ClipDropdown->Overrides().Set(Style::Display, DisplayMode::Block);
        else
            m_ClipDropdown->Overrides().Set(Style::Display, DisplayMode::None);
    }
    if (m_PropertiesFilterRow)
    {
        if (view == ActiveView::DopeSheet || view == ActiveView::Curves)
            m_PropertiesFilterRow->Overrides().Set(Style::Display, DisplayMode::Flex);
        else
            m_PropertiesFilterRow->Overrides().Set(Style::Display, DisplayMode::None);
    }

    RefreshLeftPane();
    RefreshClipViewEmptyState();
    RefreshClipDropdown();
    UpdateInterpolationButtonState();
}

AnimationWindowPanel::ActiveView AnimationWindowPanel::GetDefaultActiveView() const
{
    switch (m_PanelKind)
    {
    case PanelKind::Timeline:
        return ActiveView::TimeComposite;
    case PanelKind::ClipEditor:
        return ActiveView::ClipEditor;
    case PanelKind::Animation:
    default:
        return ActiveView::Curves;
    }
}

AnimationWindowPanel::ActiveView AnimationWindowPanel::NormalizeViewForPanel(ActiveView view) const
{
    switch (m_PanelKind)
    {
    case PanelKind::Timeline:
        return ActiveView::TimeComposite;
    case PanelKind::ClipEditor:
        return ActiveView::ClipEditor;
    case PanelKind::Animation:
    default:
        if (view == ActiveView::DopeSheet || view == ActiveView::Curves)
            return view;
        return ActiveView::DopeSheet;
    }
}

int AnimationWindowPanel::GetViewDropdownIndex() const
{
    if (m_PanelKind != PanelKind::Animation)
        return 0;
    return m_ActiveView == ActiveView::DopeSheet ? 0 : 1;
}

std::string AnimationWindowPanel::GetPanelBaseTitle() const
{
    return GetAnimationPanelTitle(m_PanelKind);
}

void AnimationWindowPanel::ApplyTimelineZoom(float scrollY, float mouseX)
{
    const float viewStart = m_TimelineState.viewStart;
    const float viewEnd   = m_TimelineState.viewEnd;
    const float duration  = viewEnd - viewStart;

    // Convert mouse global X to a time position using the content area bounds.
    float mouseTime = viewStart + duration * 0.5f;
    if (m_ContentArea)
    {
        const float contentX = m_ContentArea->GetLayoutX();
        const float contentW = m_ContentArea->GetLayoutWidth();
        if (contentW > 0.0f)
            mouseTime = viewStart + std::clamp((mouseX - contentX) / contentW, 0.0f, 1.0f) * duration;
    }

    const float zoomFactor = (scrollY < 0.0f) ? 1.0f / 1.15f : 1.15f;
    float newDuration = std::clamp(duration * zoomFactor, 0.1f, 1000.0f);

    // Keep the time under the mouse fixed: preserve the ratio of mouseTime within the view.
    const float ratio = duration > 0.0f ? (mouseTime - viewStart) / duration : 0.5f;
    float newStart = mouseTime - ratio * newDuration;
    float newEnd   = newStart + newDuration;
    if (newStart < 0.0f)
    {
        newStart = 0.0f;
        newEnd = newDuration;
    }
    if (newEnd > m_TimelineState.fullEnd)
    {
        newEnd = m_TimelineState.fullEnd;
        newStart = newEnd - newDuration;
        if (newStart < 0.0f)
            newStart = 0.0f;
    }
    m_TimelineState.viewStart = newStart;
    m_TimelineState.viewEnd = newEnd;
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
    if (m_DopeSheetView)
        m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_TimeCompositeView)
        m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
}

void AnimationWindowPanel::ApplyTimelineZoomSliderValue(float value, bool commit)
{
    const float zoom = std::clamp(value, 1.0f, 4.0f);
    const float fullStart = m_TimelineState.fullStart;
    const float fullEnd = std::max(fullStart + 0.1f, m_TimelineState.fullEnd);
    const float baseDuration = std::max(0.1f, fullEnd - fullStart);
    const float targetDuration = std::clamp(baseDuration / zoom, 0.05f, baseDuration);

    if (!m_ZoomSliderDragActive)
    {
        m_ZoomSliderDragActive = true;
        m_ZoomSliderAnchorTime = (m_TimelineState.viewStart + m_TimelineState.viewEnd) * 0.5f;
        m_ZoomSliderAnchorTime = std::clamp(m_ZoomSliderAnchorTime, fullStart, fullEnd);
    }

    float newStart = m_ZoomSliderAnchorTime - targetDuration * 0.5f;
    float newEnd = newStart + targetDuration;
    if (newStart < fullStart)
    {
        newStart = fullStart;
        newEnd = newStart + targetDuration;
    }
    if (newEnd > fullEnd)
    {
        newEnd = fullEnd;
        newStart = newEnd - targetDuration;
    }
    if (newStart < fullStart)
        newStart = fullStart;

    m_TimelineState.viewStart = newStart;
    m_TimelineState.viewEnd = std::max(newStart + 0.05f, newEnd);
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
    if (m_DopeSheetView)
        m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_TimeCompositeView)
        m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);

    if (commit)
        m_ZoomSliderDragActive = false;
}

float AnimationWindowPanel::ComputeTimelineZoomSliderValue() const
{
    const float fullStart = m_TimelineState.fullStart;
    const float fullEnd = std::max(fullStart + 0.1f, m_TimelineState.fullEnd);
    const float baseDuration = std::max(0.1f, fullEnd - fullStart);
    const float viewDuration = std::max(0.05f, m_TimelineState.viewEnd - m_TimelineState.viewStart);
    return std::clamp(baseDuration / viewDuration, 1.0f, 4.0f);
}

void AnimationWindowPanel::SyncTimelineZoomSlider()
{
    if (auto* zoomSlider = dynamic_cast<Slider*>(FindById("AnimationWindowTimelineZoomSlider")))
        zoomSlider->SetValueWithoutNotify(ComputeTimelineZoomSliderValue());
}

float AnimationWindowPanel::ComputeTimelineContentEnd() const
{
    float maxT = 0.0f;
    if (!m_CompositeModel)
        return maxT;

    for (const auto& track : m_CompositeModel->tracks)
    {
        for (const auto& clip : track.clips)
            maxT = std::max(maxT, clip.offsetOnTimeline + std::max(0.0f, clip.outTime - clip.inTime));
        for (const auto& key : track.valueKeys)
            maxT = std::max(maxT, key.time);
        for (const auto& key : track.methodKeys)
            maxT = std::max(maxT, key.time);
        for (const auto& key : track.audioKeys)
            maxT = std::max(maxT, key.time);
        for (const auto& key : track.animationKeys)
            maxT = std::max(maxT, key.time);
        for (const auto& marker : track.markers)
            maxT = std::max(maxT, marker.time);
    }

    return maxT;
}

float AnimationWindowPanel::ComputeTimelineFullRangeEnd() const
{
    constexpr float kDefaultTimelineDuration = 10.0f;
    const float clipDuration = m_CurrentClipAsset
        ? m_CurrentClipAsset->GetDuration()
        : (m_CurrentClip ? m_CurrentClip->GetDuration() : 0.0f);
    const bool useCompositeContent = (m_ActiveView == ActiveView::TimeComposite || m_PanelKind == PanelKind::Timeline);
    const float contentEnd = useCompositeContent ? ComputeTimelineContentEnd() : 0.0f;
    return std::max({kDefaultTimelineDuration, clipDuration, contentEnd,
                     m_TimelineState.viewEnd, m_TimelineState.rangeEnd,
                     m_TimelineState.fullStart + 0.1f});
}

void AnimationWindowPanel::FrameSelected()
{
    float centerTime = m_TimelineState.currentTime;
    if (m_ActiveView == ActiveView::DopeSheet && m_DopeSheetView)
    {
        float keyTime = m_DopeSheetView->GetSelectedKeyframeTime();
        if (keyTime >= 0.0f)
            centerTime = keyTime;
    }
    const float frameDuration = 2.0f;
    float newStart = centerTime - frameDuration * 0.5f;
    float newEnd = centerTime + frameDuration * 0.5f;
    if (newStart < 0.0f)
    {
        newStart = 0.0f;
        newEnd = std::min(frameDuration, newEnd - newStart);
    }
    newEnd = std::max(newEnd, newStart + 0.1f);
    m_TimelineState.viewStart = newStart;
    m_TimelineState.viewEnd = newEnd;
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
    if (m_DopeSheetView)
        m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_TimeCompositeView)
        m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);

    // Frame vertically in Curves view
    if (m_CurvesGraphView && m_CurrentClip && !m_VisibleChannels.empty())
    {
        const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
        float vMin = std::numeric_limits<float>::infinity();
        float vMax = -std::numeric_limits<float>::infinity();
        for (int chIdx : m_VisibleChannels)
        {
            if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
            const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
            const uint32 maxComp = (ch.path == AnimPath::Rotation) ? 4u : 3u;
            for (const AnimKeyframe& key : ch.keys)
            {
                if (m_SelectedComponent < maxComp)
                {
                    const float v = GetKeyComponentValue(key, ch.path, m_SelectedComponent);
                    vMin = std::min(vMin, v);
                    vMax = std::max(vMax, v);
                }
            }
        }
        if (std::isfinite(vMin) && std::isfinite(vMax))
        {
            if (vMax - vMin < 1e-4f) { vMin -= 1.0f; vMax += 1.0f; }
            const float vp = (vMax - vMin) * 0.12f;
            m_CurvesGraphView->SetValueRange(vMin - vp, vMax + vp);
        }
    }
}

void AnimationWindowPanel::FrameAll()
{
    float viewEnd = 10.0f;
    if (m_CurrentClip)
    {
        viewEnd = std::max(m_CurrentClip->GetDuration(), 0.1f);
    }
    else if (m_CompositeModel && !m_CompositeModel->tracks.empty())
    {
        float maxT = 0.0f;
        for (const auto& track : m_CompositeModel->tracks)
        {
            for (const auto& clip : track.clips)
            {
                float endT = clip.offsetOnTimeline + (clip.outTime - clip.inTime);
                if (endT > maxT)
                    maxT = endT;
            }
            for (const auto& key : track.valueKeys)
                maxT = std::max(maxT, key.time);
            for (const auto& key : track.methodKeys)
                maxT = std::max(maxT, key.time);
            for (const auto& key : track.audioKeys)
                maxT = std::max(maxT, key.time);
            for (const auto& key : track.animationKeys)
                maxT = std::max(maxT, key.time);
            for (const auto& marker : track.markers)
                maxT = std::max(maxT, marker.time);
        }
        viewEnd = std::max(maxT, 0.1f);
    }
    const float padding = viewEnd * 0.05f;
    m_TimelineState.viewStart = 0.0f;
    m_TimelineState.viewEnd = viewEnd + padding;
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
    if (m_DopeSheetView)
        m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_TimeCompositeView)
        m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_LaneClipEditorView)
        m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);

    if (m_CurvesGraphView && m_CurrentClip && !m_VisibleChannels.empty())
    {
        const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
        float vMin = std::numeric_limits<float>::infinity();
        float vMax = -std::numeric_limits<float>::infinity();
        for (int chIdx : m_VisibleChannels)
        {
            if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
            const AnimChannel& ch = channels[static_cast<size_t>(chIdx)];
            const uint32 maxComp = (ch.path == AnimPath::Rotation) ? 4u : 3u;
            for (const AnimKeyframe& key : ch.keys)
            {
                if (m_SelectedComponent < maxComp)
                {
                    const float v = GetKeyComponentValue(key, ch.path, m_SelectedComponent);
                    vMin = std::min(vMin, v);
                    vMax = std::max(vMax, v);
                }
            }
        }
        if (std::isfinite(vMin) && std::isfinite(vMax))
        {
            if (vMax - vMin < 1e-4f) { vMin -= 1.0f; vMax += 1.0f; }
            const float vp = (vMax - vMin) * 0.12f;
            m_CurvesGraphView->SetValueRange(vMin - vp, vMax + vp);
        }
    }
}

bool AnimationWindowPanel::FrameSelectionBounds()
{
    if (!m_CurrentClip) return false;

    struct SelEntry { int channel; float keyTime; };
    std::vector<SelEntry> selEntries;
    if (m_CurvesGraphView)
        for (const auto& s : m_CurvesGraphView->GetSelectedKeyframes())
            selEntries.push_back({s.Channel, s.KeyTime});
    if (m_DopeSheetView)
        for (const auto& s : m_DopeSheetView->GetSelectedKeyframes())
            selEntries.push_back({s.Channel, s.KeyTime});

    if (selEntries.empty()) return false;

    const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
    float tMin =  std::numeric_limits<float>::infinity();
    float tMax = -std::numeric_limits<float>::infinity();
    float vMin =  std::numeric_limits<float>::infinity();
    float vMax = -std::numeric_limits<float>::infinity();
    constexpr float kEps = 1e-4f;
    for (const auto& sel : selEntries)
    {
        if (sel.channel < 0 || static_cast<size_t>(sel.channel) >= channels.size()) continue;
        const AnimChannel& ch = channels[static_cast<size_t>(sel.channel)];
        const uint32 maxComp = (ch.path == AnimPath::Rotation) ? 4u : 3u;
        auto accumulateKey = [&](const AnimKeyframe& key)
        {
            tMin = std::min(tMin, key.time);
            tMax = std::max(tMax, key.time);
            if (m_SelectedComponent < maxComp)
            {
                float v = 0.0f;
                if (ch.path == AnimPath::Translation) v = key.translation[m_SelectedComponent];
                else if (ch.path == AnimPath::Rotation) v = key.rotation[m_SelectedComponent];
                else v = key.scale[m_SelectedComponent];
                vMin = std::min(vMin, v);
                vMax = std::max(vMax, v);
            }
        };
        for (size_t keyIndex = 0; keyIndex < ch.keys.size(); ++keyIndex)
        {
            if (std::abs(ch.keys[keyIndex].time - sel.keyTime) > kEps) continue;
            if (m_ActiveView == ActiveView::Curves && keyIndex > 0)
                accumulateKey(ch.keys[keyIndex - 1]);
            accumulateKey(ch.keys[keyIndex]);
            if (m_ActiveView == ActiveView::Curves && keyIndex + 1 < ch.keys.size())
                accumulateKey(ch.keys[keyIndex + 1]);
        }
    }
    if (!std::isfinite(tMin)) return false;

    const float tp = std::max((tMax - tMin) * 0.08f, 0.1f);
    m_TimelineState.viewStart = std::max(0.0f, tMin - tp);
    m_TimelineState.viewEnd   = tMax + tp;
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
    if (m_DopeSheetView)     m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView)   m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_TimeCompositeView) m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_LaneClipEditorView) m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);

    if (m_CurvesGraphView && m_ActiveView == ActiveView::Curves && std::isfinite(vMin))
    {
        if (vMax - vMin < 1e-4f) { vMin -= 1.0f; vMax += 1.0f; }
        const float vp = (vMax - vMin) * 0.12f;
        m_CurvesGraphView->SetValueRange(vMin - vp, vMax + vp);
    }
    return true;
}

std::filesystem::path AnimationWindowPanel::GetCurrentMenuAnimationAssetPath() const
{
    if (m_ActiveView == ActiveView::TimeComposite)
    {
        if (!m_CurrentTimelinePath.empty())
            return m_CurrentTimelinePath;
        if (m_CurrentTimelineAsset)
            return m_CurrentTimelineAsset->GetPath();
    }
    if (m_ActiveView == ActiveView::ClipEditor)
    {
        if (!m_CurrentClipSetPath.empty())
            return m_CurrentClipSetPath;
        if (m_CurrentClipSetAsset)
            return m_CurrentClipSetAsset->GetPath();
    }
    if (!m_CurrentClipPath.empty())
        return m_CurrentClipPath;
    if (!m_CurrentTimelinePath.empty())
        return m_CurrentTimelinePath;
    if (m_CurrentTimelineAsset)
        return m_CurrentTimelineAsset->GetPath();
    if (!m_CurrentClipSetPath.empty())
        return m_CurrentClipSetPath;
    if (m_CurrentClipSetAsset)
        return m_CurrentClipSetAsset->GetPath();
    return {};
}

bool AnimationWindowPanel::OpenAnimationAssetPath(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    const std::string extension = NormalizeExtension(path.extension().string());
    bool opened = false;
    if (extension == ".timeline")
    {
        opened = OpenTimeline(path);
        if (opened)
            SetActiveView(ActiveView::TimeComposite);
    }
    else if (extension == ".clipset")
    {
        opened = OpenClipSet(path);
        if (opened)
            SetActiveView(ActiveView::ClipEditor);
    }
    else
    {
        opened = OpenAnimation(path);
        if (opened && (m_ActiveView == ActiveView::TimeComposite || m_ActiveView == ActiveView::ClipEditor))
            SetActiveView(ActiveView::DopeSheet);
    }

    if (opened)
    {
        RefreshClipDropdown();
        UpdateTitle();
        if (m_OnRevealInAssetsPanel)
            m_OnRevealInAssetsPanel(path);
    }
    return opened;
}

bool AnimationWindowPanel::SaveCurrentMenuAnimationAssetIfDirty()
{
    if (m_ActiveView == ActiveView::TimeComposite)
        return !m_TimelineDirty || SaveCurrentTimeline();
    if (m_ActiveView == ActiveView::ClipEditor)
        return !m_ClipSetDirty || SaveCurrentClipSet();
    return !m_Dirty || SaveCurrentClip();
}

void AnimationWindowPanel::DuplicateCurrentAnimationAsset()
{
    const std::filesystem::path source = GetCurrentMenuAnimationAssetPath();
    if (source.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec))
        return;
    if (!SaveCurrentMenuAnimationAssetIfDirty())
        return;

    const std::vector<std::uint8_t> bytes = ReadBinaryFile(source);
    const auto sourceSize = std::filesystem::file_size(source, ec);
    if (bytes.empty() && (!ec && sourceSize > 0))
        return;

    std::filesystem::path destination = Editor::MakeUniqueFilePath(
        source.parent_path(),
        source.stem().string() + " Copy",
        source.extension().string());
    if (destination.empty())
        return;

    AssetManager* assets = m_Context ? m_Context->Assets : nullptr;
    auto onChanged = [this, destination]()
    {
        (void)OpenAnimationAssetPath(destination);
    };
    if (m_UndoRedo)
    {
        m_UndoRedo->Execute(std::make_unique<CopyAnimationAssetFileCommand>(
            source, destination, bytes, assets, std::move(onChanged)));
    }
    else
    {
        if (WriteBinaryFile(destination, bytes))
        {
            RegisterAnimationAssetPath(assets, destination);
            onChanged();
        }
    }
}

void AnimationWindowPanel::RenameCurrentAnimationAsset()
{
    const std::filesystem::path source = GetCurrentMenuAnimationAssetPath();
    if (source.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec))
        return;

    if (!m_RenameAnimationModal)
    {
        auto modal = std::make_unique<RenameLayoutModal>();
        m_RenameAnimationModal = modal.get();
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal));
    }

    m_RenameAnimationModal->SetOnCommit([this, source](const std::string& rawName)
    {
        std::string name = rawName;
        const size_t begin = name.find_first_not_of(" \t\r\n");
        const size_t end = name.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return;
        name = name.substr(begin, end - begin + 1);
        std::filesystem::path typed(name);
        if (typed.has_parent_path())
            return;

        const std::string extension = source.extension().string();
        if (NormalizeExtension(typed.extension().string()) == NormalizeExtension(extension))
            name = typed.stem().string();

        std::filesystem::path destination = source.parent_path() / (name + extension);
        destination = destination.lexically_normal();
        if (destination == source)
            return;
        std::error_code existsEc;
        if (std::filesystem::exists(destination, existsEc))
            return;
        if (!SaveCurrentMenuAnimationAssetIfDirty())
            return;

        AssetManager* assets = m_Context ? m_Context->Assets : nullptr;
        auto onPathChanged = [this](const std::filesystem::path& path)
        {
            (void)OpenAnimationAssetPath(path);
        };
        if (m_UndoRedo)
        {
            m_UndoRedo->Execute(std::make_unique<RenameAnimationAssetFileCommand>(
                source, destination, assets, std::move(onPathChanged)));
        }
        else
        {
            std::error_code ec;
            std::filesystem::rename(source, destination, ec);
            if (!ec)
            {
                RenameAnimationAssetPathInRegistry(assets, source, destination);
                onPathChanged(destination);
            }
        }
    });
    m_RenameAnimationModal->SetOnCancel({});
    m_RenameAnimationModal->Show("Rename Animation Asset", source.stem().string());
}

void AnimationWindowPanel::RemoveCurrentAnimationAsset()
{
    const std::filesystem::path source = GetCurrentMenuAnimationAssetPath();
    if (source.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec))
        return;

    if (!m_RemoveAnimationConfirmModal)
    {
        auto modal = std::make_unique<ConfirmActionModal>();
        m_RemoveAnimationConfirmModal = modal.get();
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal));
    }

    m_RemoveAnimationConfirmModal->SetOnConfirm([this, source]()
    {
        const std::vector<std::uint8_t> bytes = ReadBinaryFile(source);
        std::error_code sizeEc;
        const auto sourceSize = std::filesystem::file_size(source, sizeEc);
        if (bytes.empty() && (!sizeEc && sourceSize > 0))
            return;

        auto onDeletedChanged = [this, source](bool deleted)
        {
            if (deleted)
            {
                if (m_CurrentClipPath == source)
                {
                    m_CurrentClipAsset.reset();
                    m_CurrentClipPath.clear();
                    m_PreviewModelPath.clear();
                    ClearDirty();
                    SetCurrentClip(nullptr);
                }
                if (m_CurrentTimelinePath == source)
                {
                    m_CurrentTimelineAsset.reset();
                    m_CurrentTimelinePath.clear();
                    m_TimelineDirty = false;
                }
                if (m_CurrentClipSetPath == source)
                {
                    m_CurrentClipSetAsset.reset();
                    m_CurrentClipSetPath.clear();
                    m_ClipSetDirty = false;
                }
                RefreshClipDropdown();
                UpdateTitle();
                ShowAnimationManagerInspector();
            }
            else
            {
                (void)OpenAnimationAssetPath(source);
            }
        };

        AssetManager* assets = m_Context ? m_Context->Assets : nullptr;
        if (m_UndoRedo)
        {
            m_UndoRedo->Execute(std::make_unique<DeleteAnimationAssetFileCommand>(
                source, bytes, assets, std::move(onDeletedChanged)));
        }
        else
        {
            std::error_code ec;
            (void)std::filesystem::remove(source, ec);
            if (!ec)
            {
                UnregisterAnimationAssetPath(assets, source);
                onDeletedChanged(true);
            }
        }
    });
    m_RemoveAnimationConfirmModal->SetOnCancel({});
    m_RemoveAnimationConfirmModal->Show(
        "Remove Animation Asset",
        "Remove \"" + source.filename().string() + "\" from disk?\nThis can be undone from the editor undo stack.",
        "Remove");
}

void AnimationWindowPanel::ShowAnimationManagerInspector()
{
    if (!m_OnTimelineInspectorRequested)
        return;

    const std::filesystem::path path = GetCurrentMenuAnimationAssetPath();
    m_OnTimelineInspectorRequested("Animation", [this, path](UIElement* parent)
    {
        if (!parent)
            return;

        auto addLabel = [parent](const std::string& text, uint32_t color = 0xFFD6D6D6)
        {
            auto label = std::make_unique<Label>();
            label->SetText(text);
            label->Overrides()
                .Set(Style::Color, color)
                .Set(Style::FontSize, StyleLength::Px(12.0f))
                .Set(Style::WhiteSpaceProp, WhiteSpace::Pre);
            parent->AddChild(std::move(label));
        };
        auto addButton = [parent](const std::string& text, std::function<void()> fn)
        {
            auto button = std::make_unique<Button>();
            button->SetText(text);
            button->RegisterEventHandler(kEventButtonClick, [fn = std::move(fn)](UIEvent&)
            {
                if (fn) fn();
            });
            parent->AddChild(std::move(button));
        };

        parent->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::PaddingTop, StyleLength::Px(8.0f))
            .Set(Style::PaddingRight, StyleLength::Px(8.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(8.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(8.0f));

        addLabel("Asset", 0xFFFFFFFF);
        addLabel(path.empty() ? "(None)" : path.filename().string());
        addLabel(path.empty() ? "" : path.string(), 0xFF9C9C9C);
        addButton("New...", [this]()
        {
            if (auto* btn = dynamic_cast<Button*>(FindById("AnimationWindowNewAnimation")))
                btn->TriggerClick();
        });
        addButton("Duplicate...", [this]() { DuplicateCurrentAnimationAsset(); });
        addButton("Rename...", [this]() { RenameCurrentAnimationAsset(); });
        addButton("Edit Transitions...", [this]() { ShowAnimationTransitionsInspector(); });
        addButton("Open in Inspector", [this]()
        {
            const std::filesystem::path inspectPath = GetCurrentMenuAnimationAssetPath();
            if (m_OnRevealInAssetsPanel && !inspectPath.empty())
                m_OnRevealInAssetsPanel(inspectPath);
        });
        addButton("Remove", [this]() { RemoveCurrentAnimationAsset(); });
    });
}

void AnimationWindowPanel::ShowAnimationTransitionsInspector()
{
    if (!m_OnTimelineInspectorRequested)
        return;

    const std::filesystem::path currentPath = GetCurrentMenuAnimationAssetPath();
    m_OnTimelineInspectorRequested("Animation Transitions", [this, currentPath](UIElement* parent)
    {
        if (!parent)
            return;
        parent->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::PaddingTop, StyleLength::Px(8.0f))
            .Set(Style::PaddingRight, StyleLength::Px(8.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(8.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(8.0f));

        auto title = std::make_unique<Label>();
        title->SetText("AnimationController transitions");
        title->Overrides().Set(Style::Color, 0xFFFFFFFF).Set(Style::FontSize, StyleLength::Px(13.0f)).Set(Style::FontWeight, 600);
        parent->AddChild(std::move(title));

        auto body = std::make_unique<Label>();
        body->SetText("AnimationController assets are parsed but states and transitions are not evaluated yet. Clip and Timeline playback on the Animator still work.");
        body->Overrides()
            .Set(Style::Color, 0xFFD6D6D6)
            .Set(Style::FontSize, StyleLength::Px(12.0f))
            .Set(Style::WhiteSpaceProp, WhiteSpace::Normal);
        parent->AddChild(std::move(body));

        auto create = std::make_unique<Button>();
        create->SetText("Create Controller");
        create->RegisterEventHandler(kEventButtonClick, [this, currentPath](UIEvent&)
        {
            const std::filesystem::path dir = !currentPath.empty()
                ? currentPath.parent_path()
                : (m_Context ? m_Context->AssetsRoot : std::filesystem::path{});
            if (dir.empty())
                return;
            AssetManager* assets = m_Context ? m_Context->Assets : nullptr;
            auto result = Editor::CreateAssetFile(dir, "NewAnimationController", ".animcontroller",
                [](const std::string&) {
                    return "{\n"
                           "  \"schemaVersion\": 1,\n"
                           "  \"assetType\": \"AnimationController\",\n"
                           "  \"entryState\": \"\",\n"
                           "  \"parameters\": [],\n"
                           "  \"states\": [],\n"
                           "  \"transitions\": []\n"
                           "}\n";
                },
                "Create Animation Controller",
                m_UndoRedo,
                assets);
            if (!result.path.empty() && m_OnRevealInAssetsPanel)
                m_OnRevealInAssetsPanel(result.path);
        });
        parent->AddChild(std::move(create));
    });
}

bool AnimationWindowPanel::OpenTimeline(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    auto asset = std::make_shared<TimelineAsset>(GUID::Null(), path);
    if (!asset->Load())
        return false;

    if (!m_CompositeModel)
        m_CompositeModel = std::make_unique<TimeCompositeModel>();
    std::string loadError;
    if (!Editor::LoadCompositeModelFromTimelineDocument(asset->GetDocument(), *m_CompositeModel, &loadError))
        return false;

    m_CurrentTimelineAsset = asset;
    m_CurrentTimelinePath  = path;
    m_TimelineDirty        = false;

    m_SelectedCompositeTrack = static_cast<size_t>(-1);
    m_SelectedCompositeClip  = static_cast<size_t>(-1);
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->ClearSelection();
        m_TimeCompositeView->SetModel(m_CompositeModel.get());
        m_TimeCompositeView->MarkDirty(VisualDirty);
    }
    return true;
}

bool AnimationWindowPanel::OpenClipSet(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    auto asset = std::make_shared<ClipSetAsset>(GUID::Null(), path);
    if (!asset->Load())
        return false;

    if (!m_LaneClipModel)
        m_LaneClipModel = std::make_unique<LaneClipModel>();
    if (!DeserializeLaneClipModel(asset->GetDocument(), *m_LaneClipModel))
        return false;

    m_CurrentClipSetAsset = asset;
    m_CurrentClipSetPath  = path;
    m_ClipSetDirty        = false;

    m_SelectedLane     = static_cast<size_t>(-1);
    m_SelectedLaneClip = static_cast<size_t>(-1);
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->ClearSelection();
        m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
        m_LaneClipEditorView->MarkDirty(VisualDirty);
    }
    return true;
}

bool AnimationWindowPanel::OpenAnimation(const std::filesystem::path& path, uint32 selectedAnimationIndex)
{
    if (path.empty())
        return false;

    if (m_Dirty && m_CurrentClipAsset && m_UnsavedChangesModal)
    {
        m_PendingOpenPath  = path;
        m_PendingOpenIndex = selectedAnimationIndex;
        const std::string filename = m_CurrentClipPath.empty()
            ? "Untitled.anim"
            : m_CurrentClipPath.filename().string();
        m_UnsavedChangesModal->Show(
            "Unsaved Changes",
            "\"" + filename + "\" has unsaved changes.\nSave before opening a new animation?");
        return false;
    }

    return DoOpenAnimation(path, selectedAnimationIndex);
}

bool AnimationWindowPanel::DoOpenAnimation(const std::filesystem::path& path, uint32 selectedAnimationIndex)
{
    if (path.empty())
        return false;

    std::filesystem::path clipPath = path;
    auto clip = std::make_shared<AnimationClip>(GUID::Null(), clipPath);
    clip->SetSelectedAnimationIndex(selectedAnimationIndex);
    if (!clip->Load())
        return false;

    const std::string clipExtension = NormalizeExtension(clipPath.extension().string());
    if (clipExtension != ".anim" && clipExtension != ".animation")
    {
        clip->SetSourceInfo(clipPath, selectedAnimationIndex);
        m_PreviewModelPath = clipPath;
    }
    else if (!clip->GetSourcePath().empty())
    {
        m_PreviewModelPath = clip->GetSourcePath();
    }
    else
    {
        m_PreviewModelPath.clear();
    }

    m_NewClip.reset();
    m_CurrentClipAsset = clip;
    m_CurrentClipPath = clipPath;
    m_SelectedChannel = -1;
    m_SelectedComponent = 0u;
    m_SelectedTreeId = 0;
    m_VisibleChannels.clear();
    ClearDirty();
    SetCurrentClip(m_CurrentClipAsset.get());
    UpdatePlaybackRangeFromCurrentClip();
    m_TimelineState.viewStart = m_TimelineState.rangeStart;
    m_TimelineState.viewEnd   = m_TimelineState.rangeEnd;
    RefreshPropertyTree();
    RefreshSequencingModels();
    RefreshChannelSelection();
    m_LastSelectionSnapshot = CaptureSelectionSnapshot();
    RefreshClipDropdown();
    return true;
}

bool AnimationWindowPanel::SaveCurrentClip()
{
    if (!m_CurrentClipAsset)
        return false;

    if (!EnsureEditableClip())
        return false;

    std::filesystem::path savePath = m_CurrentClipPath;
    if (savePath.empty())
        savePath = GetSuggestedEditablePath();

    return SaveClipToPath(savePath);
}

namespace
{
bool WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& doc);
} // namespace

bool AnimationWindowPanel::SaveCurrentTimeline()
{
    if (!m_CurrentTimelineAsset || !m_CompositeModel)
        return false;

    std::filesystem::path savePath = m_CurrentTimelinePath;
    if (savePath.empty())
        savePath = m_CurrentTimelineAsset->GetPath();
    if (savePath.empty())
        return false;

    const nlohmann::json doc = Editor::SaveTimelineDocumentFromCompositeModel(*m_CompositeModel);
    if (!WriteJsonFile(savePath, doc))
        return false;

    m_CurrentTimelinePath = savePath;
    m_CurrentTimelineAsset->SetDocument(doc);
    m_TimelineDirty = false;
    if (!HasUnsavedChanges())
        ClearDirty();
    else
        UpdateTitle();
    return true;
}

bool AnimationWindowPanel::SaveCurrentClipSet()
{
    if (!m_CurrentClipSetAsset || !m_LaneClipModel)
        return false;

    std::filesystem::path savePath = m_CurrentClipSetPath;
    if (savePath.empty())
        savePath = m_CurrentClipSetAsset->GetPath();
    if (savePath.empty())
        return false;

    const nlohmann::json doc = SerializeLaneClipModel(*m_LaneClipModel);
    if (!WriteJsonFile(savePath, doc))
        return false;

    m_CurrentClipSetPath = savePath;
    m_CurrentClipSetAsset->SetDocument(doc);
    m_ClipSetDirty = false;
    if (!HasUnsavedChanges())
        ClearDirty();
    else
        UpdateTitle();
    return true;
}

void AnimationWindowPanel::UpdateTitle()
{
    std::string title = GetPanelBaseTitle();
    if (m_PanelKind == PanelKind::Timeline && !m_CurrentTimelinePath.empty())
    {
        title += ": " + m_CurrentTimelinePath.filename().string();
    }
    else if (m_PanelKind == PanelKind::Timeline && m_CurrentTimelineAsset)
    {
        title += ": Untitled.timeline";
    }
    else if (m_PanelKind == PanelKind::ClipEditor && !m_CurrentClipSetPath.empty())
    {
        title += ": " + m_CurrentClipSetPath.filename().string();
    }
    else if (m_PanelKind == PanelKind::ClipEditor && m_CurrentClipSetAsset)
    {
        title += ": Untitled.clipset";
    }
    else if (!m_CurrentClipPath.empty())
    {
        title += ": " + m_CurrentClipPath.filename().string();
    }
    else if (m_CurrentClipAsset)
    {
        title += ": Untitled.anim";
    }

    if (HasUnsavedChanges())
        title += " *";

    SetTitle(title);
}

void AnimationWindowPanel::MarkDirty()
{
    m_Dirty = true;
    UpdateTitle();
    if (m_UnsavedIndicator)
        m_UnsavedIndicator->RemoveClass("hidden");
}

void AnimationWindowPanel::ClearDirty()
{
    m_Dirty = false;
    UpdateTitle();
    if (m_UnsavedIndicator)
    {
        if (HasUnsavedChanges())
            m_UnsavedIndicator->RemoveClass("hidden");
        else
            m_UnsavedIndicator->AddClass("hidden");
    }
}

bool AnimationWindowPanel::EnsureEditableClip()
{
    if (!m_CurrentClipAsset)
        return false;

    if (m_CurrentClipAsset->IsEditable())
        return true;

    m_NewClip = std::make_shared<AnimationClip>(GUID::Null(), std::filesystem::path());
    m_NewClip->CopyFrom(*m_CurrentClipAsset);
    m_NewClip->SetSourceInfo(m_CurrentClipPath, m_CurrentClipAsset->GetSelectedAnimationIndex());
    m_CurrentClipAsset = m_NewClip;
    m_CurrentClipPath = GetSuggestedEditablePath();
    SetCurrentClip(m_CurrentClipAsset.get());
    RefreshPropertyTree();
    RefreshSequencingModels();
    MarkDirty();
    return true;
}

bool AnimationWindowPanel::SaveClipToPath(const std::filesystem::path& path)
{
    if (!m_CurrentClipAsset || path.empty())
        return false;

    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);

    if (!m_CurrentClipAsset->SaveToPath(path))
        return false;

    m_CurrentClipPath = path;
    ClearDirty();
    return true;
}

namespace
{
bool WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& doc)
{
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << doc.dump(2);
    return out.good();
}
} // namespace

void AnimationWindowPanel::CreateNewTimelineAtPath(const std::filesystem::path& path)
{
    if (path.empty()) return;

    if (!m_CompositeModel)
        m_CompositeModel = std::make_unique<TimeCompositeModel>();
    *m_CompositeModel = TimeCompositeModel{};

    nlohmann::json doc = SerializeTimeCompositeModel(*m_CompositeModel);
    if (!WriteJsonFile(path, doc)) return;

    auto asset = std::make_shared<TimelineAsset>(GUID::Null(), path);
    asset->SetDocument(doc);
    m_CurrentTimelineAsset = asset;
    m_CurrentTimelinePath  = path;
    m_TimelineDirty        = false;

    m_SelectedCompositeTrack = static_cast<size_t>(-1);
    m_SelectedCompositeClip  = static_cast<size_t>(-1);
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->ClearSelection();
        m_TimeCompositeView->SetModel(m_CompositeModel.get());
        m_TimeCompositeView->MarkDirty(VisualDirty);
    }
    RefreshSequencerSidebar();
}

void AnimationWindowPanel::CreateNewClipSetAtPath(const std::filesystem::path& path)
{
    if (path.empty()) return;

    if (!m_LaneClipModel)
        m_LaneClipModel = std::make_unique<LaneClipModel>();
    *m_LaneClipModel = LaneClipModel{};

    nlohmann::json doc = SerializeLaneClipModel(*m_LaneClipModel);
    if (!WriteJsonFile(path, doc)) return;

    auto asset = std::make_shared<ClipSetAsset>(GUID::Null(), path);
    asset->SetDocument(doc);
    m_CurrentClipSetAsset = asset;
    m_CurrentClipSetPath  = path;
    m_ClipSetDirty        = false;

    m_SelectedLane     = static_cast<size_t>(-1);
    m_SelectedLaneClip = static_cast<size_t>(-1);
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->ClearSelection();
        m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
        m_LaneClipEditorView->MarkDirty(VisualDirty);
    }
    RefreshSequencerSidebar();
}

void AnimationWindowPanel::CreateNewAnimationAtPath(const std::filesystem::path& path)
{
    // Create a new animation clip with the chosen path
    m_NewClip = std::make_shared<AnimationClip>(GUID::Null(), path);
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1{};
    k1.time = 1.0f;
    k1.rotation[3] = 1.0f;
    k1.scale[0] = k1.scale[1] = k1.scale[2] = 1.0f;
    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.path = AnimPath::Translation;
    ch.keys = {k0, k1};
    m_NewClip->SetChannelsAndDurationForTest({ch}, 1.0f);
    m_CurrentClipAsset = m_NewClip;
    m_CurrentClipPath = path;

    // Save the file immediately
    (void)SaveClipToPath(path);
    m_PreviewModelPath.clear();
    SetCurrentClip(m_CurrentClipAsset.get());
    UpdatePlaybackRangeFromCurrentClip();
    m_TimelineState.viewStart = m_TimelineState.rangeStart;
    m_TimelineState.viewEnd   = m_TimelineState.rangeEnd;
    RefreshPropertyTree();
    RefreshSequencingModels();
    RefreshClipDropdown();
}

void AnimationWindowPanel::RefreshPropertyTree()
{
    if (!m_PropertiesTree)
        return;

    std::vector<AnimationTreeNode> nodes;
    m_ChannelBindings.clear();
    m_TreeNodeChannels.clear();
    m_ComponentLeafIndex.clear();
    m_RowToTreeId.clear();
    m_LockedTreeIds.clear();
    m_PinnedTreeIds.clear();
    m_TreeColorTags.clear();
    m_BoneTreeIds.clear();

    TreeId nextId = 1;
    auto addNode = [&nodes, &nextId](TreeId parentId, const String& label, bool expandable) -> TreeId
    {
        const TreeId id = nextId++;
        nodes.push_back({id, parentId, label, expandable, {}});
        if (parentId != 0)
        {
            for (AnimationTreeNode& node : nodes)
            {
                if (node.Id == parentId)
                {
                    node.Children.push_back(id);
                    break;
                }
            }
        }
        return id;
    };

    if (m_CurrentClip)
    {
        const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
        String objectLabel = "Object";
        const std::filesystem::path objectPath = !m_PreviewModelPath.empty()
            ? m_PreviewModelPath
            : (!m_CurrentClip->GetSourcePath().empty() ? m_CurrentClip->GetSourcePath() : m_CurrentClipPath);
        if (!objectPath.empty() && !objectPath.stem().string().empty())
            objectLabel = objectPath.stem().string();

        SourceHierarchyData sourceHierarchy;
        bool hasSourceHierarchy = false;
        std::vector<std::filesystem::path> hierarchySourceCandidates;
        if (!m_PreviewModelPath.empty())
            hierarchySourceCandidates.push_back(m_PreviewModelPath);
        if (!m_CurrentClip->GetSourcePath().empty())
            hierarchySourceCandidates.push_back(m_CurrentClip->GetSourcePath());
        if (!m_CurrentClipPath.empty())
            hierarchySourceCandidates.push_back(m_CurrentClipPath);

        auto& assetManager = EngineCore::GetInstance().GetAssetManager();
        for (const std::filesystem::path& candidatePath : hierarchySourceCandidates)
        {
            if (candidatePath.empty())
            continue;

            std::filesystem::path resolvedPath;
            if (!m_CurrentClipPath.empty())
                resolvedPath = assetManager.ResolveAssetPathFromReference(candidatePath, m_CurrentClipPath);
            if (resolvedPath.empty())
                resolvedPath = assetManager.ResolveAssetPath(candidatePath);

            if (BuildSourceHierarchy(resolvedPath, *m_CurrentClip, sourceHierarchy))
            {
                hasSourceHierarchy = true;
                break;
            }
        }

        auto addChannelChildren = [&](TreeId parentId, int channelIndex)
        {
            if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
                return;

            const AnimChannel& channel = channels[static_cast<size_t>(channelIndex)];
            const String channelLabel = channel.path == AnimPath::Translation
                ? "Translation"
                : (channel.path == AnimPath::Rotation ? "Rotation" : (channel.path == AnimPath::MorphWeight ? "Morph Weight" : "Scale"));
            const TreeId channelId = addNode(parentId, channelLabel, true);
            m_ChannelBindings.emplace(channelId, std::make_pair(channelIndex, 0u));
            m_TreeNodeChannels[channelId] = {channelIndex};

            const uint32 componentCount = AnimChannelComponentCount(channel);
            const char* labels[4] = {"Value", "Y", "Z", "W"};
            for (uint32 componentIndex = 0; componentIndex < componentCount; ++componentIndex)
            {
                const TreeId componentId = addNode(channelId, labels[componentIndex], false);
                m_ChannelBindings.emplace(componentId, std::make_pair(channelIndex, componentIndex));
                m_TreeNodeChannels[componentId] = {channelIndex};
                m_ComponentLeafIndex.emplace(componentId, componentIndex);
            }
        };

        if (hasSourceHierarchy)
        {
            std::unordered_map<std::string, const SourceHierarchyNode*> sourceNodeByKey;
            std::unordered_map<std::string, std::vector<std::string>> sourceChildren;
            std::unordered_map<std::string, std::vector<int>> ownChannelsByKey;
            std::unordered_set<std::string> includedKeys;
            for (const SourceHierarchyNode& node : sourceHierarchy.Nodes)
            {
                sourceNodeByKey.emplace(node.Key, &node);
                sourceChildren[node.ParentKey].push_back(node.Key);
            }

            for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
            {
                const auto keyIt = sourceHierarchy.ChannelParentKeys.find(static_cast<int>(channelIndex));
                if (keyIt == sourceHierarchy.ChannelParentKeys.end())
            continue;
                ownChannelsByKey[keyIt->second].push_back(static_cast<int>(channelIndex));
                std::string currentKey = keyIt->second;
                while (!currentKey.empty() && includedKeys.insert(currentKey).second)
                {
                    const auto nodeIt = sourceNodeByKey.find(currentKey);
                    if (nodeIt == sourceNodeByKey.end())
                        break;
                    currentKey = nodeIt->second->ParentKey;
                }
            }

            std::function<std::vector<int>(const std::string&, TreeId)> addHierarchyBranch =
                [&](const std::string& key, TreeId parentId) -> std::vector<int>
            {
                const auto nodeIt = sourceNodeByKey.find(key);
                if (nodeIt == sourceNodeByKey.end())
                    return {};

                const SourceHierarchyNode& sourceNode = *nodeIt->second;
                const TreeId treeId = addNode(parentId, sourceNode.Label, true);
                m_BoneTreeIds.insert(treeId);
                std::vector<int> descendantChannels = ownChannelsByKey[key];

                const auto childrenIt = sourceChildren.find(key);
                if (childrenIt != sourceChildren.end())
                {
                    for (const std::string& childKey : childrenIt->second)
                    {
                        if (!includedKeys.contains(childKey))
                            continue;
                        std::vector<int> childChannels = addHierarchyBranch(childKey, treeId);
                        descendantChannels.insert(descendantChannels.end(), childChannels.begin(), childChannels.end());
                    }
                }

                std::sort(descendantChannels.begin(), descendantChannels.end());
                descendantChannels.erase(std::unique(descendantChannels.begin(), descendantChannels.end()), descendantChannels.end());
                m_TreeNodeChannels[treeId] = descendantChannels;

                for (int channelIndex : ownChannelsByKey[key])
                    addChannelChildren(treeId, channelIndex);

                return descendantChannels;
            };

            const auto rootChildrenIt = sourceChildren.find("");
            if (rootChildrenIt != sourceChildren.end())
            {
                for (const std::string& rootKey : rootChildrenIt->second)
                {
                    if (!includedKeys.contains(rootKey))
                        continue;
                    (void)addHierarchyBranch(rootKey, 0);
                }
            }
        }

        if (nodes.empty())
        {
            std::unordered_map<uint32, TreeId> targetNodes;
            for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
            {
                const AnimChannel& channel = channels[channelIndex];
                TreeId targetId = 0;
                auto targetIt = targetNodes.find(channel.boneIndex);
                if (targetIt == targetNodes.end())
                {
                    const String targetLabel = !channel.targetName.empty()
                        ? channel.targetName
                        : ("Bone " + std::to_string(channel.boneIndex));
                    targetId = addNode(0, targetLabel, true);
                    targetNodes.emplace(channel.boneIndex, targetId);
                    m_TreeNodeChannels[targetId] = {};
                    m_BoneTreeIds.insert(targetId);
                }
                else
                {
                    targetId = targetIt->second;
                }

                m_TreeNodeChannels[targetId].push_back(static_cast<int>(channelIndex));
                addChannelChildren(targetId, static_cast<int>(channelIndex));
            }
        }

        std::vector<int> objectChannels;
        objectChannels.reserve(channels.size());
        for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
            objectChannels.push_back(static_cast<int>(channelIndex));

        const TreeId objectRootId = nextId++;
        AnimationTreeNode objectRoot{objectRootId, 0, objectLabel, true, {}};
        for (AnimationTreeNode& node : nodes)
        {
            if (node.ParentId == 0)
            {
                node.ParentId = objectRootId;
                objectRoot.Children.push_back(node.Id);
            }
        }
        nodes.push_back(std::move(objectRoot));
        m_TreeNodeChannels[objectRootId] = std::move(objectChannels);
    }

    if (m_PropertyFilter == PropertyFilter::ChannelsOnly && !nodes.empty())
    {
        // Re-parent every node that has channel bindings (Translation/Rotation/Scale + components)
        // directly under the object root, dropping intermediate bone hierarchy nodes.
        TreeId objectRootId = 0;
        for (const AnimationTreeNode& n : nodes)
        {
            if (n.ParentId == 0) { objectRootId = n.Id; break; }
        }
        if (objectRootId != 0)
        {
            std::unordered_set<TreeId> keep{objectRootId};
            for (const auto& kv : m_ChannelBindings)
                keep.insert(kv.first);
            for (const AnimationTreeNode& n : nodes)
            {
                if (n.ParentId == 0) continue;
                if (keep.count(n.Id) && m_ChannelBindings.count(n.Id) == 0)
                    continue;
                if (m_ChannelBindings.count(n.Id) && m_ChannelBindings[n.Id].second == 0u)
                {
                    keep.insert(n.Id);
                }
            }
            std::vector<AnimationTreeNode> rebuilt;
            rebuilt.reserve(keep.size());
            for (AnimationTreeNode& n : nodes)
            {
                if (!keep.count(n.Id)) continue;
                if (n.Id == objectRootId) { n.Children.clear(); rebuilt.push_back(std::move(n)); continue; }
                const auto bind = m_ChannelBindings.find(n.Id);
                // A channel root is a group node (Translation/Rotation/Scale) — it has a channel binding
                // but is NOT a component leaf (X/Y/Z/W). Component leaves sit in m_ComponentLeafIndex.
                const bool isChannelRoot = bind != m_ChannelBindings.end()
                                        && m_ComponentLeafIndex.count(n.Id) == 0;
                if (isChannelRoot)
                {
                    n.ParentId = objectRootId;
                    rebuilt.push_back(std::move(n));
                }
                else if (m_ChannelBindings.count(n.Id))
                {
                    rebuilt.push_back(std::move(n));
                }
            }
            for (AnimationTreeNode& n : rebuilt)
            {
                if (n.Id == objectRootId)
                {
                    for (const AnimationTreeNode& c : rebuilt)
                        if (c.ParentId == objectRootId) n.Children.push_back(c.Id);
                }
            }
            nodes = std::move(rebuilt);
        }
    }

    if (m_PropertyFilter == PropertyFilter::Flat && !nodes.empty())
    {
        // Show every component leaf as a flat row under the object root.
        // Each leaf gets a fully-qualified label: BoneName.ChannelType.Component
        // (the object name is omitted from the label since it stays as the root).
        TreeId objectRootId = 0;
        for (const AnimationTreeNode& n : nodes)
        {
            if (n.ParentId == 0) { objectRootId = n.Id; break; }
        }

        String objectRootLabel;
        for (const AnimationTreeNode& n : nodes)
        {
            if (n.Id == objectRootId) { objectRootLabel = n.Label; break; }
        }

        const std::vector<AnimChannel>& channels = m_CurrentClip ? m_CurrentClip->GetChannels() : std::vector<AnimChannel>{};

        std::vector<AnimationTreeNode> flat;
        AnimationTreeNode flatRoot{objectRootId, 0, objectRootLabel, true, {}};

        // The X/Y/Z/W suffix is appended at render time as a styled child
        // label (.tree-flat-component-suffix) using m_ComponentLeafIndex —
        // the label string here is the prefix only.
        for (const auto& [leafId, componentIndex] : m_ComponentLeafIndex)
        {
            (void)componentIndex;
            const auto bindIt = m_ChannelBindings.find(leafId);
            if (bindIt == m_ChannelBindings.end())
                continue;
            const int channelIndex = bindIt->second.first;
            if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
                continue;

            const AnimChannel& ch = channels[static_cast<size_t>(channelIndex)];
            const String boneName = !ch.targetName.empty() ? ch.targetName : ("Bone" + std::to_string(ch.boneIndex));
            const char* channelType = ch.path == AnimPath::Translation ? "Translation"
                                    : ch.path == AnimPath::Rotation    ? "Rotation"
                                    : ch.path == AnimPath::MorphWeight ? "MorphWeight"
                                                                       : "Scale";
            const String flatLabel = boneName + "." + channelType + ".";

            AnimationTreeNode leaf{leafId, objectRootId, flatLabel, false, {}};
            flatRoot.Children.push_back(leafId);
            m_BoneTreeIds.insert(leafId);
            flat.push_back(std::move(leaf));
        }

        flat.insert(flat.begin(), std::move(flatRoot));
        nodes = std::move(flat);
    }

    if (!m_PropertiesSearchQuery.empty() && !nodes.empty())
    {
        std::string needle;
        needle.reserve(m_PropertiesSearchQuery.size());
        for (char c : m_PropertiesSearchQuery) needle.push_back(static_cast<char>(std::tolower(c)));

        auto labelMatches = [&](const std::string& label) -> bool
        {
            std::string hay;
            hay.reserve(label.size());
            for (char c : label) hay.push_back(static_cast<char>(std::tolower(c)));
            return hay.find(needle) != std::string::npos;
        };

        std::unordered_map<TreeId, size_t> indexById;
        for (size_t i = 0; i < nodes.size(); ++i) indexById[nodes[i].Id] = i;

        std::unordered_set<TreeId> matched;
        for (const AnimationTreeNode& n : nodes)
            if (labelMatches(n.Label)) matched.insert(n.Id);

        // Mark all ancestors of matched nodes as kept; also keep matched-node descendants.
        std::unordered_set<TreeId> keep = matched;
        for (TreeId id : matched)
        {
            TreeId p = id;
            while (true)
            {
                const auto it = indexById.find(p);
                if (it == indexById.end()) break;
                const TreeId parent = nodes[it->second].ParentId;
                if (parent == 0) break;
                keep.insert(parent);
                p = parent;
            }
        }
        std::function<void(TreeId)> markDescendants = [&](TreeId id)
        {
            const auto it = indexById.find(id);
            if (it == indexById.end()) return;
            for (TreeId child : nodes[it->second].Children)
            {
                if (keep.insert(child).second) markDescendants(child);
            }
        };
        for (TreeId id : matched) markDescendants(id);

        std::vector<AnimationTreeNode> filtered;
        filtered.reserve(keep.size());
        for (AnimationTreeNode& n : nodes)
        {
            if (!keep.count(n.Id)) continue;
            std::vector<TreeId> kept;
            for (TreeId c : n.Children) if (keep.count(c)) kept.push_back(c);
            n.Children = std::move(kept);
            filtered.push_back(std::move(n));
        }
        nodes = std::move(filtered);
    }

    m_PropertiesTreeProvider = std::make_unique<AnimationChannelTreeProvider>(std::move(nodes));
    m_PropertiesTree->SetDataProvider(m_PropertiesTreeProvider.get());
    m_SuppressDSRowRebuild = true;
    m_PropertiesTree->ExpandAll();
    m_SuppressDSRowRebuild = false;
    BuildDopeSheetTrackRows();
    if (m_DopeSheetView)
        m_DopeSheetView->SetScrollOffset(m_PropertiesTree->GetScrollOffsetY());
}

void AnimationWindowPanel::EnsurePropertyTreeRowActions(UIElement* row)
{
    if (!row || row->HasClass("tree-row-actions-ready"))
        return;
    row->AddClass("tree-row-actions-ready");

    auto actions = std::make_unique<UIElement>();
    actions->AddClass("tree-row-actions");

    auto colorTag = std::make_unique<UIElement>();
    colorTag->AddClass("tree-row-color-tag");

    auto makeBtn = [](const char* extraClass) -> std::unique_ptr<Button>
    {
        auto b = std::make_unique<Button>();
        b->AddClass("tree-row-action-icon");
        b->AddClass(extraClass);
        return b;
    };

    auto lock = makeBtn("tree-row-action-lock");
    lock->RegisterEventHandler(kEventMouseDown, [this, row](UIEvent& e)
    {
        if (e.Button != 0) return;
        const auto it = m_RowToTreeId.find(row);
        if (it == m_RowToTreeId.end() || it->second == 0) return;
        const TreeId id = it->second;
        const bool nowLocked = m_LockedTreeIds.insert(id).second;
        if (!nowLocked) m_LockedTreeIds.erase(id);
        for (TreeId selId : GetEffectiveSelection(id))
        {
            if (selId == id) continue;
            if (nowLocked) m_LockedTreeIds.insert(selId);
            else           m_LockedTreeIds.erase(selId);
        }
        for (const auto& [rowEl, treeId] : m_RowToTreeId)
            UpdatePropertyTreeRowActionState(treeId, rowEl);
        e.Stop();
    });

    auto prevKey = makeBtn("tree-row-action-prev-key");
    prevKey->RegisterEventHandler(kEventMouseUp, [this, row](UIEvent& e)
    {
        if (e.Button != 0) return;
        const auto it = m_RowToTreeId.find(row);
        if (it == m_RowToTreeId.end() || it->second == 0) return;
        GoToAdjacentKeyOnTree(GetEffectiveSelection(it->second), false);
        e.Stop();
    });

    auto addKey = makeBtn("tree-row-action-add-key");
    addKey->RegisterEventHandler(kEventMouseUp, [this, row](UIEvent& e)
    {
        if (e.Button != 0) return;
        const auto it = m_RowToTreeId.find(row);
        if (it == m_RowToTreeId.end() || it->second == 0) return;
        AddKeysOnTree(it->second, GetEffectiveSelection(it->second));
        e.Stop();
    });

    auto nextKey = makeBtn("tree-row-action-next-key");
    nextKey->RegisterEventHandler(kEventMouseUp, [this, row](UIEvent& e)
    {
        if (e.Button != 0) return;
        const auto it = m_RowToTreeId.find(row);
        if (it == m_RowToTreeId.end() || it->second == 0) return;
        GoToAdjacentKeyOnTree(GetEffectiveSelection(it->second), true);
        e.Stop();
    });

    auto pin = makeBtn("tree-row-action-pin");
    pin->RegisterEventHandler(kEventMouseDown, [this, row](UIEvent& e)
    {
        if (e.Button != 0) return;
        const auto it = m_RowToTreeId.find(row);
        if (it == m_RowToTreeId.end() || it->second == 0) return;
        const TreeId id = it->second;
        const bool nowPinned = m_PinnedTreeIds.insert(id).second;
        if (!nowPinned) m_PinnedTreeIds.erase(id);
        for (TreeId selId : GetEffectiveSelection(id))
        {
            if (selId == id) continue;
            if (nowPinned) m_PinnedTreeIds.insert(selId);
            else           m_PinnedTreeIds.erase(selId);
        }
        for (const auto& [rowEl, treeId] : m_RowToTreeId)
            UpdatePropertyTreeRowActionState(treeId, rowEl);
        RefreshChannelSelection(false);
        e.Stop();
    });

    actions->AddChild(std::move(colorTag));
    actions->AddChild(std::move(prevKey));
    actions->AddChild(std::move(addKey));
    actions->AddChild(std::move(nextKey));
    actions->AddChild(std::move(pin));
    actions->AddChild(std::move(lock));
    row->AddChild(std::move(actions));

    // Swatch is a separate child so it can hug the right edge independently.
    auto swatch = std::make_unique<UIElement>();
    swatch->AddClass("tree-row-component-swatch");
    swatch->RegisterEventHandler(kEventMouseUp, [this, row](UIEvent& e)
    {
        if (e.Button != 0 || !m_OpenColorPickerWindow) return;
        // Only open for component rows (X/Y/Z/W), not for name/group rows.
        const bool isComponent = row->HasClass("tree-component-x") || row->HasClass("tree-component-y")
                              || row->HasClass("tree-component-z") || row->HasClass("tree-component-w");
        if (!isComponent) return;
        // Determine component index from the row's CSS class.
        uint32 component = 0u;
        if (row->HasClass("tree-component-y")) component = 1u;
        else if (row->HasClass("tree-component-z")) component = 2u;
        else if (row->HasClass("tree-component-w")) component = 3u;

        // Resolve which channel this row belongs to (for per-channel override).
        int channel = -1;
        const auto rowIt = m_RowToTreeId.find(row);
        if (rowIt != m_RowToTreeId.end())
        {
            const auto bindIt = m_ChannelBindings.find(rowIt->second);
            if (bindIt != m_ChannelBindings.end())
                channel = bindIt->second.first;
        }

        // Prefer per-channel override, fall back to per-component.
        uint32 currentArgb = 0u;
        if (channel >= 0)
        {
            const uint32 perChannelKey = ((static_cast<uint32>(channel) + 1u) << 8u) | component;
            const auto it = m_CurveColorOverrides.find(perChannelKey);
            if (it != m_CurveColorOverrides.end())
                currentArgb = it->second;
        }
        if (currentArgb == 0u)
        {
            const auto it = m_CurveColorOverrides.find(component);
            if (it != m_CurveColorOverrides.end())
                currentArgb = it->second;
        }
        if (currentArgb == 0u)
        {
            static constexpr uint32 kDefaults[4] = { 0xFFF25959u, 0xFF59D966u, 0xFF598FF2u, 0xFFF2CC59u };
            currentArgb = component < 4u ? kDefaults[component] : 0xFFDDDDDDu;
        }
        const uint32 originalArgb = currentArgb;

        auto applyColor = [this, component, channel](uint32_t argb, bool persist)
        {
            if (channel >= 0)
            {
                const uint32 key = ((static_cast<uint32>(channel) + 1u) << 8u) | component;
                m_CurveColorOverrides[key] = argb;
            }
            else
            {
                m_CurveColorOverrides[component] = argb;
                if (persist)
                {
                    auto& s = AnimationWindowSettings::Get();
                    switch (component)
                    {
                    case 0: s.ColorX = argb; break;
                    case 1: s.ColorY = argb; break;
                    case 2: s.ColorZ = argb; break;
                    case 3: s.ColorW = argb; break;
                    default: break;
                    }
                    s.Save();
                }
            }
            if (m_CurvesGraphView)
                m_CurvesGraphView->SetChannelColors(m_CurveColorOverrides);
            if (m_PropertiesTree)
                m_PropertiesTree->RefreshFromProvider();
        };

        ColorPickerCallbacks cbs;
        cbs.onValueChanging = [applyColor](uint32_t argb, float) { applyColor(argb, false); };
        cbs.onApply         = [applyColor](uint32_t argb, float) { applyColor(argb, true); };
        cbs.onCancel        = [applyColor, originalArgb]()       { applyColor(originalArgb, false); };
        m_OpenColorPickerWindow(currentArgb, 1.0f, std::move(cbs));
        e.Stop();
    });
    row->AddChild(std::move(swatch));
}

void AnimationWindowPanel::UpdatePropertyTreeRowActionState(TreeId id, UIElement* row)
{
    if (!row) return;
    UIElement* actions = nullptr;
    for (const auto& child : row->GetChildren())
    {
        if (child && child->HasClass("tree-row-actions"))
        {
            actions = child.get();
            break;
        }
    }
    if (!actions) return;

    const bool locked = m_LockedTreeIds.count(id) != 0;
    const bool pinned = m_PinnedTreeIds.count(id) != 0;
    if (locked) actions->AddClass("any-active"); else actions->RemoveClass("any-active");

    UIElement* lockEl = nullptr;
    UIElement* tagEl = nullptr;
    UIElement* pinEl = nullptr;
    for (const auto& child : actions->GetChildren())
    {
        if (!child) continue;
        if (child->HasClass("tree-row-action-lock")) lockEl = child.get();
        else if (child->HasClass("tree-row-color-tag")) tagEl = child.get();
        else if (child->HasClass("tree-row-action-pin")) pinEl = child.get();
    }
    if (lockEl)
    {
        if (locked) lockEl->AddClass("active");
        else lockEl->RemoveClass("active");
        lockEl->Overrides().Set(Style::BackgroundTint, 0xFFFFFFFFu);
    }
    if (pinEl)
    {
        if (pinned) pinEl->AddClass("active"); else pinEl->RemoveClass("active");
    }
    if (tagEl)
    {
        tagEl->RemoveClass("tag-yellow");
        tagEl->RemoveClass("tag-red");
        tagEl->RemoveClass("tag-green");
        tagEl->RemoveClass("tag-blue");
        const auto it = m_TreeColorTags.find(id);
        if (it != m_TreeColorTags.end())
        {
            switch (it->second)
            {
                case 1u: tagEl->AddClass("tag-yellow"); break;
                case 2u: tagEl->AddClass("tag-red"); break;
                case 3u: tagEl->AddClass("tag-green"); break;
                case 4u: tagEl->AddClass("tag-blue"); break;
                default: break;
            }
        }
    }

    // Update swatch background color from per-component override.
    UIElement* swatchEl = nullptr;
    for (const auto& child : row->GetChildren())
    {
        if (child && child->HasClass("tree-row-component-swatch"))
        {
            swatchEl = child.get();
            break;
        }
    }
    if (swatchEl)
    {
        uint32 component = 0u;
        if (row->HasClass("tree-component-y")) component = 1u;
        else if (row->HasClass("tree-component-z")) component = 2u;
        else if (row->HasClass("tree-component-w")) component = 3u;

        // Resolve: prefer per-channel override, fall back to per-component, then CSS default.
        uint32 resolvedArgb = 0u;
        const auto rowIt = m_RowToTreeId.find(row);
        if (rowIt != m_RowToTreeId.end())
        {
            const auto bindIt = m_ChannelBindings.find(rowIt->second);
            if (bindIt != m_ChannelBindings.end())
            {
                const uint32 perChannelKey = ((static_cast<uint32>(bindIt->second.first) + 1u) << 8u) | component;
                const auto it = m_CurveColorOverrides.find(perChannelKey);
                if (it != m_CurveColorOverrides.end())
                    resolvedArgb = it->second;
            }
        }
        if (resolvedArgb == 0u)
        {
            const auto it = m_CurveColorOverrides.find(component);
            if (it != m_CurveColorOverrides.end())
                resolvedArgb = it->second;
        }

        if (resolvedArgb != 0u)
        {
            const uint32 argb = (resolvedArgb & 0x00FFFFFFu) | 0xFF000000u;
            swatchEl->Overrides().Set(Style::BackgroundColor, argb);
        }
        else
        {
            swatchEl->Overrides().Reset(Style::BackgroundColor);
        }
    }
}

void AnimationWindowPanel::GoToAdjacentKeyOnTree(const std::vector<TreeId>& ids, bool forward)
{
    if (!m_CurrentClip) return;

    const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
    const float t = m_TimelineState.currentTime;
    const float epsilon = 1.0e-5f;
    float bestPrev = -std::numeric_limits<float>::infinity();
    float bestNext = std::numeric_limits<float>::infinity();
    for (TreeId id : ids)
    {
        const auto it = m_TreeNodeChannels.find(id);
        if (it == m_TreeNodeChannels.end()) continue;
        for (int chIdx : it->second)
        {
            if (chIdx < 0 || static_cast<size_t>(chIdx) >= channels.size()) continue;
            for (const AnimKeyframe& key : channels[static_cast<size_t>(chIdx)].keys)
            {
                if (forward)
                {
                    if (key.time > t + epsilon && key.time < bestNext) bestNext = key.time;
                }
                else
                {
                    if (key.time < t - epsilon && key.time > bestPrev) bestPrev = key.time;
                }
            }
        }
    }

    const float target = forward ? bestNext : bestPrev;
    if (!std::isfinite(target)) return;

    m_TimelineState.currentTime = std::max(m_TimelineState.rangeStart,
                                            std::min(m_TimelineState.rangeEnd, target));
    UpdateCurrentFrameLabel();
    UpdateTimelineRulerAndLabels();
}

void AnimationWindowPanel::AddKeyOnTree(TreeId id)
{
    const auto bind = m_ChannelBindings.find(id);
    if (bind != m_ChannelBindings.end())
    {
        m_SelectedChannel = bind->second.first;
        m_SelectedComponent = bind->second.second;
    }
    else
    {
        const auto it = m_TreeNodeChannels.find(id);
        if (it == m_TreeNodeChannels.end() || it->second.empty()) return;
        m_SelectedChannel = it->second.front();
        m_SelectedComponent = 0u;
    }
    InsertKeyAtCurrentTime();
}

void AnimationWindowPanel::AddKeysOnTree(TreeId clickedId, const std::vector<TreeId>& ids)
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset) return;

    // Set selected channel from the clicked row so the curves view highlights correctly.
    {
        const auto bind = m_ChannelBindings.find(clickedId);
        if (bind != m_ChannelBindings.end())
        {
            m_SelectedChannel = bind->second.first;
            m_SelectedComponent = bind->second.second;
        }
        else
        {
            const auto nodeIt = m_TreeNodeChannels.find(clickedId);
            if (nodeIt != m_TreeNodeChannels.end() && !nodeIt->second.empty())
            {
                m_SelectedChannel = nodeIt->second.front();
                m_SelectedComponent = 0u;
            }
        }
    }

    // Collect unique channel indices from all selected tree IDs.
    std::vector<size_t> channels;
    for (TreeId selId : ids)
    {
        const auto bind = m_ChannelBindings.find(selId);
        if (bind != m_ChannelBindings.end())
        {
            channels.push_back(static_cast<size_t>(bind->second.first));
        }
        else
        {
            const auto nodeIt = m_TreeNodeChannels.find(selId);
            if (nodeIt != m_TreeNodeChannels.end())
                for (int ch : nodeIt->second)
                    channels.push_back(static_cast<size_t>(ch));
        }
    }
    std::sort(channels.begin(), channels.end());
    channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
    if (channels.empty()) return;

    const float time = m_TimelineState.currentTime;
    (void)ExecuteClipEditWithUndo("Insert Animation Key", [this, channels, time]()
    {
        bool anyAdded = false;
        for (size_t ch : channels)
        {
            const bool added = m_CurrentClipAsset->AddKeyframe(ch, time);
            if (added) { RecomputeAutoTangents(ch); anyAdded = true; }
        }
        return anyAdded;
    });
}

std::vector<TreeId> AnimationWindowPanel::GetEffectiveSelection(TreeId clickedId) const
{
    if (!m_PropertiesTreeSelection) return {clickedId};
    const auto& sel = m_PropertiesTreeSelection->GetSelection();
    for (UI::Interaction::ItemId selId : sel)
    {
        if (static_cast<TreeId>(selId) == clickedId)
        {
            std::vector<TreeId> ids;
            ids.reserve(sel.size());
            for (UI::Interaction::ItemId s : sel)
                ids.push_back(static_cast<TreeId>(s));
            return ids;
        }
    }
    return {clickedId};
}

void AnimationWindowPanel::RefreshChannelSelection(bool syncTreeSelection)
{
    if (!m_CurrentClip)
    {
        m_VisibleChannels.clear();
    }

    if (m_CurrentClip)
    {
        const std::vector<AnimChannel>& channels = m_CurrentClip->GetChannels();
        std::vector<int> validVisibleChannels;
        validVisibleChannels.reserve(m_VisibleChannels.size());
        for (int channelIndex : m_VisibleChannels)
        {
            if (channelIndex >= 0 && static_cast<size_t>(channelIndex) < channels.size())
                validVisibleChannels.push_back(channelIndex);
        }
        m_VisibleChannels = std::move(validVisibleChannels);

        if (m_VisibleChannels.empty() && !channels.empty())
            m_VisibleChannels = {0};

        if ((m_SelectedChannel < 0 || std::find(m_VisibleChannels.begin(), m_VisibleChannels.end(), m_SelectedChannel) == m_VisibleChannels.end()) &&
            !m_VisibleChannels.empty())
        {
            m_SelectedChannel = m_VisibleChannels.front();
            if (m_SelectedTreeId == 0)
                m_SelectedComponent = 0u;
        }

        if (m_SelectedChannel >= 0 && static_cast<size_t>(m_SelectedChannel) < channels.size())
        {
            const uint32 componentCount = channels[static_cast<size_t>(m_SelectedChannel)].path == AnimPath::Rotation ? 4u : 3u;
            if (m_SelectedComponent >= componentCount)
                m_SelectedComponent = 0u;
        }
    }

    if (m_DopeSheetView)
    {
        m_DopeSheetView->SetActiveChannel(m_SelectedChannel);
        m_DopeSheetView->SetVisibleChannels(m_VisibleChannels);
        BuildDopeSheetTrackRows();
    }
    if (m_CurvesGraphView)
    {
        // Show all components when a parent channel node is selected (not a component leaf)
        // or when multiple component leaves of the same channel are selected.
        bool showAllComponents = false;
        if (m_VisibleChannels.size() == 1)
        {
            const bool isComponentLeaf = m_ComponentLeafIndex.count(m_SelectedTreeId) > 0;
            const bool isChannelParent = !isComponentLeaf && m_ChannelBindings.count(m_SelectedTreeId) > 0;
            const bool multiItemsSameChannel = m_PropertiesTreeSelection &&
                                               m_PropertiesTreeSelection->GetSelection().size() > 1;
            showAllComponents = isChannelParent || multiItemsSameChannel;
        }
        std::vector<PinnedCurve> pinnedCurves;
        for (TreeId id : m_PinnedTreeIds)
        {
            if (const auto leaf = m_ComponentLeafIndex.find(id); leaf != m_ComponentLeafIndex.end())
            {
                if (const auto binding = m_ChannelBindings.find(id); binding != m_ChannelBindings.end())
                    pinnedCurves.push_back({binding->second.first, leaf->second});
            }
            else if (const auto node = m_TreeNodeChannels.find(id); node != m_TreeNodeChannels.end() && m_CurrentClip)
            {
                const auto& channels = m_CurrentClip->GetChannels();
                for (int channel : node->second)
                    if (channel >= 0 && static_cast<size_t>(channel) < channels.size())
                        for (uint32 component = 0; component < GetComponentCountByPath(channels[channel].path); ++component)
                            pinnedCurves.push_back({channel, component});
            }
        }
        m_CurvesGraphView->SetPinnedCurves(std::move(pinnedCurves));
        m_CurvesGraphView->SetShowAllComponents(showAllComponents);
        m_CurvesGraphView->SetVisibleChannels(m_VisibleChannels);
        m_CurvesGraphView->SetSelectedCurve(m_SelectedChannel, m_SelectedComponent);
    }

    if (m_PropertiesTreeSelection && syncTreeSelection)
    {
        TreeId selectedId = 0;
        if (m_SelectedTreeId != 0 &&
            (m_ChannelBindings.contains(m_SelectedTreeId) || m_TreeNodeChannels.contains(m_SelectedTreeId)))
        {
            selectedId = m_SelectedTreeId;
        }
        else
        {
            for (const auto& [treeId, binding] : m_ChannelBindings)
            {
                if (binding.first == m_SelectedChannel && binding.second == m_SelectedComponent)
                {
                    selectedId = treeId;
                    break;
                }
            }
        }

        if (selectedId != 0)
            m_PropertiesTreeSelection->SetSingle(selectedId);
    }

    UpdateInterpolationButtonState();
    ApplyAutoFitHeightIfEnabled();
}

void AnimationWindowPanel::BeginClipUndoGesture(const char* actionName)
{
    ClearPendingClipUndoGesture();

    if (!m_UndoRedo || !m_CurrentClipAsset)
        return;

    Vector<uint8> snapshot;
    if (!m_CurrentClipAsset->SaveToData(snapshot))
        return;

    m_PendingCurveUndoClip = m_CurrentClipAsset;
    m_PendingCurveUndoBefore.assign(snapshot.begin(), snapshot.end());
    m_PendingCurveUndoName = actionName ? actionName : "Edit Animation Curve";
    // Capture the current retime in/out so undo/redo can restore it together
    // with the keyframe times. This is harmless for non-retime gestures —
    // the snapshot just records the current (possibly inactive) region.
    if (m_CurvesGraphView)
    {
        m_PendingRetimeRegionBefore = m_CurvesGraphView->GetRetimeRegion();
        m_PendingRetimeRegionCaptured = true;
    }
}

void AnimationWindowPanel::CommitClipUndoGesture()
{
    if (!m_UndoRedo || !m_PendingCurveUndoClip || m_PendingCurveUndoBefore.empty())
    {
        ClearPendingClipUndoGesture();
        return;
    }

    Vector<uint8> afterSnapshot;
    if (!m_PendingCurveUndoClip->SaveToData(afterSnapshot))
    {
        ClearPendingClipUndoGesture();
        return;
    }

    const std::vector<std::uint8_t> afterBytes(afterSnapshot.begin(), afterSnapshot.end());
    if (afterBytes != m_PendingCurveUndoBefore)
    {
        const bool regionCaptured = m_PendingRetimeRegionCaptured;
        const CurvesGraphView::RetimeRegionState beforeRegion = m_PendingRetimeRegionBefore;
        CurvesGraphView::RetimeRegionState afterRegion;
        if (regionCaptured && m_CurvesGraphView)
            afterRegion = m_CurvesGraphView->GetRetimeRegion();
        const std::vector<std::uint8_t> beforeBytesCopy = m_PendingCurveUndoBefore;

        auto cmd = std::make_unique<AnimationClipSnapshotCommand>(
            m_PendingCurveUndoName,
            m_PendingCurveUndoClip,
            m_PendingCurveUndoBefore,
            afterBytes,
            [this, regionCaptured, beforeRegion, afterRegion, beforeBytesCopy](AnimationClip* clip)
            {
                if (!clip)
                    return;
                MarkDirty();
                if (m_CurrentClip == clip)
                    RefreshDiscreteClipEditState();

                // Restore the retime in/out alongside the keyframe state.
                // m_OnApplied fires for both Undo() and Redo(); infer which
                // by comparing the just-restored clip bytes to the captured
                // before-snapshot — equal means we're at the gesture's
                // start (Undo) and apply the pre-gesture region.
                if (regionCaptured && m_CurvesGraphView && m_CurrentClipAsset)
                {
                    Vector<uint8> nowBytes;
                    if (m_CurrentClipAsset->SaveToData(nowBytes))
                    {
                        const std::vector<std::uint8_t> now(nowBytes.begin(), nowBytes.end());
                        const bool isBefore = (now == beforeBytesCopy);
                        m_CurvesGraphView->SetRetimeRegion(isBefore ? beforeRegion : afterRegion);
                    }
                }
            });
        m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
    }

    ClearPendingClipUndoGesture();
}

void AnimationWindowPanel::ClearPendingClipUndoGesture()
{
    m_PendingCurveUndoClip.reset();
    m_PendingCurveUndoBefore.clear();
    m_PendingCurveUndoName.clear();
    m_PendingRetimeRegionCaptured = false;
    m_PendingRetimeRegionBefore = {};
}

void AnimationWindowPanel::BeginPanelUndoGesture(const char* actionName)
{
    ClearPendingPanelUndoGesture();
    if (!m_UndoRedo)
        return;

    m_PendingPanelUndoBefore = CapturePanelEditSnapshot();
    m_PendingPanelUndoName = actionName ? actionName : "Edit Animation Panel";
    m_PendingPanelUndoActive = true;
}

void AnimationWindowPanel::CommitPanelUndoGesture()
{
    if (!m_PendingPanelUndoActive)
    {
        ClearPendingPanelUndoGesture();
        return;
    }

    const PanelEditSnapshot afterSnapshot = CapturePanelEditSnapshot();
    if (m_UndoRedo)
    {
        auto cmd = std::make_unique<AnimationPanelSnapshotCommand>(
            m_PendingPanelUndoName.empty() ? "Edit Animation Panel" : m_PendingPanelUndoName,
            this,
            m_PendingPanelUndoBefore,
            afterSnapshot,
            false);
        m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
    }

    ClearPendingPanelUndoGesture();
}

void AnimationWindowPanel::ClearPendingPanelUndoGesture()
{
    m_PendingPanelUndoActive = false;
    m_PendingPanelUndoBefore = {};
    m_PendingPanelUndoName.clear();
}

bool AnimationWindowPanel::ExecuteClipEditWithUndo(const char* actionName, const std::function<bool()>& applyEdit)
{
    if (!applyEdit)
        return false;

    Vector<uint8> beforeSnapshot;
    const bool canUndo = m_UndoRedo && m_CurrentClipAsset && m_CurrentClipAsset->SaveToData(beforeSnapshot);
    if (!applyEdit())
        return false;

    RefreshDiscreteClipEditState();

    if (!canUndo || !m_CurrentClipAsset)
        return true;

    Vector<uint8> afterSnapshot;
    if (!m_CurrentClipAsset->SaveToData(afterSnapshot))
        return true;

    const std::vector<std::uint8_t> beforeBytes(beforeSnapshot.begin(), beforeSnapshot.end());
    const std::vector<std::uint8_t> afterBytes(afterSnapshot.begin(), afterSnapshot.end());
    if (beforeBytes == afterBytes)
        return true;

    auto cmd = std::make_unique<AnimationClipSnapshotCommand>(
        actionName ? actionName : "Edit Animation Clip",
        m_CurrentClipAsset,
        beforeBytes,
        afterBytes,
        [this](AnimationClip* clip)
        {
            if (clip && m_CurrentClip == clip)
                RefreshDiscreteClipEditState();
        });
    m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
    return true;
}

AnimationWindowPanel::SelectionSnapshot AnimationWindowPanel::CaptureSelectionSnapshot() const
{
    SelectionSnapshot s;
    if (m_PropertiesTreeSelection)
    {
        s.TreeIds   = m_PropertiesTreeSelection->GetSelection();
        s.TreeAnchor = m_PropertiesTreeSelection->GetAnchor();
    }
    s.SelectedTreeId     = m_SelectedTreeId;
    s.SelectedChannel    = m_SelectedChannel;
    s.SelectedComponent  = m_SelectedComponent;
    s.VisibleChannels    = m_VisibleChannels;
    if (m_CurvesGraphView)
        s.GraphKeys = m_CurvesGraphView->GetSelectedKeyframes();
    return s;
}

void AnimationWindowPanel::ApplySelectionSnapshot(const SelectionSnapshot& s, bool syncTree)
{
    m_SuppressSelectionUndo = true;

    if (m_PropertiesTreeSelection)
        m_PropertiesTreeSelection->SetSelection(s.TreeIds, s.TreeAnchor);

    m_SelectedTreeId    = s.SelectedTreeId;
    m_SelectedChannel   = s.SelectedChannel;
    m_SelectedComponent = s.SelectedComponent;
    m_VisibleChannels   = s.VisibleChannels;

    if (m_CurvesGraphView)
        m_CurvesGraphView->RestoreSelection(s.GraphKeys, s.SelectedChannel, s.SelectedComponent);

    if (syncTree && m_PropertiesTree)
        m_PropertiesTree->SyncSelectionVisuals();

    RefreshChannelSelection(false);
    UpdateStatsBar();

    // Update snapshot baseline so the next user action compares against this state.
    m_LastSelectionSnapshot = s;
    m_SuppressSelectionUndo = false;
}

void AnimationWindowPanel::PushSelectionUndo(const SelectionSnapshot& before, const SelectionSnapshot& after)
{
    if (!m_UndoRedo) return;

    class AnimationSelectionCommand final : public Editor::IEditorCommand
    {
    public:
        AnimationSelectionCommand(AnimationWindowPanel* panel,
                                  SelectionSnapshot before,
                                  SelectionSnapshot after)
            : m_Panel(panel), m_Before(std::move(before)), m_After(std::move(after)) {}

        const char* GetName() const override { return "Animation Selection"; }
        void Do() override {}

        void Undo() override { if (m_Panel) m_Panel->ApplySelectionSnapshot(m_Before, true); }
        void Redo() override { if (m_Panel) m_Panel->ApplySelectionSnapshot(m_After, true); }

    private:
        AnimationWindowPanel* m_Panel;
        SelectionSnapshot m_Before;
        SelectionSnapshot m_After;
    };

    m_UndoRedo->CommitAlreadyApplied(std::make_unique<AnimationSelectionCommand>(this, before, after));
}

AnimationWindowPanel::PanelEditSnapshot AnimationWindowPanel::CapturePanelEditSnapshot() const
{
    PanelEditSnapshot snapshot{};
    snapshot.MarkerTimes = m_MarkerTimes;
    if (m_CompositeModel)
        snapshot.CompositeModel = *m_CompositeModel;
    if (m_LaneClipModel)
        snapshot.LaneModel = *m_LaneClipModel;
    snapshot.SelectedCompositeTrack = m_SelectedCompositeTrack;
    snapshot.SelectedCompositeClip = m_SelectedCompositeClip;
    snapshot.SelectedTimelineKeyType = m_SelectedTimelineKeyType;
    snapshot.SelectedTimelineKeyTrack = m_SelectedTimelineKeyTrack;
    snapshot.SelectedTimelineKey = m_SelectedTimelineKey;
    snapshot.SelectedLane = m_SelectedLane;
    snapshot.SelectedLaneClip = m_SelectedLaneClip;
    return snapshot;
}

void AnimationWindowPanel::ApplyPanelEditSnapshot(const PanelEditSnapshot& snapshot)
{
    m_MarkerTimes = snapshot.MarkerTimes;
    if (!m_CompositeModel)
        m_CompositeModel = std::make_unique<TimeCompositeModel>();
    if (!m_LaneClipModel)
        m_LaneClipModel = std::make_unique<LaneClipModel>();
    *m_CompositeModel = snapshot.CompositeModel;
    *m_LaneClipModel = snapshot.LaneModel;
    m_SelectedCompositeTrack = snapshot.SelectedCompositeTrack;
    m_SelectedCompositeClip = snapshot.SelectedCompositeClip;
    m_SelectedTimelineKeyType = snapshot.SelectedTimelineKeyType;
    m_SelectedTimelineKeyTrack = snapshot.SelectedTimelineKeyTrack;
    m_SelectedTimelineKey = snapshot.SelectedTimelineKey;
    m_SelectedLane = snapshot.SelectedLane;
    m_SelectedLaneClip = snapshot.SelectedLaneClip;

    if (m_MarkersElement)
        m_MarkersElement->SetMarkerTimes(m_MarkerTimes);
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->SetModel(m_CompositeModel.get());
        if (m_SelectedCompositeTrack < m_CompositeModel->tracks.size() &&
            m_SelectedCompositeClip < m_CompositeModel->tracks[m_SelectedCompositeTrack].clips.size())
            m_TimeCompositeView->SetSelectedClip(m_SelectedCompositeTrack, m_SelectedCompositeClip);
        else
        {
            m_SelectedCompositeTrack = static_cast<size_t>(-1);
            m_SelectedCompositeClip = static_cast<size_t>(-1);
            m_TimeCompositeView->ClearSelection();
        }
        m_TimeCompositeView->MarkDirty(VisualDirty);
    }
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
        if (m_SelectedLane < m_LaneClipModel->lanes.size() &&
            m_SelectedLaneClip < m_LaneClipModel->lanes[m_SelectedLane].clips.size())
            m_LaneClipEditorView->SetSelectedClip(m_SelectedLane, m_SelectedLaneClip);
        else
        {
            m_SelectedLane = static_cast<size_t>(-1);
            m_SelectedLaneClip = static_cast<size_t>(-1);
            m_LaneClipEditorView->ClearSelection();
        }
        m_LaneClipEditorView->MarkDirty(VisualDirty);
    }
    RefreshSequencerSidebar();
    RefreshTimelineInspector();
    if (m_ActiveView == ActiveView::TimeComposite && m_CurrentTimelineAsset)
        m_TimelineDirty = true;
    else if (m_ActiveView == ActiveView::ClipEditor && m_CurrentClipSetAsset)
        m_ClipSetDirty = true;
    else
        m_Dirty = true;

    UpdateTitle();
    if (m_UnsavedIndicator)
        m_UnsavedIndicator->RemoveClass("hidden");
}

bool AnimationWindowPanel::ExecutePanelEditWithUndo(const char* actionName,
                                                    const std::function<bool()>& applyEdit,
                                                    bool mergeable)
{
    if (!applyEdit)
        return false;

    auto markEditedViewDirty = [this]()
    {
        if (m_ActiveView == ActiveView::TimeComposite && m_CurrentTimelineAsset)
            m_TimelineDirty = true;
        else if (m_ActiveView == ActiveView::ClipEditor && m_CurrentClipSetAsset)
            m_ClipSetDirty = true;
        else
            m_Dirty = true;

        UpdateTitle();
        if (m_UnsavedIndicator)
            m_UnsavedIndicator->RemoveClass("hidden");
    };

    const PanelEditSnapshot beforeSnapshot = CapturePanelEditSnapshot();
    if (!applyEdit())
        return false;

    const PanelEditSnapshot afterSnapshot = CapturePanelEditSnapshot();
    if (!m_UndoRedo)
    {
        markEditedViewDirty();
        return true;
    }

    auto cmd = std::make_unique<AnimationPanelSnapshotCommand>(
        actionName ? actionName : "Edit Animation Panel",
        this,
        beforeSnapshot,
        afterSnapshot,
        mergeable);
    m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
    markEditedViewDirty();
    return true;
}

void AnimationWindowPanel::RefreshDiscreteClipEditState()
{
    MarkDirty();
    RefreshPropertyTree();
    UpdatePlaybackRangeFromCurrentClip();
    RefreshSequencingModels();
    RefreshChannelSelection();
    UpdateInterpolationButtonState();
    UpdatePreviewBinding();
    if (m_DopeSheetView)
        m_DopeSheetView->MarkDirty(VisualDirty);
    if (m_CurvesGraphView)
        m_CurvesGraphView->MarkDirty(VisualDirty);
}

void AnimationWindowPanel::RefreshEditedClipState()
{
    MarkDirty();
    RefreshSequencingModels();
    if (m_DopeSheetView)
        m_DopeSheetView->MarkDirty(VisualDirty);
    if (m_CurvesGraphView)
        m_CurvesGraphView->MarkDirty(VisualDirty);
    ApplyAutoFitHeightIfEnabled();
}

void AnimationWindowPanel::ApplyAutoFitHeightIfEnabled()
{
    if (m_AutoFitHeight && m_CurvesGraphView && m_CurrentClip)
        m_CurvesGraphView->FitVisibleCurves();
}

void AnimationWindowPanel::RefreshSequencingModels()
{
    if (!m_CompositeModel)
        m_CompositeModel = std::make_unique<TimeCompositeModel>();
    if (!m_LaneClipModel)
        m_LaneClipModel = std::make_unique<LaneClipModel>();

    // Only rebuild when no file-backed asset owns the model.
    // File-backed timelines and clipsets manage their own models via their open/create paths.
    if (!m_CurrentTimelineAsset)
        m_CompositeModel->tracks.clear();

    if (!m_CurrentClipSetAsset)
        m_LaneClipModel->lanes.clear();

    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->SetModel(m_CompositeModel.get());
        if (m_SelectedCompositeTrack < m_CompositeModel->tracks.size() &&
            m_SelectedCompositeClip < m_CompositeModel->tracks[m_SelectedCompositeTrack].clips.size())
            m_TimeCompositeView->SetSelectedClip(m_SelectedCompositeTrack, m_SelectedCompositeClip);
        else
        {
            m_SelectedCompositeTrack = static_cast<size_t>(-1);
            m_SelectedCompositeClip = static_cast<size_t>(-1);
            m_TimeCompositeView->ClearSelection();
        }
    }
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->SetModel(m_LaneClipModel.get());
        if (m_SelectedLane < m_LaneClipModel->lanes.size() &&
            m_SelectedLaneClip < m_LaneClipModel->lanes[m_SelectedLane].clips.size())
            m_LaneClipEditorView->SetSelectedClip(m_SelectedLane, m_SelectedLaneClip);
        else
        {
            m_SelectedLane = static_cast<size_t>(-1);
            m_SelectedLaneClip = static_cast<size_t>(-1);
            m_LaneClipEditorView->ClearSelection();
        }
    }

    RefreshSequencerSidebar();
    RefreshTimelineInspector();
}

void AnimationWindowPanel::UpdatePlaybackRangeFromCurrentClip()
{
    m_TimelineState.fullStart = 0.0f;
    m_TimelineState.fullEnd = std::max(m_CurrentClip ? m_CurrentClip->GetDuration() : 10.0f, 0.1f);
    m_TimelineState.rangeStart = 0.0f;
    m_TimelineState.rangeEnd = m_TimelineState.fullEnd;
    m_TimelineState.currentTime = std::clamp(m_TimelineState.currentTime, m_TimelineState.rangeStart, m_TimelineState.rangeEnd);
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
}

void AnimationWindowPanel::SetPreviewEnabled(bool enabled)
{
    if (m_PreviewEnabled == enabled)
        return;
    m_PreviewEnabled = enabled;
    if (m_PreviewToggleBtn)
    {
        if (enabled) m_PreviewToggleBtn->AddClass("active");
        else         m_PreviewToggleBtn->RemoveClass("active");
    }
    UpdatePreviewBinding();
    if (m_OnPreviewToggled)
        m_OnPreviewToggled(enabled);
}

void AnimationWindowPanel::UpdatePreviewBinding()
{
    if (m_PreviewEnabled && !m_PreviewModelPath.empty() && m_CurrentClipAsset && m_CurrentClipAsset->IsLoaded())
    {
        ModelThumbnailHandler::SetExternalAnimationPreview(m_PreviewModelPath, m_CurrentClipAsset, m_TimelineState.currentTime);
        return;
    }

    ModelThumbnailHandler::ClearExternalAnimationPreview();
}

void AnimationWindowPanel::BreakTangents()
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<size_t, float>> targets;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
        for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
            if (sk.Channel >= 0) targets.emplace_back(static_cast<size_t>(sk.Channel), sk.KeyTime);
    if (targets.empty() && m_SelectedChannel >= 0 && m_ContextMenuKeyTime >= 0.0f)
        targets.emplace_back(static_cast<size_t>(m_SelectedChannel), m_ContextMenuKeyTime);
    if (targets.empty()) return;

    (void)ExecuteClipEditWithUndo("Break Tangents", [this, targets]() {
        bool changed = false;
        for (const auto& [chIdx, t] : targets)
        {
            const auto& channels = m_CurrentClipAsset->GetChannels();
            if (chIdx >= channels.size()) continue;
            const uint32 compCount = AnimChannelComponentCount(channels[chIdx]);
            for (uint32 c = 0; c < compCount; ++c)
                changed |= m_CurrentClipAsset->SetKeyframeTangentBroken(chIdx, t, c, true);
        }
        return changed;
    });
}

void AnimationWindowPanel::UnifyTangents()
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<size_t, float>> targets;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
        for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
            if (sk.Channel >= 0) targets.emplace_back(static_cast<size_t>(sk.Channel), sk.KeyTime);
    if (targets.empty() && m_SelectedChannel >= 0 && m_ContextMenuKeyTime >= 0.0f)
        targets.emplace_back(static_cast<size_t>(m_SelectedChannel), m_ContextMenuKeyTime);
    if (targets.empty()) return;

    (void)ExecuteClipEditWithUndo("Unify Tangents", [this, targets]() {
        bool changed = false;
        for (const auto& [chIdx, t] : targets)
        {
            const auto& channels = m_CurrentClipAsset->GetChannels();
            if (chIdx >= channels.size()) continue;
            const uint32 compCount = AnimChannelComponentCount(channels[chIdx]);
            for (uint32 c = 0; c < compCount; ++c)
            {
                // Clear broken flag and mirror in-tangent to match out-tangent direction
                m_CurrentClipAsset->SetKeyframeTangentBroken(chIdx, t, c, false);
                const AnimChannel& ch = m_CurrentClipAsset->GetChannels()[chIdx];
                const size_t ki = [&]() -> size_t {
                    for (size_t i = 0; i < ch.keys.size(); ++i)
                        if (std::abs(ch.keys[i].time - t) < 1e-5f) return i;
                    return ch.keys.size();
                }();
                if (ki < ch.keys.size())
                {
                    const float outSlope = ch.keys[ki].outTangent[c];
                    const float outW     = ch.keys[ki].outWeight[c];
                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, true, outSlope, outW);
                    changed = true;
                }
            }
        }
        return changed;
    });
}

void AnimationWindowPanel::ToggleTangentMode()
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<size_t, float>> targets;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
        for (const auto& sk : m_CurvesGraphView->GetSelectedKeyframes())
            if (sk.Channel >= 0) targets.emplace_back(static_cast<size_t>(sk.Channel), sk.KeyTime);
    if (targets.empty() && m_SelectedChannel >= 0 && m_ContextMenuKeyTime >= 0.0f)
        targets.emplace_back(static_cast<size_t>(m_SelectedChannel), m_ContextMenuKeyTime);
    if (targets.empty()) return;

    // Check if any selected key has broken tangents
    bool anyBroken = false;
    for (const auto& [chIdx, t] : targets)
    {
        const auto& channels = m_CurrentClipAsset->GetChannels();
        if (chIdx >= channels.size()) continue;
        const AnimChannel& ch = channels[chIdx];
        const size_t ki = [&]() -> size_t {
            for (size_t i = 0; i < ch.keys.size(); ++i)
                if (std::abs(ch.keys[i].time - t) < 1e-5f) return i;
            return ch.keys.size();
        }();
        if (ki < ch.keys.size() && ch.keys[ki].tangentBroken != 0)
        {
            anyBroken = true;
            break;
        }
    }

    // Toggle: if any are broken, unify them; otherwise break them
    const char* opName = anyBroken ? "Unify Tangents" : "Break Tangents";
    (void)ExecuteClipEditWithUndo(opName, [this, targets, anyBroken]() {
        bool changed = false;
        for (const auto& [chIdx, t] : targets)
        {
            const auto& channels = m_CurrentClipAsset->GetChannels();
            if (chIdx >= channels.size()) continue;
            const AnimChannel& ch = channels[chIdx];
            const uint32 compCount = AnimChannelComponentCount(ch);
            
            const size_t ki = [&]() -> size_t {
                for (size_t i = 0; i < ch.keys.size(); ++i)
                    if (std::abs(ch.keys[i].time - t) < 1e-5f) return i;
                return ch.keys.size();
            }();
            if (ki >= ch.keys.size()) continue;

            for (uint32 c = 0; c < compCount; ++c)
            {
                if (anyBroken)
                {
                    // Unify: clear broken flag and mirror out-tangent to in-tangent
                    m_CurrentClipAsset->SetKeyframeTangentBroken(chIdx, t, c, false);
                    const float inSlope = ch.keys[ki].inTangent[c];
                    const float inW     = ch.keys[ki].inWeight[c];
                    changed |= m_CurrentClipAsset->SetKeyframeTangent(chIdx, t, c, false, inSlope, inW);
                }
                else
                {
                    // Break: set broken flag
                    changed |= m_CurrentClipAsset->SetKeyframeTangentBroken(chIdx, t, c, true);
                }
            }
        }
        return changed;
    });
}

void AnimationWindowPanel::GoToPreviousKey()
{
    if (!m_CurrentClip) return;
    const float t = m_TimelineState.currentTime;
    constexpr float kEps = 1e-5f;
    float best = -std::numeric_limits<float>::infinity();
    bool found = false;
    for (int chIdx : m_VisibleChannels)
    {
        if (chIdx < 0 || static_cast<size_t>(chIdx) >= m_CurrentClip->GetChannels().size()) continue;
        for (const AnimKeyframe& key : m_CurrentClip->GetChannels()[static_cast<size_t>(chIdx)].keys)
        {
            if (key.time < t - kEps && key.time > best)
            {
                best = key.time;
                found = true;
            }
        }
    }
    if (found)
    {
        m_TimelineState.currentTime = best;
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
        UpdatePreviewBinding();
    }
}

void AnimationWindowPanel::GoToNextKey()
{
    if (!m_CurrentClip) return;
    const float t = m_TimelineState.currentTime;
    constexpr float kEps = 1e-5f;
    float best = std::numeric_limits<float>::infinity();
    bool found = false;
    for (int chIdx : m_VisibleChannels)
    {
        if (chIdx < 0 || static_cast<size_t>(chIdx) >= m_CurrentClip->GetChannels().size()) continue;
        for (const AnimKeyframe& key : m_CurrentClip->GetChannels()[static_cast<size_t>(chIdx)].keys)
        {
            if (key.time > t + kEps && key.time < best)
            {
                best = key.time;
                found = true;
            }
        }
    }
    if (found)
    {
        m_TimelineState.currentTime = best;
        UpdateTimelineRulerAndLabels();
        UpdateCurrentFrameLabel();
        UpdatePreviewBinding();
    }
}

void AnimationWindowPanel::CenterViewOnCurrentTime()
{
    const float halfWidth = (m_TimelineState.viewEnd - m_TimelineState.viewStart) * 0.5f;
    const float t = m_TimelineState.currentTime;
    m_TimelineState.viewStart = t - halfWidth;
    m_TimelineState.viewEnd   = t + halfWidth;
    UpdateTimelineRulerAndLabels();
    if (m_DopeSheetView)   m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView) m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
}

void AnimationWindowPanel::FramePlaybackRange()
{
    constexpr float kPad = 0.05f;
    const float start = m_TimelineState.rangeStart;
    const float end   = std::max(start + 0.1f, m_TimelineState.rangeEnd);
    const float pad   = (end - start) * kPad;
    m_TimelineState.viewStart = start - pad;
    m_TimelineState.viewEnd   = end   + pad;
    UpdateTimelineRulerAndLabels();
    if (m_DopeSheetView)   m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    if (m_CurvesGraphView) m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
}

void AnimationWindowPanel::InsertKeyAtCurrentTime()
{
    if (!m_CurrentClipAsset)
    {
        m_NewClip = std::make_shared<AnimationClip>(GUID::Null(), std::filesystem::path());
        m_CurrentClipAsset = m_NewClip;
        SetCurrentClip(m_CurrentClipAsset.get());
    }

    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    size_t channelIndex = 0;
    if (m_SelectedChannel >= 0)
    {
        channelIndex = static_cast<size_t>(m_SelectedChannel);
    }
    else
    {
        if (!m_CurrentClipAsset->GetChannels().empty())
            channelIndex = 0;
        else
            channelIndex = m_CurrentClipAsset->FindOrAddChannel(0u, "Root", AnimPath::Translation);
        m_SelectedChannel = static_cast<int>(channelIndex);
        m_SelectedComponent = 0u;
    }

    (void)ExecuteClipEditWithUndo("Insert Animation Key",
                                  [this, channelIndex]()
                                  {
                                      const bool added = m_CurrentClipAsset->AddKeyframe(channelIndex, m_TimelineState.currentTime);
                                      if (added)
                                          RecomputeAutoTangents(channelIndex);
                                      return added;
                                  });
}

void AnimationWindowPanel::DuplicateSelectedKey()
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<int, float>> selectedKeys;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
    {
        for (const CurvesGraphView::SelectedKey& key : m_CurvesGraphView->GetSelectedKeyframes())
            selectedKeys.emplace_back(key.Channel, key.KeyTime);
    }
    else if (m_DopeSheetView)
    {
        for (const DopeSheetView::SelectedKey& key : m_DopeSheetView->GetSelectedKeyframes())
            selectedKeys.emplace_back(key.Channel, key.KeyTime);
    }

    if (selectedKeys.empty())
        return;

    const float frameOffset = 1.0f / std::max(m_TimelineState.fps, 1.0f);
    (void)ExecuteClipEditWithUndo("Duplicate Animation Key",
                                  [this, selectedKeys, frameOffset]()
                                  {
                                      bool changed = false;
                                      for (const auto& [channel, keyTime] : selectedKeys)
                                      {
                                          if (channel < 0 || keyTime < 0.0f)
                                              continue;
                                          changed |= m_CurrentClipAsset->DuplicateKeyframe(static_cast<size_t>(channel),
                                                                                           keyTime,
                                                                                           keyTime + frameOffset);
                                      }
                                      return changed;
                                  });
}

void AnimationWindowPanel::DeleteSelectedTimelineKeys()
{
    if (!m_CompositeModel || !m_TimeCompositeView)
        return;

    std::vector<TimeCompositeView::TrackKeyRef> keys = m_TimeCompositeView->GetSelectedTrackKeys();
    if (keys.empty() &&
        m_SelectedTimelineKeyTrack != static_cast<size_t>(-1) &&
        m_SelectedTimelineKey != static_cast<size_t>(-1))
    {
        keys.push_back({m_SelectedTimelineKeyTrack, m_SelectedTimelineKeyType, m_SelectedTimelineKey});
    }
    if (keys.empty())
        return;

    auto keyStillExists = [this](const TimeCompositeView::TrackKeyRef& ref) -> bool
    {
        if (ref.trackIdx >= m_CompositeModel->tracks.size())
            return false;
        const CompositeTrack& track = m_CompositeModel->tracks[ref.trackIdx];
        switch (ref.type)
        {
        case CompositeTrackType::Property:
            return ref.keyIdx < track.valueKeys.size();
        case CompositeTrackType::Method:
            return ref.keyIdx < track.methodKeys.size();
        case CompositeTrackType::Audio:
            return ref.keyIdx < track.audioKeys.size();
        case CompositeTrackType::Animation:
            return ref.keyIdx < track.animationKeys.size();
        default:
            return false;
        }
    };

    std::vector<TimeCompositeView::TrackKeyRef> valid;
    valid.reserve(keys.size());
    for (const TimeCompositeView::TrackKeyRef& ref : keys)
    {
        if (keyStillExists(ref))
            valid.push_back(ref);
    }
    if (valid.empty())
        return;

    std::sort(valid.begin(), valid.end(),
              [](const TimeCompositeView::TrackKeyRef& left, const TimeCompositeView::TrackKeyRef& right)
              {
                  if (left.trackIdx != right.trackIdx)
                      return left.trackIdx < right.trackIdx;
                  if (left.type != right.type)
                      return static_cast<uint8_t>(left.type) < static_cast<uint8_t>(right.type);
                  return left.keyIdx > right.keyIdx;
              });

    (void)ExecutePanelEditWithUndo("Remove Track Key",
                                   [this, valid]()
                                   {
                                       bool changed = false;
                                       for (const TimeCompositeView::TrackKeyRef& ref : valid)
                                       {
                                           if (ref.trackIdx >= m_CompositeModel->tracks.size())
                                               continue;
                                           CompositeTrack& track = m_CompositeModel->tracks[ref.trackIdx];
                                           const auto eraseAt = [&](auto& keyList)
                                           {
                                               if (ref.keyIdx >= keyList.size())
                                                   return;
                                               keyList.erase(keyList.begin() +
                                                             static_cast<std::ptrdiff_t>(ref.keyIdx));
                                               changed = true;
                                           };
                                           switch (ref.type)
                                           {
                                           case CompositeTrackType::Property:
                                               eraseAt(track.valueKeys);
                                               break;
                                           case CompositeTrackType::Method:
                                               eraseAt(track.methodKeys);
                                               break;
                                           case CompositeTrackType::Audio:
                                               eraseAt(track.audioKeys);
                                               break;
                                           case CompositeTrackType::Animation:
                                               eraseAt(track.animationKeys);
                                               break;
                                           default:
                                               break;
                                           }
                                       }
                                       m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
                                       m_SelectedTimelineKey = static_cast<size_t>(-1);
                                       if (m_TimeCompositeView)
                                       {
                                           m_TimeCompositeView->ClearMarkerInteractionState();
                                           m_TimeCompositeView->MarkDirty(VisualDirty);
                                       }
                                       RefreshSequencerSidebar();
                                       RefreshTimelineInspector();
                                       return changed;
                                   },
                                   false);
}

void AnimationWindowPanel::DeleteSelectedKey()
{
    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    std::vector<std::pair<int, float>> selectedKeys;
    if (m_ActiveView == ActiveView::Curves && m_CurvesGraphView)
    {
        for (const CurvesGraphView::SelectedKey& key : m_CurvesGraphView->GetSelectedKeyframes())
            selectedKeys.emplace_back(key.Channel, key.KeyTime);
    }
    else if (m_DopeSheetView)
    {
        for (const DopeSheetView::SelectedKey& key : m_DopeSheetView->GetSelectedKeyframes())
            selectedKeys.emplace_back(key.Channel, key.KeyTime);
    }

    if (selectedKeys.empty())
        return;

    std::sort(selectedKeys.begin(), selectedKeys.end(),
              [](const auto& left, const auto& right)
              {
                  if (left.first != right.first)
                      return left.first < right.first;
                  return left.second > right.second;
              });
    const bool removed = ExecuteClipEditWithUndo("Delete Animation Key",
                                                 [this, selectedKeys]()
                                                 {
                                                     bool changed = false;
                                                     for (const auto& [channel, keyTime] : selectedKeys)
                                                     {
                                                         if (channel < 0 || keyTime < 0.0f)
                                                             continue;
                                                         changed |= m_CurrentClipAsset->RemoveKeyframe(static_cast<size_t>(channel), keyTime);
                                                     }
                                                     return changed;
                                                 });
    if (removed && m_DopeSheetView)
        m_DopeSheetView->ClearSelection();
    if (removed && m_CurvesGraphView && m_ActiveView == ActiveView::Curves)
        m_CurvesGraphView->ClearSelection();
}

void AnimationWindowPanel::DuplicateSelectedStrip()
{
    if (m_ActiveView == ActiveView::TimeComposite)
    {
        if (!m_CompositeModel || m_SelectedCompositeTrack >= m_CompositeModel->tracks.size())
            return;
        if (m_SelectedCompositeClip >= m_CompositeModel->tracks[m_SelectedCompositeTrack].clips.size())
            return;
        (void)ExecutePanelEditWithUndo("Duplicate Composite Clip",
                                       [this]()
                                       {
                                           auto& track = m_CompositeModel->tracks[m_SelectedCompositeTrack];
                                           CompositeClip duplicate = track.clips[m_SelectedCompositeClip];
                                           duplicate.name += " Copy";
                                           duplicate.offsetOnTimeline += std::max(0.1f, duplicate.outTime - duplicate.inTime);
                                           track.clips.insert(track.clips.begin() + static_cast<std::ptrdiff_t>(m_SelectedCompositeClip + 1), duplicate);
                                           ++m_SelectedCompositeClip;
                                           if (m_TimeCompositeView)
                                           {
                                               m_TimeCompositeView->SetSelectedClip(m_SelectedCompositeTrack, m_SelectedCompositeClip);
                                               m_TimeCompositeView->MarkDirty(VisualDirty);
                                           }
                                           return true;
                                       },
                                       false);
        return;
    }

    if (m_ActiveView == ActiveView::ClipEditor)
    {
        if (!m_LaneClipModel || m_SelectedLane >= m_LaneClipModel->lanes.size())
            return;
        if (m_SelectedLaneClip >= m_LaneClipModel->lanes[m_SelectedLane].clips.size())
            return;
        (void)ExecutePanelEditWithUndo("Duplicate Lane Clip",
                                       [this]()
                                       {
                                           auto& lane = m_LaneClipModel->lanes[m_SelectedLane];
                                           LaneClipInstance duplicate = lane.clips[m_SelectedLaneClip];
                                           duplicate.name += " Copy";
                                           duplicate.startTimeOnLane += std::max(0.1f, duplicate.sourceDuration * duplicate.scale);
                                           lane.clips.insert(lane.clips.begin() + static_cast<std::ptrdiff_t>(m_SelectedLaneClip + 1), duplicate);
                                           ++m_SelectedLaneClip;
                                           if (m_LaneClipEditorView)
                                           {
                                               m_LaneClipEditorView->SetSelectedClip(m_SelectedLane, m_SelectedLaneClip);
                                               m_LaneClipEditorView->MarkDirty(VisualDirty);
                                           }
                                           return true;
                                       },
                                       false);
    }
}

void AnimationWindowPanel::DeleteSelectedStrip()
{
    if (m_ActiveView == ActiveView::TimeComposite)
    {
        if (!m_CompositeModel || m_SelectedCompositeTrack >= m_CompositeModel->tracks.size())
            return;
        if (m_SelectedCompositeClip >= m_CompositeModel->tracks[m_SelectedCompositeTrack].clips.size())
            return;
        (void)ExecutePanelEditWithUndo("Delete Composite Clip",
                                       [this]()
                                       {
                                           auto& track = m_CompositeModel->tracks[m_SelectedCompositeTrack];
                                           track.clips.erase(track.clips.begin() + static_cast<std::ptrdiff_t>(m_SelectedCompositeClip));
                                           if (track.clips.empty())
                                               m_SelectedCompositeClip = static_cast<size_t>(-1);
                                           else
                                               m_SelectedCompositeClip = std::min(m_SelectedCompositeClip, track.clips.size() - 1);
                                           if (m_TimeCompositeView)
                                           {
                                               if (m_SelectedCompositeClip == static_cast<size_t>(-1))
                                                   m_TimeCompositeView->ClearSelection();
                                               else
                                                   m_TimeCompositeView->SetSelectedClip(m_SelectedCompositeTrack, m_SelectedCompositeClip);
                                               m_TimeCompositeView->MarkDirty(VisualDirty);
                                           }
                                           return true;
                                       },
                                       false);
        return;
    }

    if (m_ActiveView == ActiveView::ClipEditor)
    {
        if (!m_LaneClipModel || m_SelectedLane >= m_LaneClipModel->lanes.size())
            return;
        if (m_SelectedLaneClip >= m_LaneClipModel->lanes[m_SelectedLane].clips.size())
            return;
        (void)ExecutePanelEditWithUndo("Delete Lane Clip",
                                       [this]()
                                       {
                                           auto& lane = m_LaneClipModel->lanes[m_SelectedLane];
                                           lane.clips.erase(lane.clips.begin() + static_cast<std::ptrdiff_t>(m_SelectedLaneClip));
                                           if (lane.clips.empty())
                                               m_SelectedLaneClip = static_cast<size_t>(-1);
                                           else
                                               m_SelectedLaneClip = std::min(m_SelectedLaneClip, lane.clips.size() - 1);
                                           if (m_LaneClipEditorView)
                                           {
                                               if (m_SelectedLaneClip == static_cast<size_t>(-1))
                                                   m_LaneClipEditorView->ClearSelection();
                                               else
                                                   m_LaneClipEditorView->SetSelectedClip(m_SelectedLane, m_SelectedLaneClip);
                                               m_LaneClipEditorView->MarkDirty(VisualDirty);
                                           }
                                           return true;
                                       },
                                       false);
    }
}

std::filesystem::path AnimationWindowPanel::GetSuggestedEditablePath() const
{
    if (!m_CurrentClipPath.empty())
    {
        if (NormalizeExtension(m_CurrentClipPath.extension().string()) == ".anim")
            return m_CurrentClipPath;

        const uint32 animationIndex = m_CurrentClipAsset ? m_CurrentClipAsset->GetSelectedAnimationIndex() : 0u;
        return m_CurrentClipPath.parent_path() /
               (m_CurrentClipPath.stem().string() + "_anim_" + std::to_string(animationIndex) + ".anim");
    }

    return std::filesystem::path("Untitled.anim");
}

void AnimationWindowPanel::SetCurrentClip(const AnimationClip* clip)
{
    ClearPendingClipUndoGesture();
    m_CurrentClip = clip;
    if (!m_CurrentClip)
    {
        m_SelectedTreeId = 0;
        m_VisibleChannels.clear();
    }
    if (m_DopeSheetView)
        m_DopeSheetView->SetClip(clip);
    if (m_CurvesGraphView)
        m_CurvesGraphView->SetClip(clip);
    RefreshPropertyTree();
    RefreshChannelSelection();
    RefreshSequencingModels();
    RefreshClipViewEmptyState();
    UpdateTitle();
    UpdatePreviewBinding();
    m_NeedsInitialFrame = true;
    FrameAll();
}

void AnimationWindowPanel::TickTimeline(float deltaTimeSeconds)
{
    if (!m_TimelineState.playing || m_TimelineState.paused)
        return;
    const float dt = static_cast<float>(AdvanceTimeline(m_TimelineState, deltaTimeSeconds));
    if (!m_TimelineState.playing)
    {
        UpdatePlayButtonState();
        UpdatePlayBackwardButtonState();
    }
    // Scroll with playhead (S toggles): shift visible range by same amount as playhead so timeline moves along
    if (m_ScrollWithPlayhead)
    {
        const float duration = m_TimelineState.viewEnd - m_TimelineState.viewStart;
        m_TimelineState.viewStart += dt;
        m_TimelineState.viewEnd += dt;
        if (m_TimelineState.viewStart < 0.0f)
        {
            m_TimelineState.viewStart = 0.0f;
            m_TimelineState.viewEnd = m_TimelineState.viewStart + duration;
        }
        if (m_DopeSheetView)
            m_DopeSheetView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_CurvesGraphView)
            m_CurvesGraphView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_TimeCompositeView)
            m_TimeCompositeView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
        if (m_LaneClipEditorView)
            m_LaneClipEditorView->SetTimeRange(m_TimelineState.viewStart, m_TimelineState.viewEnd);
    }
    UpdateTimelineRulerAndLabels();
    UpdateCurrentFrameLabel();
}

void AnimationWindowPanel::UpdateTimelineRulerAndLabels()
{
    m_TimelineState.fullStart = 0.0f;
    m_TimelineState.fullEnd = std::max(m_TimelineState.fullStart + 0.1f, ComputeTimelineFullRangeEnd());
    m_TimelineState.viewStart = std::clamp(m_TimelineState.viewStart, m_TimelineState.fullStart, m_TimelineState.fullEnd);
    m_TimelineState.viewEnd = std::clamp(m_TimelineState.viewEnd,
                                         std::max(m_TimelineState.viewStart + 0.05f, m_TimelineState.fullStart),
                                         m_TimelineState.fullEnd);

    const float t = m_TimelineState.currentTime;
    const float rs = m_TimelineState.rangeStart;
    const float re = m_TimelineState.rangeEnd;
    const float vs = m_TimelineState.viewStart;
    const float ve = m_TimelineState.viewEnd;
    const float fps = m_TimelineState.fps;
    if (m_TimelineBarElement)
        m_TimelineBarElement->SetTimelineState(t, vs, ve, fps);
    if (m_TimeRangeSlider)
    {
        m_TimeRangeSlider->SetFullRange(m_TimelineState.fullStart, m_TimelineState.fullEnd);
        m_TimeRangeSlider->SetVisibleRange(rs, re);
        m_TimeRangeSlider->SetCurrentTime(t);
        m_TimeRangeSlider->SetFps(fps);
    }
    UIManager* uiMgr = GetOwnerManager();
    auto isFieldFocused = [&](const UIElement* el) -> bool {
        return uiMgr && el && uiMgr->GetFocusedElementId() == el->GetId();
    };
    if (m_RangeStartField && !isFieldFocused(m_RangeStartField))
        m_RangeStartField->SetValue(static_cast<int>(std::round(rs * fps)));
    if (m_RangeEndField && !isFieldFocused(m_RangeEndField))
        m_RangeEndField->SetValue(static_cast<int>(std::round(re * fps)));
    if (!m_MarkersElement)
    {
        UIElement* markersEl = FindById("AnimationWindowTimelineMarkers");
        m_MarkersElement = markersEl ? dynamic_cast<TimelineMarkersElement*>(markersEl) : nullptr;
        if (m_MarkersElement)
            WireMarkersElementCallbacks();
    }
    if (m_MarkersElement)
    {
        m_MarkersElement->SetTimeRange(vs, ve);
        m_MarkersElement->SetCurrentTime(t);
        // Markers draw from current range (same as time labels) so they stay in sync on pan/drag.
        // Do not call SetMarkerTimes here during pan/zoom/tick — marker list is set when added/moved/removed.
    }
    if (m_TimeLabelsElement)
        m_TimeLabelsElement->SetTimelineState(t, vs, ve, fps);
    if (m_FrameLabelsElement)
        m_FrameLabelsElement->SetTimelineState(t, vs, ve, fps);
    if (m_DopeSheetView)
    {
        m_DopeSheetView->SetTimeRange(vs, ve);
        m_DopeSheetView->SetCurrentTime(t);
    }
    if (m_CurvesGraphView)
    {
        m_CurvesGraphView->SetTimeRange(vs, ve);
        m_CurvesGraphView->SetCurrentTime(t);
    }
    if (m_TimeCompositeView)
    {
        m_TimeCompositeView->SetTimeRange(vs, ve);
        m_TimeCompositeView->SetCurrentTime(t);
    }
    if (m_LaneClipEditorView)
    {
        m_LaneClipEditorView->SetTimeRange(vs, ve);
        m_LaneClipEditorView->SetCurrentTime(t);
    }
    SyncTimelineZoomSlider();
    UpdatePreviewBinding();
}

// ----------------------------------------------------------------------------
// Curve-edit options bar (Add Noise / Simplify / Smooth Lowpass / Smooth Peak)
// ----------------------------------------------------------------------------

namespace
{
    /// Wires Godot/Blender-style drag-to-scrub on `dragTarget`. Draggable
    /// labels pass themselves as the target; numeric fields can pass the
    /// field itself so dragging anywhere in the input box scrubs the value
    /// — clicks shorter than the threshold still fall through to focus the
    /// underlying TextInput for typing.
    ///
    /// The `onChanged` callback fires for every scrub step (live, not on
    /// release). TextFieldBase::SetValue suppresses its own value-changed
    /// callback, so we cannot rely on it to drive live previews — the
    /// caller must pass the same handler it would normally hook to
    /// SetOnValueChanged for typed edits.
    template <typename FieldT, typename T>
    void WireFieldDragScrub(UIElement* dragTarget,
                            FieldT* field,
                            float sensitivity,
                            T minVal,
                            T maxVal,
                            std::function<void(const T&)> onChanged)
    {
        if (!dragTarget || !field) return;
        struct DragState
        {
            bool armed = false;     // mouse-down received, watching for threshold
            bool active = false;    // crossed threshold; we are scrubbing
            float startX = 0.0f;
            float lastX = 0.0f;
            float virtualValue = 0.0f; // float-tracked even for IntField, so sub-pixel motion accumulates
        };
        auto state = std::make_shared<DragState>();
        constexpr float kDragThreshold = 4.0f;

        dragTarget->RegisterEventHandler(kEventMouseDown,
            [field, state](UIEvent& e)
        {
            if (e.Button != 0) return;
            state->armed = true;
            state->active = false;
            state->startX = e.X;
            state->lastX = e.X;
            state->virtualValue = static_cast<float>(field->GetValue());
            // Don't capture or stop yet — short clicks must reach the inner
            // TextInput so the user can focus and type.
        });

        dragTarget->RegisterEventHandler(kEventMouseMove,
            [field, dragTarget, state, sensitivity, minVal, maxVal, onChanged](UIEvent& e)
        {
            if (!state->armed) return;
            if (!state->active)
            {
                if (std::abs(e.X - state->startX) < kDragThreshold) return;
                state->active = true;
                e.Capture(dragTarget);
            }
            const float dx = e.X - state->lastX;
            state->lastX = e.X;
            state->virtualValue += dx * sensitivity;
            const float clamped = std::clamp(state->virtualValue,
                                              static_cast<float>(minVal),
                                              static_cast<float>(maxVal));
            T newVal = static_cast<T>(clamped);
            if (newVal != field->GetValue())
            {
                field->SetValue(newVal);
                if (onChanged) onChanged(newVal);
            }
            e.Stop();
        });

        dragTarget->RegisterEventHandler(kEventMouseUp,
            [state](UIEvent& e)
        {
            if (!state->armed) return;
            const bool wasActive = state->active;
            state->armed = false;
            state->active = false;
            // Eat the up-event only if we actually scrubbed; otherwise let it
            // fall through so the bare click can give focus to the TextInput.
            if (wasActive) e.Stop();
        });
    }
}

void AnimationWindowPanel::WireCurveOptions()
{
    // One change-handler per field, used by both typed edits (SetOnValueChanged)
    // and the drag-to-scrub helpers below. TextFieldBase::SetValue suppresses
    // its own value-changed callback, so the drag path must invoke this
    // explicitly to drive live previews.
    auto onNoiseFreqMin = [this](const int& v)
    { m_NoiseFreqMin = std::max(0, v); RefreshOptionsPreview(); };
    auto onNoiseFreqMax = [this](const int& v)
    { m_NoiseFreqMax = std::max(0, v); RefreshOptionsPreview(); };
    auto onNoiseMagnitude = [this](const float& v)
    { m_NoiseMagnitude = std::max(0.0f, v); RefreshOptionsPreview(); };
    auto onSimplifyTimeTol = [this](const float& v)
    { m_SimplifyTimeTol = std::max(0.0f, v); RefreshOptionsPreview(); };
    auto onSimplifyValueTol = [this](const float& v)
    { m_SimplifyValueTol = std::max(0.0f, v); RefreshOptionsPreview(); };
    auto onSmoothFilterWidth = [this](const int& v)
    { m_SmoothFilterWidth = std::max(0, v); RefreshOptionsPreview(); };
    auto onSmoothSampleCount = [this](const int& v)
    { m_SmoothSampleCount = std::max(0, v); RefreshOptionsPreview(); };

    if (m_NoiseFreqMinField)
    {
        m_NoiseFreqMinField->SetValue(m_NoiseFreqMin);
        m_NoiseFreqMinField->SetOnValueChanged(onNoiseFreqMin);
    }
    if (m_NoiseFreqMaxField)
    {
        m_NoiseFreqMaxField->SetValue(m_NoiseFreqMax);
        m_NoiseFreqMaxField->SetOnValueChanged(onNoiseFreqMax);
    }
    if (m_NoiseMagnitudeField)
    {
        m_NoiseMagnitudeField->SetValue(m_NoiseMagnitude);
        m_NoiseMagnitudeField->SetOnValueChanged(onNoiseMagnitude);
    }
    if (m_SimplifyMethodDropdown)
        m_SimplifyMethodDropdown->SetOptionsFromLabels({"Classic"}, 0);
    if (m_SimplifyTimeTolField)
    {
        m_SimplifyTimeTolField->SetValue(m_SimplifyTimeTol);
        m_SimplifyTimeTolField->SetOnValueChanged(onSimplifyTimeTol);
    }
    if (m_SimplifyValueTolField)
    {
        m_SimplifyValueTolField->SetValue(m_SimplifyValueTol);
        m_SimplifyValueTolField->SetOnValueChanged(onSimplifyValueTol);
    }
    if (m_SmoothFilterWidthField)
    {
        m_SmoothFilterWidthField->SetValue(m_SmoothFilterWidth);
        m_SmoothFilterWidthField->SetOnValueChanged(onSmoothFilterWidth);
    }
    if (m_SmoothSampleCountField)
    {
        m_SmoothSampleCountField->SetValue(m_SmoothSampleCount);
        m_SmoothSampleCountField->SetOnValueChanged(onSmoothSampleCount);
    }

    if (m_NoiseConfirmBtn)    m_NoiseConfirmBtn   ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ExitCurveOptionsMode(true); });
    if (m_SimplifyConfirmBtn) m_SimplifyConfirmBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ExitCurveOptionsMode(true); });
    if (m_SmoothConfirmBtn)   m_SmoothConfirmBtn  ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ExitCurveOptionsMode(true); });

    // Drag-to-scrub on both the dedicated drag labels and the field input
    // boxes themselves (Godot/Blender style). Sensitivity is in value-units
    // per drag-pixel.
    constexpr float kIntFreqSens   = 0.10f;
    constexpr float kIntPassSens   = 0.05f;
    constexpr float kFloatMagSens  = 0.002f;
    constexpr float kFloatTolSens  = 0.002f;
    constexpr int   kIntMin = 0;
    constexpr int   kIntMax = 1000;
    constexpr float kFloatMin = 0.0f;
    constexpr float kFloatMax = 1.0e6f;

    auto wireIntPair = [&](Label* label, IntField* field, float sens, std::function<void(const int&)> cb)
    {
        WireFieldDragScrub<IntField, int>(label, field, sens, kIntMin, kIntMax, cb);
        WireFieldDragScrub<IntField, int>(field, field, sens, kIntMin, kIntMax, cb);
    };
    auto wireFloatPair = [&](Label* label, FloatField* field, float sens, std::function<void(const float&)> cb)
    {
        WireFieldDragScrub<FloatField, float>(label, field, sens, kFloatMin, kFloatMax, cb);
        WireFieldDragScrub<FloatField, float>(field, field, sens, kFloatMin, kFloatMax, cb);
    };

    wireIntPair  (m_NoiseFreqMinLabel,         m_NoiseFreqMinField,      kIntFreqSens,  onNoiseFreqMin);
    wireIntPair  (m_NoiseFreqMaxLabel,         m_NoiseFreqMaxField,      kIntFreqSens,  onNoiseFreqMax);
    wireFloatPair(m_NoiseMagnitudeLabel,       m_NoiseMagnitudeField,    kFloatMagSens, onNoiseMagnitude);
    wireFloatPair(m_SimplifyTimeTolLabel,      m_SimplifyTimeTolField,   kFloatTolSens, onSimplifyTimeTol);
    wireFloatPair(m_SimplifyValueTolLabel,     m_SimplifyValueTolField,  kFloatTolSens, onSimplifyValueTol);
    wireIntPair  (m_SmoothFilterWidthLabel,    m_SmoothFilterWidthField, kIntPassSens,  onSmoothFilterWidth);
    wireIntPair  (m_SmoothSampleCountLabel,    m_SmoothSampleCountField, kIntPassSens,  onSmoothSampleCount);

    // Lattice options panel — config-only (no checkmark; values apply live
    // because the lattice tool is itself a continuous toggle, not a one-shot
    // operation with a baseline-snapshot/commit workflow like the others).
    auto onLatticePointCount = [this](const int& v)
    {
        if (m_CurvesGraphView)
            m_CurvesGraphView->SetLatticePointCount(v);
    };
    if (m_LatticePointCountField)
    {
        m_LatticePointCountField->SetValue(m_CurvesGraphView ? m_CurvesGraphView->GetLatticePointCount() : 5);
        m_LatticePointCountField->SetOnValueChanged(onLatticePointCount);
    }
    wireIntPair(m_LatticePointCountLabel, m_LatticePointCountField, kIntPassSens, onLatticePointCount);
    if (m_LatticeBasisDropdown)
    {
        m_LatticeBasisDropdown->SetOptionsFromLabels(
            {"Bezier", "Catmull-Rom"},
            m_CurvesGraphView && m_CurvesGraphView->GetLatticeBasis() == CurvesGraphView::LatticeBasis::CatmullRom ? 1 : 0);
        m_LatticeBasisDropdown->SetOnValueChanged([this](const std::string&)
        {
            if (!m_CurvesGraphView || !m_LatticeBasisDropdown) return;
            const int idx = m_LatticeBasisDropdown->GetSelectedIndex();
            m_CurvesGraphView->SetLatticeBasis(idx == 1
                ? CurvesGraphView::LatticeBasis::CatmullRom
                : CurvesGraphView::LatticeBasis::Bezier);
        });
    }
}

void AnimationWindowPanel::UpdateLatticeOptionsPanel()
{
    if (!m_CurvesGraphView || !m_OptionsBar) return;
    const bool latticeOn = m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice;

    auto setActive = [](UIElement* panel, bool on)
    {
        if (!panel) return;
        if (on) panel->AddClass("anim-options-active");
        else    panel->RemoveClass("anim-options-active");
    };

    if (latticeOn)
    {
        // Sync the field values with the view's current state, then show
        // the bar + the lattice-only panel (the other panels are gated on
        // m_CurveOptionsMode and stay inactive while Lattice is on).
        if (m_LatticePointCountField)
            m_LatticePointCountField->SetValue(m_CurvesGraphView->GetLatticePointCount());
        if (m_LatticeBasisDropdown)
            m_LatticeBasisDropdown->SetSelectedIndex(
                m_CurvesGraphView->GetLatticeBasis() == CurvesGraphView::LatticeBasis::CatmullRom ? 1 : 0);
        setActive(m_OptionsLatticePanel, true);
        m_OptionsBar->RemoveClass("hidden");
    }
    else
    {
        setActive(m_OptionsLatticePanel, false);
        // Only hide the bar if no curve-options mode is also showing.
        if (m_CurveOptionsMode == CurveOptionsMode::None)
            m_OptionsBar->AddClass("hidden");
    }
}

void AnimationWindowPanel::EnterCurveOptionsMode(CurveOptionsMode mode)
{
    if (mode == CurveOptionsMode::None) return;

    // Click the same icon again → cancel (toggle off).
    if (m_CurveOptionsMode == mode)
    {
        ExitCurveOptionsMode(false);
        return;
    }

    // Switching directly between modes: discard the prior preview first.
    if (m_CurveOptionsMode != CurveOptionsMode::None)
        ExitCurveOptionsMode(false);

    // Lattice tool and the curve-options modes share the options bar and
    // both operate on the curve view, so they're mutually exclusive — turn
    // the lattice off before opening any of these one-shot modes.
    if (m_CurvesGraphView &&
        m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice)
    {
        m_CurvesGraphView->SetActiveTool(CurvesGraphView::CurveTool::Select);
        if (auto* lb = dynamic_cast<Button*>(FindById("AnimationWindowLattice"))) lb->RemoveClass("active");
        UpdateLatticeOptionsPanel();
    }

    if (!EnsureEditableClip() || !m_CurrentClipAsset)
        return;

    Vector<uint8> snapshot;
    if (!m_CurrentClipAsset->SaveToData(snapshot))
        return;
    m_CurveOptionsBaseline.assign(snapshot.begin(), snapshot.end());
    m_CurveOptionsMode = mode;

    auto setActive = [](UIElement* panel, bool on)
    {
        if (!panel) return;
        if (on) panel->AddClass("anim-options-active");
        else    panel->RemoveClass("anim-options-active");
    };
    setActive(m_OptionsNoisePanel,    mode == CurveOptionsMode::Noise);
    setActive(m_OptionsSimplifyPanel, mode == CurveOptionsMode::Simplify);
    setActive(m_OptionsSmoothPanel,
              mode == CurveOptionsMode::SmoothLowpass || mode == CurveOptionsMode::SmoothPeak);

    if (m_SmoothTitleLabel)
    {
        m_SmoothTitleLabel->SetText(mode == CurveOptionsMode::SmoothPeak
            ? std::string("Smooth Options (Peak-Preserving)")
            : std::string("Smooth Options (Low-Pass)"));
    }

    if (m_OptionsBar) m_OptionsBar->RemoveClass("hidden");

    UpdateCurveOptionsToolbarButtonStates();
    RefreshOptionsPreview();
}

void AnimationWindowPanel::ExitCurveOptionsMode(bool commit)
{
    if (m_CurveOptionsMode == CurveOptionsMode::None)
        return;

    if (commit)
    {
        if (m_UndoRedo && m_CurrentClipAsset && !m_CurveOptionsBaseline.empty())
        {
            Vector<uint8> afterSnapshot;
            if (m_CurrentClipAsset->SaveToData(afterSnapshot))
            {
                const std::vector<std::uint8_t> afterBytes(afterSnapshot.begin(), afterSnapshot.end());
                if (afterBytes != m_CurveOptionsBaseline)
                {
                    const char* name =
                        (m_CurveOptionsMode == CurveOptionsMode::Noise)      ? "Add Curve Noise" :
                        (m_CurveOptionsMode == CurveOptionsMode::Simplify)   ? "Simplify Animation Curve" :
                        (m_CurveOptionsMode == CurveOptionsMode::SmoothPeak) ? "Smooth Curve (Peak-Preserving)"
                                                                             : "Smooth Curve (Low-Pass)";
                    auto cmd = std::make_unique<AnimationClipSnapshotCommand>(
                        std::string(name),
                        m_CurrentClipAsset,
                        m_CurveOptionsBaseline,
                        afterBytes,
                        [this](AnimationClip* clip)
                        {
                            if (!clip) return;
                            MarkDirty();
                            if (m_CurrentClip == clip) RefreshDiscreteClipEditState();
                        });
                    m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
                    MarkDirty();
                }
            }
        }
    }
    else
    {
        RestoreOptionsBaseline();
        RefreshEditedClipState();
    }

    m_CurveOptionsMode = CurveOptionsMode::None;
    m_CurveOptionsBaseline.clear();
    // Keep the bar visible if the Lattice tool is still on — its config
    // panel uses the same overlay.
    const bool latticeOn = m_CurvesGraphView &&
        m_CurvesGraphView->GetActiveTool() == CurvesGraphView::CurveTool::Lattice;
    if (m_OptionsBar && !latticeOn) m_OptionsBar->AddClass("hidden");
    auto setActive = [](UIElement* panel, bool on)
    {
        if (!panel) return;
        if (on) panel->AddClass("anim-options-active");
        else    panel->RemoveClass("anim-options-active");
    };
    setActive(m_OptionsNoisePanel,    false);
    setActive(m_OptionsSimplifyPanel, false);
    setActive(m_OptionsSmoothPanel,   false);

    UpdateCurveOptionsToolbarButtonStates();
}

void AnimationWindowPanel::RestoreOptionsBaseline()
{
    if (!m_CurrentClipAsset || m_CurveOptionsBaseline.empty()) return;
    Vector<uint8> data(m_CurveOptionsBaseline.begin(), m_CurveOptionsBaseline.end());
    m_CurrentClipAsset->RestoreFromData(data);
}

void AnimationWindowPanel::RefreshOptionsPreview()
{
    if (m_CurveOptionsMode == CurveOptionsMode::None) return;
    RestoreOptionsBaseline();
    ApplyCurveOptionsOperation();
    RefreshEditedClipState();
}

void AnimationWindowPanel::UpdateCurveOptionsToolbarButtonStates()
{
    auto setPressed = [&](const char* id, bool pressed)
    {
        if (auto* btn = dynamic_cast<Button*>(FindById(id)))
        {
            if (pressed) btn->AddClass("pressed");
            else         btn->RemoveClass("pressed");
        }
    };
    setPressed("AnimationWindowSmoothLowpass", m_CurveOptionsMode == CurveOptionsMode::SmoothLowpass);
    setPressed("AnimationWindowSmoothPeak",    m_CurveOptionsMode == CurveOptionsMode::SmoothPeak);
    setPressed("AnimationWindowCurveSimplify", m_CurveOptionsMode == CurveOptionsMode::Simplify);
    setPressed("AnimationWindowAddNoise",      m_CurveOptionsMode == CurveOptionsMode::Noise);
}

void AnimationWindowPanel::ApplyCurveOptionsOperation()
{
    if (!m_CurrentClipAsset) return;

    std::unordered_map<int, std::unordered_set<float>> selByChannel;
    auto collect = [&](const auto& selKeys) {
        for (const auto& sk : selKeys)
            if (sk.Channel >= 0) selByChannel[sk.Channel].insert(sk.KeyTime);
    };
    if (m_CurvesGraphView) collect(m_CurvesGraphView->GetSelectedKeyframes());
    if (m_DopeSheetView)   collect(m_DopeSheetView->GetSelectedKeyframes());

    const bool hasSel = !selByChannel.empty();
    if (!hasSel && m_SelectedChannel < 0) return;

    auto forEachAffectedChannel = [&](auto&& fn)
    {
        auto applyUnlocked = [&](size_t channel, const std::unordered_set<float>* filter)
        {
            for (TreeId id : m_LockedTreeIds)
            {
                if (const auto binding = m_ChannelBindings.find(id); binding != m_ChannelBindings.end() &&
                    static_cast<size_t>(binding->second.first) == channel) return;
                if (const auto node = m_TreeNodeChannels.find(id); node != m_TreeNodeChannels.end() &&
                    std::find(node->second.begin(), node->second.end(), static_cast<int>(channel)) != node->second.end()) return;
            }
            fn(channel, filter);
        };
        if (hasSel)
            for (const auto& kv : selByChannel)
                applyUnlocked(static_cast<size_t>(kv.first), &kv.second);
        else
            applyUnlocked(static_cast<size_t>(m_SelectedChannel), nullptr);
    };

    const auto& channels = m_CurrentClipAsset->GetChannels();

    switch (m_CurveOptionsMode)
    {
    case CurveOptionsMode::Noise:
    {
        // Deterministic seed mixing the parameter values, so previews don't
        // shimmer on each refresh and dragging a value back gives the same
        // pattern again. Frequency is currently advisory — the noise applies
        // per-keyframe; surfacing it in the schema lets us add Hz-based
        // re-sampling later without changing the UI.
        std::mt19937 rng(static_cast<uint32_t>(
            static_cast<uint32_t>(m_NoiseFreqMin * 73856093u) ^
            static_cast<uint32_t>(m_NoiseFreqMax * 19349663u) ^
            static_cast<uint32_t>(static_cast<int>(m_NoiseMagnitude * 1000.0f) * 83492791u)));
        std::uniform_real_distribution<float> mag(-m_NoiseMagnitude, m_NoiseMagnitude);

        forEachAffectedChannel([&](size_t chIdx, const std::unordered_set<float>* filter)
        {
            if (chIdx >= channels.size()) return;
            const AnimChannel& ch = channels[chIdx];
            if (ch.keys.empty()) return;
            const uint32 compCount = GetComponentCountByPath(ch.path);
            for (const AnimKeyframe& key : ch.keys)
            {
                if (filter && !filter->count(key.time)) continue;
                for (uint32 c = 0; c < compCount; ++c)
                {
                    const float* src = (ch.path == AnimPath::Translation) ? key.translation
                                     : (ch.path == AnimPath::Rotation)    ? key.rotation
                                                                          : key.scale;
                    m_CurrentClipAsset->SetKeyframeComponentValue(chIdx, key.time, c, src[c] + mag(rng));
                }
            }
        });
        break;
    }

    case CurveOptionsMode::Simplify:
    {
        const float tolerance = std::max(0.0f, m_SimplifyValueTol);
        forEachAffectedChannel([&](size_t chIdx, const std::unordered_set<float>* filter)
        {
            if (chIdx >= channels.size()) return;
            const AnimChannel& ch = channels[chIdx];
            if (ch.keys.size() < 3) return;
            const uint32 compCount = GetComponentCountByPath(ch.path);

            const auto work = ReduceCurveKeys(ch.keys, m_SimplifyTimeTol, tolerance,
                [&](const auto& original, const auto& candidate, float time) {
                    float error = 0.0f;
                    for (uint32 c = 0; c < compCount; ++c)
                    {
                        const float difference = std::fabs(
                            EvalKeyframeCurveValue(original, ch.path, ch.interp, c, time) -
                            EvalKeyframeCurveValue(candidate, ch.path, ch.interp, c, time));
                        if (!std::isfinite(difference)) return std::numeric_limits<float>::infinity();
                        error = std::max(error, difference);
                    }
                    return error;
                }, [&](const AnimKeyframe& key) { return !filter || filter->count(key.time) != 0; });

            std::unordered_set<float> kept;
            kept.reserve(work.size());
            for (const auto& k : work) kept.insert(k.time);
            std::vector<float> removedTimes;
            for (const auto& k : ch.keys)
                if (!kept.count(k.time)) removedTimes.push_back(k.time);
            for (float time : removedTimes)
                m_CurrentClipAsset->RemoveKeyframe(chIdx, time);
        });
        break;
    }

    case CurveOptionsMode::SmoothLowpass:
    case CurveOptionsMode::SmoothPeak:
    {
        // Filter Width or Sample Count == 0 → no-op (the user explicitly
        // dialled smoothing off; preview should reflect the baseline).
        if (m_SmoothFilterWidth <= 0 || m_SmoothSampleCount <= 0) break;
        const bool peakPreserve = (m_CurveOptionsMode == CurveOptionsMode::SmoothPeak);
        const int radius = m_SmoothFilterWidth;
        const int passes = m_SmoothSampleCount;
        const float sigma = std::max(1.0f, radius * 0.5f);
        std::vector<float> weights(static_cast<size_t>(radius) + 1u);
        for (int d = 0; d <= radius; ++d)
            weights[static_cast<size_t>(d)] = std::exp(-(d * d) / (2.0f * sigma * sigma));

        forEachAffectedChannel([&](size_t chIdx, const std::unordered_set<float>* filter)
        {
            if (chIdx >= channels.size()) return;
            const AnimChannel& ch = channels[chIdx];
            const size_t keyCount = ch.keys.size();
            if (keyCount < 3) return;
            const uint32 compCount = GetComponentCountByPath(ch.path);

            std::vector<float> values(keyCount * compCount);
            for (size_t i = 0; i < keyCount; ++i)
                for (uint32 c = 0; c < compCount; ++c)
                    values[i * compCount + c] = GetKeyComponentByPath(ch.keys[i], ch.path, c);

            std::vector<uint8_t> isExtremum(keyCount, 0);
            if (peakPreserve)
            {
                // Endpoints are pinned so smoothing doesn't drift them; an
                // interior key is a peak if any component reverses across it.
                for (size_t i = 0; i < keyCount; ++i)
                {
                    if (i == 0 || i + 1 == keyCount) { isExtremum[i] = 1; continue; }
                    bool extremum = false;
                    for (uint32 c = 0; c < compCount && !extremum; ++c)
                    {
                        const float vp = values[(i - 1) * compCount + c];
                        const float vc = values[i * compCount + c];
                        const float vn = values[(i + 1) * compCount + c];
                        if ((vc >= vp && vc >= vn) || (vc <= vp && vc <= vn))
                            extremum = true;
                    }
                    isExtremum[i] = extremum ? 1u : 0u;
                }
            }

            std::vector<float> next(values);
            for (int p = 0; p < passes; ++p)
            {
                for (size_t i = 0; i < keyCount; ++i)
                {
                    if (peakPreserve && isExtremum[i]) continue;
                    for (uint32 c = 0; c < compCount; ++c)
                    {
                        float wsum = weights[0];
                        float vsum = values[i * compCount + c] * weights[0];
                        for (int d = 1; d <= radius; ++d)
                        {
                            const float w = weights[static_cast<size_t>(d)];
                            const ptrdiff_t li = static_cast<ptrdiff_t>(i) - d;
                            const ptrdiff_t ri = static_cast<ptrdiff_t>(i) + d;
                            if (li >= 0)
                            {
                                vsum += values[static_cast<size_t>(li) * compCount + c] * w;
                                wsum += w;
                            }
                            if (ri < static_cast<ptrdiff_t>(keyCount))
                            {
                                vsum += values[static_cast<size_t>(ri) * compCount + c] * w;
                                wsum += w;
                            }
                        }
                        next[i * compCount + c] = (wsum > 0.0f) ? (vsum / wsum) : values[i * compCount + c];
                    }
                }
                values.swap(next);
            }

            for (size_t i = 0; i < keyCount; ++i)
            {
                if (filter && !filter->count(ch.keys[i].time)) continue;
                if (peakPreserve && isExtremum[i]) continue;
                for (uint32 c = 0; c < compCount; ++c)
                    m_CurrentClipAsset->SetKeyframeComponentValue(chIdx, ch.keys[i].time, c, values[i * compCount + c]);
            }
        });
        break;
    }

    case CurveOptionsMode::None:
        break;
    }
}

} // namespace GameEngine
