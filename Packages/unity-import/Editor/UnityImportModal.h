#pragma once

#include "UI/UIElement.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GameEngine {

class Button;
class Label;
class TextField;
class Checkbox;
class ScrollView;

// One converter output file and its category (scene|material|texture|shader|...).
struct UnityImportOutput {
    std::string Path;
    std::string Kind;
};

// Per-scene result carried out of the converter's --json summary.
struct UnityImportSceneResult {
    std::string Name;
    std::string Output;              // absolute path in the tmp stage
    std::string RemappedOutput;      // absolute path after the move into Assets
    std::vector<std::string> Dropped; // honest-drop entries ("kind: detail")
    long long Entities = 0;
    long long ResolvedMeshes = 0;
    long long UnresolvedMeshes = 0;
    long long SeededMeshes = 0;      // refs bound to FBX the converter extracted from the pack
};

// In-editor "Import Unity Package" modal.
//
// Full-screen overlay (same pattern as SaveSceneAsModal) that drives the staged
// converter assembly (UnityConverter.dll on the engine-hosted CoreCLR): pick a
// .unitypackage or extracted dir -> list scenes -> select
// a subset + destination -> convert into a tmp stage under the project cache
// (outside the FileWatcher's scope) -> overwrite-collision check -> one move
// pass into Assets/ -> register the moved files with the AssetRegistry ->
// completion summary with honest-drop report. Scene-open is gated on that
// registration finishing so the scenes' path-form references (seeded models,
// staged source textures) resolve deterministically at open. No auto-open.
class UnityImportModal : public UIElement {
public:
    UnityImportModal();
    ~UnityImportModal() override;

    // Wiring (call before Show; the modal is attached to the UI root once).
    void SetConverterDll(std::filesystem::path dll) { m_ConverterDll = std::move(dll); }
    void SetProjectContext(std::filesystem::path defaultDestination,
                           std::filesystem::path cacheRoot,
                           std::filesystem::path assetDbPath);
    void SetOnOpenScene(std::function<void(const std::filesystem::path&)> cb) { m_OnOpenScene = std::move(cb); }
    // Synchronous "register these freshly moved files with the project now"
    // hook. Invoked on the modal's worker thread after the move pass; the
    // modal keeps its "Open scene" buttons back until it returns.
    void SetOnAssetsChanged(std::function<void(const std::vector<std::filesystem::path>&)> cb) { m_OnAssetsChanged = std::move(cb); }

    void Show();
    void Hide();

private:
    enum class State { Config, Busy, Complete, Error };

    void BuildUI();
    void ShowSection(State state);

    void OnBrowseBundle();
    void OnBrowseDestination();
    void OnImport();
    void OnCancel();
    void Dismiss();

    // Worker-thread entry points (run RunConverterHosted); results marshaled
    // back to the UI thread via PostAction.
    void StartListScenes(const std::filesystem::path& bundle);
    void PopulateSceneList(const std::string& inventoryJson);
    void UpdateSelectedCount();
    void StartConversion(const std::vector<std::string>& sceneArgs);
    void ApplyProgressToUI();
    void OnConversionFinished();

    // Finalize chain after a successful conversion. The UI thread only
    // snapshots inputs and answers the overwrite prompt; every filesystem
    // phase (collision scan, move pass, stage cleanup, registration) runs on
    // the worker thread — unbounded IO on the UI thread starves the message
    // pump (Windows AppHang class).
    void CollisionCheckAndMove();
    void OnCollisionScanDone(std::filesystem::path dest,
                             std::filesystem::path stageAssets,
                             std::vector<UnityImportOutput> outputs,
                             std::vector<UnityImportSceneResult> scenes,
                             std::vector<std::filesystem::path> collisions);
    // Worker-thread entry: move pass + stage cleanup + registration.
    void FinalizeMoveAndRegister(std::filesystem::path dest,
                                 std::filesystem::path stageAssets,
                                 std::vector<UnityImportOutput> outputs,
                                 std::vector<UnityImportSceneResult> scenes);
    static std::filesystem::path RemapStagePath(const std::string& absPath,
                                                const std::filesystem::path& stageAssets,
                                                const std::filesystem::path& dest);
    void ShowComplete(size_t unmovedOutputs);
    // Builds the open-scene row: a "registering…" note while the moved files
    // are still being registered with the AssetRegistry, buttons afterwards.
    void RefreshOpenSceneRow();
    void ShowError(const std::string& message);

