#include "EditorContextMenu/EditorContextMenu.h"

#include "Platform/ContextMenu.h"
#include "Platform/Clipboard.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Types/StringUtils.h"
#include "Platform/Shell.h"
#include "Platform/Window.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "AssetCore/AssetTypes.h"
#include "Editor/Assets/EditorAssetActions.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "Editor/Settings/SettingsStore.h"
#include "Panels/SettingsPanel.h"
#include "Assets/AssetRegistry.h"
#include "Assets/CodeAssetTemplates.h"
#include "UI/EditorTags.h"
#include "UI/EditorIcons.h"

#include <string>

namespace GameEngine
{

namespace
{
constexpr uint32_t kCmdAssetOpen = 0x3001;
constexpr uint32_t kCmdAssetEditInternal = 0x3010;
constexpr uint32_t kCmdAssetShowInFileManager = 0x3002;
constexpr uint32_t kCmdAssetCopyPath = 0x3003;
constexpr uint32_t kCmdAssetAddToBookmarks = 0x3012;
constexpr uint32_t kCmdAssetAddTag = 0x3013;
constexpr uint32_t kCmdAssetDelete = 0x301F;
constexpr uint32_t kCmdAssetRename = 0x3015;
constexpr uint32_t kCmdAssetOpenSettingsTags = 0x500F;
constexpr uint32_t kCmdAssetAssignTagBase = 0x5010;
constexpr uint32_t kCmdAssetAssignTagMaxCount = 64;
constexpr uint32_t kCmdDirectoryNewFolder = 0x3004;
constexpr uint32_t kCmdDirectoryNewSmartFolder = 0x300C;
constexpr uint32_t kCmdDirectoryRefresh = 0x3005;
constexpr uint32_t kCmdDirectoryCreateScene = 0x300A;
constexpr uint32_t kCmdDirectoryCreateCSharpScript = 0x300B;
constexpr uint32_t kCmdDirectoryCreateMaterial = 0x300D;
constexpr uint32_t kCmdDirectoryCreateShaderGraph = 0x3023;
constexpr uint32_t kCmdDirectoryCreateSurfaceShader = 0x300E;
constexpr uint32_t kCmdDirectoryCreateNavGrid = 0x300F;
constexpr uint32_t kCmdDirectoryCreateNavMesh = 0x3014;
// The six "Create code asset" command ids live in Editor::kCmdCreate* (CodeAssetTemplates.h)
// so the descriptor table and this dispatch switch share a single source of truth.
constexpr uint32_t kCmdDirectoryCreateAnimationLibrary = 0x3019;
constexpr uint32_t kCmdDirectoryCreateAnimationController = 0x301A;
constexpr uint32_t kCmdDirectoryCreateTimeline = 0x301B;
constexpr uint32_t kCmdDirectoryCreateClipSet = 0x301C;
constexpr uint32_t kCmdDirectoryCreateSpriteFrames = 0x301D;
constexpr uint32_t kCmdDirectoryImport = 0x301E;
constexpr uint32_t kCmdDirectoryDelete = 0x3022;
constexpr uint32_t kCmdAssetDbFixUpRedirects = 0x3006;
constexpr uint32_t kCmdAssetDbReportMissing = 0x3007;
constexpr uint32_t kCmdAssetDbReportConflicts = 0x3008;
constexpr uint32_t kCmdAssetDbSaveNow = 0x3009;
constexpr uint32_t kCmdRenderPipelineSetActive = 0x3011;
constexpr uint32_t kCmdSmartFolderNew = 0x3020;
constexpr uint32_t kCmdSmartFolderDelete = 0x3021;
constexpr uint32_t kCmdPhantomRemoveFromScene = 0x3030;
constexpr uint32_t kCmdPhantomCopyGuid = 0x3031;

// VCS commands (generic, provider resolved through EditorVcsProviderRegistry)
constexpr uint32_t kCmdVcsAdd = 0x4001;
constexpr uint32_t kCmdVcsRevert = 0x4002;
constexpr uint32_t kCmdVcsCommit = 0x4003;
constexpr uint32_t kCmdVcsUpdate = 0x4005;
constexpr uint32_t kCmdVcsShowLog = 0x4006;
constexpr uint32_t kCmdVcsDiff = 0x4007;
// Provider-contributed extras map LocalId into this reserved range.
constexpr uint32_t kCmdVcsProviderExtraBase = 0x4100;
constexpr uint32_t kCmdVcsProviderExtraMaxCount = 0x100;

// Adds the active provider's contributed items under its display-name root,
// mapping provider-local ids into the reserved command range.
void AppendProviderMenuItems(ContextMenuBuilder& builder,
                             const Editor::EditorVcsProviderDescriptor& provider,
                             bool isDirectory)
{
    if (!provider.CollectMenuItems)
        return;

    std::vector<Editor::VcsMenuItem> items;
    provider.CollectMenuItems(isDirectory, items);
    for (const Editor::VcsMenuItem& item : items)
    {
        if (item.Label.empty() || item.LocalId >= kCmdVcsProviderExtraMaxCount)
        {
            Logger::Log::Error("VCS: provider '{}' contributed an invalid menu item "
                               "('{}', local id {})",
                               provider.TypeId, item.Label, item.LocalId);
            continue;
        }
        builder.AddItem(provider.DisplayName + "/" + item.Label,
                        kCmdVcsProviderExtraBase + item.LocalId, MenuItemFlag_None, 0,
                        EditorIcons::kVcs);
    }
}


static bool TryMakeAssetRelativePath(const std::filesystem::path& absPath,
                                     std::filesystem::path& outRel)
{
    outRel.clear();
    if (absPath.empty())
        return false;

    auto& eng = EngineCore::GetInstance();
    auto& am = eng.GetAssetManager();

    std::error_code ec;
    const std::filesystem::path absNorm = std::filesystem::weakly_canonical(absPath, ec).lexically_normal();
    if (ec)
        return false;

    auto tryRoot = [&](const std::filesystem::path& rootAbs) -> bool
    {
        ec.clear();
        const std::filesystem::path rootNorm = std::filesystem::weakly_canonical(rootAbs, ec).lexically_normal();
        if (ec || rootNorm.empty())
            return false;
        // path is under root?
        const auto rel = absNorm.lexically_relative(rootNorm);
        const std::string relStr = rel.generic_string();
        if (rel.empty() || relStr.rfind("..", 0) == 0)
            return false;
        outRel = rel.lexically_normal();
        return !outRel.empty();
    };

    if (tryRoot(am.GetAssetRoot()))
        return true;
    for (const auto& source : am.GetRegisteredSources())
    {
        if (tryRoot(source.Root))
            return true;
    }
    return false;
}

static void SetActiveRenderPipelineFromAbsolutePath(const std::filesystem::path& absPath)
{
    std::filesystem::path rel;
    if (!TryMakeAssetRelativePath(absPath, rel))
    {
        Logger::Log::Warning("RenderPipeline: could not resolve asset-relative path for '{}'", absPath.string());
        return;
    }

    // Persist in project settings and apply live.
    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    auto& r = root["rendering"];
    if (!r.is_object())
        r = nlohmann::json::object();
    r["activeRenderPipeline"] = rel.generic_string();
    (void)store.Save(&err);

    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        rs->Spine().SetActiveRenderPipelinePath(rel);
    }