    void CleanupStage();
    std::filesystem::path DestinationDir() const;

    // Config / callbacks.
    std::filesystem::path m_ConverterDll;
    std::filesystem::path m_DefaultDestination;
    std::filesystem::path m_CacheRoot;
    std::filesystem::path m_AssetDbPath;
    std::function<void(const std::filesystem::path&)> m_OnOpenScene;
    std::function<void(const std::vector<std::filesystem::path>&)> m_OnAssetsChanged;

    // Live state.
    State m_State = State::Config;
    bool m_Visible = false;
    std::filesystem::path m_BundlePath;
    std::filesystem::path m_StageDir;      // tmp <cache>/UnityImport/<id>
    std::filesystem::path m_StageAssetsDir; // <stage>/assets (converter output root)

    // Worker + cross-thread progress/result (guarded by m_Mutex).
    std::thread m_Worker;
    std::atomic<bool> m_Busy{false};
    // Cooperative cancel for the hosted converter: checked at every converter
    // output line. Set only on destruction today (editor shutdown no longer
    // waits out a long conversion); there is deliberately no mid-conversion
    // cancel button — the subprocess path never had one either.
    std::atomic<bool> m_CancelRequested{false};
    std::mutex m_Mutex;
    std::string m_ProgressPhase;
    std::string m_ProgressDetail;
    long long m_ProgressStep = 0;
    long long m_ProgressTotal = -1; // -1 => unbounded item stream (textures/models)
    long long m_TextureCount = 0;
    long long m_ModelCount = 0;     // pack FBX extracted so far (models stream)
    bool m_ConversionSpawned = false;
    int m_ConversionExit = -1;
    bool m_ConversionOk = false;
    std::string m_StderrTail;
    std::vector<UnityImportOutput> m_Outputs;
    std::vector<UnityImportSceneResult> m_Scenes;
    long long m_MatGenerated = 0;
    long long m_TexEncoded = 0;
    long long m_TexCopied = 0;
    long long m_TexUnresolved = 0;
    long long m_ModelsSeeded = 0;   // unique pack FBX extracted (summary models.seeded)
    // True from the move pass until the OnAssetsChanged registration worker
    // finishes; RefreshOpenSceneRow holds the open-scene buttons back while set.
    bool m_RegistrationPending = false;

    // UI element pointers (owned by the tree).
    UIElement* m_ConfigSection = nullptr;
    UIElement* m_BusySection = nullptr;
    UIElement* m_CompleteSection = nullptr;
    UIElement* m_ErrorSection = nullptr;

    TextField* m_BundleField = nullptr;
    Label* m_SceneHeaderLabel = nullptr;
    ScrollView* m_SceneListScroll = nullptr;
    UIElement* m_SceneListContainer = nullptr; // scroll content root; rows live here
    Label* m_SharedAssetsLabel = nullptr;
    TextField* m_DestField = nullptr;
    Label* m_TexturesLabel = nullptr;
    Label* m_ConfigWarningLabel = nullptr;

    UIElement* m_ProgressFill = nullptr;
    Label* m_PhaseLabel = nullptr;
    Label* m_TextureSubLabel = nullptr;

    Label* m_SummaryLabel = nullptr;
    UIElement* m_OpenSceneRow = nullptr;
    Label* m_ErrorLabel = nullptr;

    struct SceneRow {
        Checkbox* Box = nullptr;
        std::string Name;
        std::string Path;
        std::string Guid;
    };
    std::vector<SceneRow> m_SceneRows;
};

} // namespace GameEngine