    SettingsPanel::NotifyActivePipelineChanged(rel.generic_string());
}
} // namespace

EditorContextMenu::EditorContextMenu(Platform::Window* window)
    : m_Window(window)
{
}

EditorContextMenu::~EditorContextMenu() = default;

void EditorContextMenu::SetWindow(Platform::Window* window)
{
    m_Window = window;
}

void EditorContextMenu::EnsureMenu()
{
    if (m_Menu)
    {
        return;
    }

    // Route through the editor factory so this menu rides the
    // InterceptableContextMenu decorator like every other editor menu.
    m_Menu = CreateContextMenu();

    if (!m_Menu)
    {
        return;
    }

    m_Menu->SetCommandHandler([this](uint32_t cmd)
                              {
        // Smart-folder commands are dispatched by the Smart Folders section
        // context menu, which does not carry an asset path. Handle them
        // before the path.empty() guard so they still fire.
        if (cmd == kCmdSmartFolderNew || cmd == kCmdSmartFolderDelete)
        {
            if (m_OnSmartFolderAction)
            {
                m_OnSmartFolderAction(cmd == kCmdSmartFolderNew
                                          ? SmartFolderAction::NewSmartFolder
                                          : SmartFolderAction::Delete);
            }
            return;
        }
        // Phantom-row commands also bypass the asset-path guard — phantoms
        // are by definition references to a path that doesn't exist.
        if (cmd == kCmdPhantomRemoveFromScene)
        {
            if (m_OnPhantomRemoveFromScene)
                m_OnPhantomRemoveFromScene();
            return;
        }
        if (cmd == kCmdPhantomCopyGuid)
        {
            if (m_OnPhantomCopyGuid)
                m_OnPhantomCopyGuid();
            return;
        }

        const std::filesystem::path path = m_AssetPath;
        if (path.empty())
        {
            return;
        }

        switch (cmd)
        {
        case kCmdAssetOpen:
            if (m_OnOpenAction)
            {
                m_OnOpenAction(path, m_IsDirectory);
            }
            else
            {
                // Fallback: open via OS shell. For directories we open the folder,
                // for files we open the file itself.
                OpenAsset(path);
            }
            break;
        case kCmdAssetEditInternal:
            if (m_OnEditAction)
            {
                m_OnEditAction(path);
            }
            break;
        case kCmdAssetAddToBookmarks:
            if (m_OnAddToBookmarks && !m_PathsForBookmarks.empty())
            {
                m_OnAddToBookmarks(m_PathsForBookmarks);
            }
            break;
        case kCmdAssetRename:
            if (m_OnRename)
                m_OnRename(path);
            break;
        case kCmdAssetDelete:
            if (m_OnDelete)
            {
                const std::vector<std::filesystem::path> paths = m_PathsForBookmarks.empty()
                    ? std::vector<std::filesystem::path>{path}
                    : m_PathsForBookmarks;
                m_OnDelete(paths);
            }
            break;
        case kCmdAssetAddTag:
            if (m_OnAddTag)
            {
                std::vector<std::filesystem::path> paths = m_PathsForBookmarks.empty()
                    ? std::vector<std::filesystem::path>{path}
                    : m_PathsForBookmarks;
                m_OnAddTag(path, paths);
            }
            break;
        case kCmdAssetOpenSettingsTags:
            if (m_OnOpenSettingsToTags)
                m_OnOpenSettingsToTags();
            break;
        case kCmdRenderPipelineSetActive:
        {
            try
            {
                SetActiveRenderPipelineFromAbsolutePath(path);
            }
            catch (...)
            {
            }
            break;
        }
	    case kCmdAssetShowInFileManager:
	    	        ShowInFileManager(path);
            break;
	        case kCmdAssetCopyPath:
	            CopyPathToClipboard(path);
	            break;
	        case kCmdDirectoryNewFolder:
	            if (m_OnDirectoryAction)
	            {
	                m_OnDirectoryAction(path, DirectoryAction::NewFolder);
	            }
	            break;
	        case kCmdDirectoryNewSmartFolder:
	            if (m_OnDirectoryAction)
	            {
	                m_OnDirectoryAction(path, DirectoryAction::NewSmartFolder);
	            }
	            break;
	        case kCmdDirectoryRefresh:
	            if (m_OnDirectoryAction)
	            {
	                m_OnDirectoryAction(path, DirectoryAction::Refresh);
	            }
	            break;
            case kCmdDirectoryCreateScene:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateScene);
                }
                break;
            case kCmdDirectoryCreateCSharpScript:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateCSharpScript);
                }
                break;
            case kCmdDirectoryCreateMaterial:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateMaterial);
                }
                break;
            case kCmdDirectoryCreateAnimationLibrary:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateAnimationLibrary);
                }
                break;
            case kCmdDirectoryCreateAnimationController:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateAnimationController);
                }
                break;
            case kCmdDirectoryCreateTimeline:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateTimeline);
                }
                break;
            case kCmdDirectoryCreateClipSet:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateClipSet);
                }
                break;
            case kCmdDirectoryCreateSpriteFrames:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateSpriteFrames);
                }
                break;
            case kCmdDirectoryCreateShaderGraph:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateShaderGraph);
                }
                break;
            case kCmdDirectoryCreateSurfaceShader:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateSurfaceShader);
                }
                break;
            case kCmdDirectoryCreateNavGrid:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateNavGrid);
                }
                break;
            case kCmdDirectoryCreateNavMesh:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::CreateNavMesh);
                }
                break;
            case kCmdDirectoryImport:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::Import);
                }
                break;
            case kCmdDirectoryDelete:
                if (m_OnDirectoryAction)
                {
                    m_OnDirectoryAction(path, DirectoryAction::Delete);
                }
                break;
            case Editor::kCmdCreateGameSystem:
            case Editor::kCmdCreateEntitySystem:
            case Editor::kCmdCreateComponent:
            case Editor::kCmdCreateCppComponent:
            case Editor::kCmdCreateCppGameSystem:
            case Editor::kCmdCreateCppEntitySystem:
                if (m_OnDirectoryAction)
                {
                    std::size_t count = 0;
                    const auto* table = Editor::GetCodeAssetDescriptors(count);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        if (table[i].CommandId == cmd)
                        {
                            m_OnDirectoryAction(path, table[i].Action);
                            break;
                        }
                    }
                }
                break;
            case kCmdAssetDbFixUpRedirects:
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    if (eng.IsInitialized())
                    {
                        const size_t changed = eng.GetAssetManager().GetRegistry().FixUpRedirects();
                        Logger::Log::Info("Fix Up Redirects: updated {} asset files", changed);
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdAssetDbReportMissing:
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    if (eng.IsInitialized())
                    {
                        auto missing = eng.GetAssetManager().GetRegistry().GetMissingAssets();
                        Logger::Log::Info("Missing assets: {}", missing.size());
                        for (const auto& m : missing)
                        {
                            Logger::Log::Info("  {}  type={}  dependents={}  path={}",
                                              m.guid.ToString(),
                                              AssetTypeToString(m.type),
                                              m.dependentCount,
                                              m.lastKnownPath.string());
                        }
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdAssetDbReportConflicts:
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    if (eng.IsInitialized())
                    {
                        auto conflicts = eng.GetAssetManager().GetRegistry().GetAssetDatabaseConflicts();
                        Logger::Log::Info("Asset DB conflicts: {}", conflicts.size());
                        for (const auto& c : conflicts)
                        {
                            Logger::Log::Info("  {}", c);
                        }
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdAssetDbSaveNow:
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    if (eng.IsInitialized())
                    {
                        const bool ok = eng.GetAssetManager().GetRegistry().SaveToFile({});
                        Logger::Log::Info("Asset DB save: {}", ok ? "OK" : "FAILED");
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdVcsAdd:
            {
                try
                {
                    auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
                    if (vcs && vcs->IsRepository())
                    {
                        vcs->Add(path);
                        Logger::Log::Info("VCS: Added {}", path.string());
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdVcsRevert:
            {
                if (m_OnVcsRevert)
                {
                    m_OnVcsRevert(path);
                }
                else
                {
                    // Fallback: directly revert without undo support
                    try
                    {
                        auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
                        if (vcs && vcs->IsRepository())
                        {
                            vcs->Revert(path);
                            Logger::Log::Info("VCS: Reverted {}", path.string());
                        }
                    }
                    catch (...)
                    {
                    }
                }
                break;
            }
            case kCmdVcsCommit:
            {
                if (m_OnShowCommitDialog)
                {
                    // Get default commit message from settings
                    std::string defaultMsg = "";
                    m_OnShowCommitDialog(defaultMsg);
                }
                else
                {
                    Logger::Log::Warning("VCS: Commit dialog callback not set");
                }
                break;
            }
            case kCmdVcsUpdate:
            {
                try
                {
                    Editor::EditorVcsProviderDescriptor provider;
                    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
                        provider.Integration().IsRepository())
                    {
                        provider.Integration().Update();
                        Logger::Log::Info("{}: {} completed", provider.DisplayName,
                                          provider.UpdateActionLabel.empty()
                                              ? std::string("Update")
                                              : provider.UpdateActionLabel);
                    }
                }
                catch (...)
                {
                }
                break;
            }
            case kCmdVcsShowLog:
            {
                if (m_OnShowVcsLog)
                {
                    m_OnShowVcsLog(path);
                }
                else
                {
                    Logger::Log::Warning("VCS: Log viewer callback not set");
                }
                break;
            }
            case kCmdVcsDiff:
            {
                // Use internal diff panel instead of external tool
                if (m_OnShowDiff)
                {
                    m_OnShowDiff(path);
                }
                else
                {
                    // Fallback to the provider's external diff tool
                    try
                    {
                        Editor::EditorVcsProviderDescriptor provider;
                        if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
                            provider.OpenExternalDiff)
                        {
                            provider.OpenExternalDiff(path);
                        }
                    }
                    catch (...)
                    {
                        Logger::Log::Warning("VCS: Exception opening diff");
                    }
                }
                break;
            }
	        default:
	            // Provider-contributed VCS items route back to the active provider.
	            if (cmd >= kCmdVcsProviderExtraBase &&
	                cmd < kCmdVcsProviderExtraBase + kCmdVcsProviderExtraMaxCount)
	            {
	                try
	                {
	                    Editor::EditorVcsProviderDescriptor provider;
	                    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
	                        provider.HandleMenuCommand)
	                    {
	                        provider.HandleMenuCommand(cmd - kCmdVcsProviderExtraBase, path);
	                    }
	                }
	                catch (...)
	                {
	                }
	                break;
	            }
	            // Tags submenu: assign tag by command id
	            if (cmd >= kCmdAssetAssignTagBase && cmd < kCmdAssetAssignTagBase + kCmdAssetAssignTagMaxCount && m_OnAssignTag)
	            {
	                const std::vector<std::filesystem::path> paths = m_PathsForBookmarks.empty()
	                    ? std::vector<std::filesystem::path>{path}
	                    : m_PathsForBookmarks;
	                const std::vector<EditorTagDefinition> tags = EditorTags::Load();
	                const size_t idx = static_cast<size_t>(cmd - kCmdAssetAssignTagBase);
	                if (idx < tags.size())
	                    m_OnAssignTag(paths, tags[idx].Name);
	                break;
	            }
	            // Items other modules registered for folders.
	            if (m_IsDirectory && Editor::EditorMenuRegistry::Get().TryInvokeDirectoryItem(cmd, path))
	                break;
	            // Script-driven context menu items (Editor-managed snapshot)
	            {
	                uint64_t dom = 0;
	                std::string method;
	                if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(cmd, dom, method) && !method.empty())
	                {
	                    try
	                    {
	                        auto& eng = EngineCore::GetInstance();
	                        auto& clr = eng.GetScriptManager().GetCLRHost();
	                        int32_t out = 0;
	                        (void)clr.InvokeInDomain(dom, method.c_str(), (uint32_t)method.size(), &out);
	                    }
	                    catch (...)
	                    {
	                    }
	                }
	            }
	            break;
	        } });
}

void EditorContextMenu::ShowAssetMenu(const std::filesystem::path& assetPath,
                                      bool isDirectory,
                                      float x, float y,
                                      const std::function<void(const std::filesystem::path&, bool)>& onOpenAction,
                                      const std::function<void(const std::filesystem::path&)>& onEditAction,
                                      const std::vector<std::filesystem::path>* pathsForBookmarks,
                                      const std::function<void(const std::vector<std::filesystem::path>&)>& onAddToBookmarks,
                                      const std::function<void(const std::vector<std::filesystem::path>&)>& onDelete,
                                      const std::function<void(const std::filesystem::path&)>& onRename)
{
    if (!m_Window)
    {
        return;
    }

    if (assetPath.empty())
    {
        return;
    }

    EnsureMenu();
    if (!m_Menu)
    {
        return;
    }

    m_AssetPath = assetPath;
    m_IsDirectory = isDirectory;
    m_PathsForBookmarks.clear();
    if (pathsForBookmarks && !pathsForBookmarks->empty())
        m_PathsForBookmarks = *pathsForBookmarks;
    m_OnOpenAction = onOpenAction;
    m_OnEditAction = onEditAction;
    m_OnAddToBookmarks = onAddToBookmarks;
    m_OnDelete = onDelete;
    m_OnRename = onRename;
    m_OnDirectoryAction = nullptr;

    m_Menu->Clear();

    ContextMenuBuilder builder;
    const std::string showLabel = Editor::ShowInFileManagerLabel();
    constexpr int kPriorityTagsFirst = -1000;

    // Tags first (Finder-style): click toggles tag on selection; colored dots from tag definition
    std::vector<EditorTagDefinition> tagsForDots;
    if (!isDirectory && (m_OnAssignTag || m_OnOpenSettingsToTags))
    {
        tagsForDots = EditorTags::Load();
        builder.AddItem("Tags", 0, MenuItemFlag_None, kPriorityTagsFirst, EditorIcons::kTag);
        for (size_t i = 0; i < tagsForDots.size() && i < kCmdAssetAssignTagMaxCount; ++i)
            builder.AddItem("Tags/" + tagsForDots[i].Name, kCmdAssetAssignTagBase + static_cast<uint32_t>(i), MenuItemFlag_None, kPriorityTagsFirst);
        if (m_OnOpenSettingsToTags)
            builder.AddItem("Tags/All Tags…", kCmdAssetOpenSettingsTags, MenuItemFlag_None, kPriorityTagsFirst, EditorIcons::kTag);
    }
    else if (!isDirectory && m_OnAddTag)
    {
        std::string tagLabel = "Add tag";
        std::string tagMeta;
        if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetMetaValue(assetPath, "tags", tagMeta))
        {
            const size_t start = tagMeta.find_first_not_of(" \t\r\n,");
            if (start != std::string::npos)
                tagLabel = "Edit tag";
        }
        builder.AddItem(tagLabel, kCmdAssetAddTag, MenuItemFlag_None, kPriorityTagsFirst,
                        tagLabel == "Edit tag" ? EditorIcons::kBrush : EditorIcons::kPlus);
    }

    builder.AddItem("Open", kCmdAssetOpen, MenuItemFlag_None, 0, EditorIcons::kFolderOpen)
        .AddItem(showLabel, kCmdAssetShowInFileManager, MenuItemFlag_None, 0, EditorIcons::kEye)
        .AddItem("Copy Full Path", kCmdAssetCopyPath, MenuItemFlag_None, 0, EditorIcons::kCopy);

    // Add "Add to Bookmarks" option if callback and paths are provided
    if (onAddToBookmarks && !m_PathsForBookmarks.empty())
    {
        std::string label = (m_PathsForBookmarks.size() == 1)
            ? "Add to Bookmarks"
            : ("Add " + std::to_string(m_PathsForBookmarks.size()) + " to Bookmarks");
        builder.AddItem(label, kCmdAssetAddToBookmarks, MenuItemFlag_None, 0, EditorIcons::kPlus);
    }

    // RenderPipeline convenience: allow activating a .rendergraph directly from the Assets browser.
    // This sets ProjectSettings.json + updates RenderServices live.
    if (!isDirectory)
    {
        std::string ext = ToLowerAscii(assetPath.extension().string());
        if (ext == ".rendergraph" || ext == ".renderpipeline")
        {
            builder.AddItem("Render Pipeline", 0, MenuItemFlag_None, -500, EditorIcons::kSettings);
            builder.AddItem("Render Pipeline/Set Active", kCmdRenderPipelineSetActive, MenuItemFlag_None, -500, EditorIcons::kSettings);
        }
    }

    // If a handler is provided, allow opening C# scripts in the internal Script Editor.
    if (!isDirectory && m_OnEditAction)
    {
        std::string ext = ToLowerAscii(assetPath.extension().string());
        if (ext == ".cs" || ext == ".rendergraph" || ext == ".renderpipeline" || ext == ".glsl" || ext == ".hlsl")
        {
            builder.AddItem("Edit (Internal)", kCmdAssetEditInternal, MenuItemFlag_None, 0, EditorIcons::kBrush);
        }
    }

    Editor::EditorVcsProviderDescriptor activeProvider;
    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(activeProvider) &&
        activeProvider.Integration().IsRepository())
    {
        const std::string& vcsName = activeProvider.DisplayName;

        // Avoid synchronous status queries here. The UI renders status independently.
        builder.AddItem(vcsName, 0, MenuItemFlag_None, 0, EditorIcons::kVcs);
        if (!isDirectory)
        {
            builder.AddItem(vcsName + "/Add", kCmdVcsAdd, MenuItemFlag_None, 0, EditorIcons::kPlus)
                .AddItem(vcsName + "/Revert", kCmdVcsRevert, MenuItemFlag_None, 0, EditorIcons::kReset)
                .AddItem(vcsName + "/Diff", kCmdVcsDiff, MenuItemFlag_None, 0, EditorIcons::kVcs);
        }

        builder.AddItem(vcsName + "/Show Log", kCmdVcsShowLog, MenuItemFlag_None, 0, EditorIcons::kEye);

        // Server-backed VCS: offer the update action (pull from remote) per file.
        if (activeProvider.ServerBacked && !activeProvider.UpdateActionLabel.empty())
        {
            builder.AddItem(vcsName + "/" + activeProvider.UpdateActionLabel, kCmdVcsUpdate,
                            MenuItemFlag_None, 0, EditorIcons::kVcs);
        }

        AppendProviderMenuItems(builder, activeProvider, isDirectory);
    }

    // Append script items targeting Assets item context menu
    {
        const uint32_t mask = static_cast<uint32_t>(Editor::EditorContextMenuTarget::AssetsItem);
        auto scriptItems = Editor::ScriptMenuRegistry::Get().GetContextItems(mask);
        for (const auto& si : scriptItems)
        {
            if (si.commandId == 0 || si.path.empty())
                continue;
            builder.AddItem(si.path, si.commandId, MenuItemFlag_None, si.priority, EditorIcons::kScript);
        }
    }

    // Sorted by (priority, path): the separator leads, Rename sits above Delete.
    if (onRename || onDelete)
        builder.AddItem("---file-ops", 0, MenuItemFlag_None, 998);
    if (onRename)
        builder.AddItem("Rename", kCmdAssetRename, MenuItemFlag_None, 999, EditorIcons::kPencil);
    if (onDelete)
        builder.AddItem("Delete", kCmdAssetDelete, MenuItemFlag_None, 1000, EditorIcons::kTrash);

    builder.Build(m_Menu.get());

    // Colored dots for tag items (from EditorTagDefinition.color)
    for (size_t i = 0; i < tagsForDots.size() && i < kCmdAssetAssignTagMaxCount; ++i)
    {
        if (!tagsForDots[i].Color.empty())
            m_Menu->SetItemColor(kCmdAssetAssignTagBase + static_cast<uint32_t>(i), tagsForDots[i].Color);
    }

    // Tag items: show checked when the selection already has that tag (click will remove)
    if (m_OnAssignTag || m_OnOpenSettingsToTags)
    {
        const std::vector<std::filesystem::path> paths = m_PathsForBookmarks.empty()
            ? std::vector<std::filesystem::path>{assetPath}
            : m_PathsForBookmarks;
        const std::vector<EditorTagDefinition> tags = EditorTags::Load();
        m_Menu->SetStateProvider([this, paths, tags](uint32_t cmd) -> MenuItemState {
            MenuItemState state{.Enabled = true, .Checked = false};
            if (cmd >= kCmdAssetAssignTagBase && cmd < kCmdAssetAssignTagBase + kCmdAssetAssignTagMaxCount)
            {
                const size_t idx = static_cast<size_t>(cmd - kCmdAssetAssignTagBase);
                if (idx < tags.size() && !paths.empty())
                {
                    auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                    const std::string& tagName = tags[idx].Name;
                    bool allHave = true;
                    for (const auto& p : paths)
                    {
                        std::string meta;
                        reg.TryGetMetaValue(p, "tags", meta);
                        std::vector<std::string> parts;
                        for (size_t i = 0; i < meta.size(); )
                        {
                            size_t j = meta.find(',', i);
                            if (j == std::string::npos) j = meta.size();
                            std::string part = meta.substr(i, j - i);
                            const size_t s = part.find_first_not_of(" \t");
                            if (s != std::string::npos) {
                                size_t e = part.find_last_not_of(" \t");
                                part = part.substr(s, e == std::string::npos ? part.size() - s : e - s + 1);
                            } else part.clear();
                            if (!part.empty()) parts.push_back(part);
                            i = j + (j < meta.size() ? 1 : 0);
                        }
                        if (std::find(parts.begin(), parts.end(), tagName) == parts.end())
                            { allHave = false; break; }
                    }
                    state.Checked = allHave;
                }
            }
            return state;
        });
    }
    else
    {
        m_Menu->SetStateProvider(nullptr);
    }

    m_Menu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
}

void EditorContextMenu::ShowDirectoryMenu(const std::filesystem::path& directoryPath,
                                          float x, float y,
                                          const std::function<void(const std::filesystem::path&, DirectoryAction)>& onDirectoryAction,
                                          bool includeDelete)
{
    if (!m_Window)
    {
        return;
    }

    if (directoryPath.empty())
    {
        return;
    }

    EnsureMenu();
    if (!m_Menu)
    {
        return;
    }

    m_AssetPath = directoryPath;
    m_IsDirectory = true;
    m_OnOpenAction = nullptr;
    m_OnDelete = nullptr;
    m_OnDirectoryAction = onDirectoryAction;

    m_Menu->Clear();

    ContextMenuBuilder builder;
    const std::string showLabel = Editor::ShowInFileManagerLabel();
    // ContextMenuBuilder sorts globally by (priority, path). Create stays at -1000
    // so it leads the root menu; each Create child gets its own step so separators
    // don't collapse (three lines under Smart Folder) and C#/C++ don't sort last.
    builder.AddItem("Create", 0, MenuItemFlag_None, -1000, EditorIcons::kPlus)
        .AddItem("Create/Import...", kCmdDirectoryImport, MenuItemFlag_None, -1100, EditorIcons::kFolderOpen)
        .AddItem("Create/---0", 0, MenuItemFlag_None, -1099)
        .AddItem("Create/New Folder", kCmdDirectoryNewFolder, MenuItemFlag_None, -1098, EditorIcons::kPlus)
        .AddItem("Create/---1", 0, MenuItemFlag_None, -1097)
        .AddItem("Create/Smart Folder", kCmdDirectoryNewSmartFolder, MenuItemFlag_None, -1096, EditorIcons::kFolder)
        .AddItem("Create/---2", 0, MenuItemFlag_None, -1095)
        .AddItem("Create/Scene", kCmdDirectoryCreateScene, MenuItemFlag_None, -1094, EditorIcons::kScene)
        .AddItem("Create/---3", 0, MenuItemFlag_None, -1093)
        .AddItem("Create/C#", 0, MenuItemFlag_None, -1092, EditorIcons::kScript)
        .AddItem("Create/C#/Script", kCmdDirectoryCreateCSharpScript, MenuItemFlag_None, -1091, EditorIcons::kScript)
        .AddItem("Create/C++", 0, MenuItemFlag_None, -1090, EditorIcons::kScript)
        .AddItem("Create/---4", 0, MenuItemFlag_None, -1089)
        .AddItem("Create/Material", kCmdDirectoryCreateMaterial, MenuItemFlag_None, -1088, EditorIcons::kMaterial)
        .AddItem("Create/Shader Graph", kCmdDirectoryCreateShaderGraph, MenuItemFlag_None, -1087, EditorIcons::kNode)
        .AddItem("Create/Surface Shader", kCmdDirectoryCreateSurfaceShader, MenuItemFlag_None, -1086, EditorIcons::kScript)
        .AddItem("Create/---5", 0, MenuItemFlag_None, -1085)
        .AddItem("Create/Animation", 0, MenuItemFlag_None, -1084, EditorIcons::kFilm)
        .AddItem("Create/Animation/Library", kCmdDirectoryCreateAnimationLibrary, MenuItemFlag_None, -1083, EditorIcons::kFilm)
        .AddItem("Create/Animation/Controller", kCmdDirectoryCreateAnimationController, MenuItemFlag_None, -1082, EditorIcons::kFilm)
        .AddItem("Create/Animation/Timeline", kCmdDirectoryCreateTimeline, MenuItemFlag_None, -1081, EditorIcons::kFilm)
        .AddItem("Create/Animation/Clip Set", kCmdDirectoryCreateClipSet, MenuItemFlag_None, -1080, EditorIcons::kFilm)
        .AddItem("Create/Animation/Sprite Frames", kCmdDirectoryCreateSpriteFrames, MenuItemFlag_None, -1079, EditorIcons::kFilm)
        .AddItem("Create/---6", 0, MenuItemFlag_None, -1078)
        .AddItem("Create/Navigation Grid", kCmdDirectoryCreateNavGrid, MenuItemFlag_None, -1077, EditorIcons::kNavGrid)
        .AddItem("Create/Navigation Mesh", kCmdDirectoryCreateNavMesh, MenuItemFlag_None, -1076, EditorIcons::kNavGrid);

    // The six "Create code asset" rows are table-driven (sorted into place by their priorities).
    {
        std::size_t codeAssetCount = 0;
        const auto* codeAssets = Editor::GetCodeAssetDescriptors(codeAssetCount);
        for (std::size_t i = 0; i < codeAssetCount; ++i)
            builder.AddItem(codeAssets[i].MenuPath, codeAssets[i].CommandId,
                            MenuItemFlag_None, codeAssets[i].Priority, EditorIcons::kScript);
    }

    builder
        .AddItem(showLabel, kCmdAssetShowInFileManager, MenuItemFlag_None, 0, EditorIcons::kEye)
        .AddItem("Copy Full Path", kCmdAssetCopyPath, MenuItemFlag_None, 0, EditorIcons::kCopy)
        .AddItem("Refresh", kCmdDirectoryRefresh, MenuItemFlag_None, 0, EditorIcons::kReset)
        .AddItem("Asset Database", 0, MenuItemFlag_None, 50, EditorIcons::kInfo)
        .AddItem("Asset Database/Save Asset Database Now", kCmdAssetDbSaveNow, MenuItemFlag_None, 50, EditorIcons::kSave)
        .AddItem("Asset Database/Report Conflicts", kCmdAssetDbReportConflicts, MenuItemFlag_None, 50, EditorIcons::kInfo)
        .AddItem("Asset Database/Report Missing Assets", kCmdAssetDbReportMissing, MenuItemFlag_None, 50, EditorIcons::kInfo)
        .AddItem("Asset Database/Fix Up Redirects", kCmdAssetDbFixUpRedirects, MenuItemFlag_None, 50, EditorIcons::kReset);

    // Add VCS menu items if in a repository
    Editor::EditorVcsProviderDescriptor activeProvider;
    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(activeProvider) &&
        activeProvider.Integration().IsRepository())
    {
        const std::string& vcsName = activeProvider.DisplayName;

        builder.AddItem(vcsName, 0, MenuItemFlag_None, 0, EditorIcons::kVcs);
        builder.AddItem(vcsName + "/Commit...", kCmdVcsCommit, MenuItemFlag_None, 0, EditorIcons::kVcs);

        if (!activeProvider.UpdateActionLabel.empty())
        {
            builder.AddItem(vcsName + "/" + activeProvider.UpdateActionLabel, kCmdVcsUpdate,
                            MenuItemFlag_None, 0, EditorIcons::kVcs);
        }

        AppendProviderMenuItems(builder, activeProvider, /*isDirectory=*/true);
    }

    // Items other modules register for folders.
    for (const Editor::EditorDirectoryMenuItemSnapshot& item : Editor::EditorMenuRegistry::Get().DirectoryMenuSnapshot())
        builder.AddItem(item.Path, item.CommandId, MenuItemFlag_None, item.Priority, item.Icon);

    // Append script items targeting Assets empty-space context menu
    {
        const uint32_t mask = static_cast<uint32_t>(Editor::EditorContextMenuTarget::AssetsEmpty);
        auto scriptItems = Editor::ScriptMenuRegistry::Get().GetContextItems(mask);
        for (const auto& si : scriptItems)
        {
            if (si.commandId == 0 || si.path.empty())
                continue;
            builder.AddItem(si.path, si.commandId, MenuItemFlag_None, si.priority, EditorIcons::kScript);
        }
    }

    if (includeDelete)
    {
        builder.AddItem("---delete", 0, MenuItemFlag_None, 1000)
            .AddItem("Delete", kCmdDirectoryDelete, MenuItemFlag_None, 1000, EditorIcons::kTrash);
    }

    builder.Build(m_Menu.get());

    m_Menu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
}

void EditorContextMenu::OpenAsset(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    // Delegate to Platform helpers so we can use proper OS APIs and
    // consistent path handling across the engine.
    Platform::OpenPath(path);
}

void EditorContextMenu::ShowInFileManager(const std::filesystem::path& path)
{
    if (path.empty())
    {
        Logger::Log::Warning("Editor: ShowInFileManager called with empty path");
        return;
    }

    std::error_code ec;
    std::filesystem::path resolvedPath = path;
    
    // Always ensure we have an absolute path
    if (!path.is_absolute())
    {
        // Try to resolve using AssetManager first (for asset-relative paths)
        try
        {
            auto& engine = EngineCore::GetInstance();
            if (engine.IsInitialized())
            {
                auto& am = engine.GetAssetManager();
                resolvedPath = am.ResolveAssetPath(path);
            }
        }
        catch (...)
        {
            // Ignore and fall through to absolute()
        }
        
        // If still relative, make absolute
        if (!resolvedPath.is_absolute())
        {
            resolvedPath = std::filesystem::absolute(path, ec);
            if (ec)
            {
                Logger::Log::Warning("Editor: Failed to resolve path '{}': {}", path.string(), ec.message());
                return;
            }
        }
    }
    
    // Normalize the path
    resolvedPath = resolvedPath.lexically_normal();
    
    // Verify the path exists
    if (!std::filesystem::exists(resolvedPath, ec))
    {
        Logger::Log::Warning("Editor: Path does not exist: {} (original: {})", 
                            resolvedPath.string(), path.string());
        return;
    }

    // Use a platform helper that handles both file and directory cases
    const bool success = Platform::ShowInFileManager(resolvedPath);
    if (!success)
    {
        Logger::Log::Warning("Editor: Platform::ShowInFileManager failed for: {}", resolvedPath.string());
    }
}

void EditorContextMenu::CopyPathToClipboard(const std::filesystem::path& path)
{
    std::string utf8 = path.string();
    if (utf8.empty())
    {
        return;
    }

    Platform::SetClipboardText(utf8.c_str());
}

void EditorContextMenu::SetOnShowCommitDialog(std::function<void(const std::string&)> callback)
{
    m_OnShowCommitDialog = std::move(callback);
}

void EditorContextMenu::SetOnShowVcsLog(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnShowVcsLog = std::move(callback);
}

void EditorContextMenu::SetOnVcsRevert(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnVcsRevert = std::move(callback);
}

void EditorContextMenu::SetOnAddTag(std::function<void(const std::filesystem::path&, const std::vector<std::filesystem::path>&)> cb)
{
    m_OnAddTag = std::move(cb);
}

void EditorContextMenu::SetOnAssignTag(std::function<void(const std::vector<std::filesystem::path>&, const std::string&)> cb)
{
    m_OnAssignTag = std::move(cb);
}

void EditorContextMenu::SetOnOpenSettingsToTags(std::function<void()> cb)
{
    m_OnOpenSettingsToTags = std::move(cb);
}

void EditorContextMenu::SetOnShowDiff(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnShowDiff = std::move(callback);
}

void EditorContextMenu::ShowSmartFolderMenu(float x, float y,
                                             const std::function<void(SmartFolderAction)>& onAction)
{
    if (!m_Window)
        return;

    EnsureMenu();
    if (!m_Menu)
        return;

    m_OnSmartFolderAction = onAction;

    m_Menu->Clear();

    ContextMenuBuilder builder;
    builder.AddItem("New Smart Folder", kCmdSmartFolderNew, MenuItemFlag_None, -100, EditorIcons::kPlus)
        .AddItem("---", 0)
        .AddItem("Delete", kCmdSmartFolderDelete, MenuItemFlag_None, 0, EditorIcons::kTrash);

    builder.Build(m_Menu.get());
    m_Menu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
}

void EditorContextMenu::ShowPhantomAssetMenu(const std::filesystem::path& displayPath,
                                              float x, float y,
                                              const std::function<void()>& onRemoveFromScene,
                                              const std::function<void()>& onCopyGuid)
{
    if (!m_Window)
        return;

    EnsureMenu();
    if (!m_Menu)
        return;

    m_OnPhantomRemoveFromScene = onRemoveFromScene;
    m_OnPhantomCopyGuid = onCopyGuid;
    // Clear path-bound state so the asset-path command guards in the
    // command handler don't accidentally swallow our phantom commands.
    m_AssetPath.clear();
    m_IsDirectory = false;
    m_PathsForBookmarks.clear();

    m_Menu->Clear();

    ContextMenuBuilder builder;
    const std::string label = std::string("Missing: ") + displayPath.filename().string();
    builder.AddItem(label, 0, MenuItemFlag_Disabled, -100, EditorIcons::kInfo)
        .AddItem("---", 0)
        .AddItem("Remove reference from scene", kCmdPhantomRemoveFromScene, MenuItemFlag_None, 0, EditorIcons::kTrash)
        .AddItem("Copy GUID", kCmdPhantomCopyGuid, MenuItemFlag_None, 0, EditorIcons::kCopy);

    builder.Build(m_Menu.get());
    m_Menu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
}

} // namespace GameEngine
