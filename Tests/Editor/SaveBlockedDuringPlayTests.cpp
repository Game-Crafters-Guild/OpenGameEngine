// Play mode simulates the authored world IN PLACE — PlayModeManager keeps a
// same-world snapshot and restores it on exit — so while it runs, the primary
// world holds runtime state and the .scene file on disk holds authored state.
// A manual save in that window serializes the running world over the authored
// file, and exiting play then restores the snapshot, so the authored work is
// gone with nothing left naming it.
//
// Auto-save has been inhibited outside Edit state for as long as it has existed
// (EditorApplication::Update passes the inhibit into TickAutoSave). Manual save
// was not, at any entry point. These arms pin the refusal at the document
// manager, which is the serialization choke point every manual entry point funnels
// through — Ctrl+S, the toolbar, the File menu, universal search, the dirty-scene
// and quit prompts, and the MCP save_scene handler.
//
// The file-bytes assertions are what give the arms teeth: they fail if the guard
// is removed, because the refused Save would then reach Scene::SaveSceneToFile
// and rewrite the file with the mutated world.

#include <gtest/gtest.h>

#include "Scene/SceneDocumentManager.h"

#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;

std::vector<char> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Per-process scratch root: a fixed name is shared by every EditorTests process
// on the machine, and SetUp's remove_all would delete a concurrent run's scene
// out from under it. Same contract as SceneBackupRecoveryTests.
std::filesystem::path ProcessScratchRoot()
{
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return std::filesystem::temp_directory_path() / ("ge-save-during-play-" + std::to_string(pid));
}

class SaveBlockedDuringPlayTests : public ::testing::Test
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
    }

    void SetUp() override
    {
        m_Dir = ProcessScratchRoot();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
        std::filesystem::create_directories(m_Dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // Entities carry a Transform so they are certain to reach the file: a
    // component-less entity is not something the scene writer is obliged to
    // emit, and the byte comparisons below would then compare two identical
    // files and pass for the wrong reason.
    static void AddPlacedEntity(World& world, float x)
    {
        ECS::EntityHandle e = world.CreateEntity();
        Components::Transform xf{};
        xf.matrix[12] = x; // column 3 = translation
        world.AddComponentImmediate(e, xf);
    }

    // A world with one placed entity, saved to `<name>.scene`, in Edit state
    // (probe unset).
    void MakeSavedDocument(World& world, Editor::SceneDocumentManager& doc,
                           std::filesystem::path& outPath, const std::string& name)
    {
        doc.SetWorld(&world);
        AddPlacedEntity(world, 1.0f);
        world.ProcessCommands();
        outPath = m_Dir / (name + ".scene");
        ASSERT_TRUE(doc.SaveAs(outPath));
        ASSERT_TRUE(std::filesystem::exists(outPath));
    }

    // Diverge the running world from what the authored file holds.
    static void SimulateRuntimeMutation(World& world)
    {
        for (int i = 0; i < 4; ++i)
            AddPlacedEntity(world, 10.0f + static_cast<float>(i));
        world.ProcessCommands();
    }

    std::filesystem::path m_Dir;
};

// ---------------------------------------------------------------------------
// The refusal itself, on a live document that writes real files.
// ---------------------------------------------------------------------------

TEST_F(SaveBlockedDuringPlayTests, SaveRefusedWhilePlaying_AuthoredFileUntouched)
{
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "authored"));

    const std::vector<char> authored = ReadAllBytes(scenePath);
    ASSERT_FALSE(authored.empty());

    // Enter play, and let the running world diverge from the authored one.
    bool playing = true;
    doc.SetPlayModeProbe([&playing]() { return playing; });
    EXPECT_TRUE(doc.IsSaveBlockedByPlayMode());

    SimulateRuntimeMutation(world);

    EXPECT_FALSE(doc.Save());
    // Remove the guard in SceneDocumentManager::Save and this is what breaks:
    // the refused save reaches SaveSceneToFile and writes the 5-entity world.
    EXPECT_EQ(ReadAllBytes(scenePath), authored);
}

TEST_F(SaveBlockedDuringPlayTests, SaveAllowedAgainAfterPlayStops)
{
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "resumes"));

    const std::vector<char> authored = ReadAllBytes(scenePath);

    bool playing = true;
    doc.SetPlayModeProbe([&playing]() { return playing; });
    SimulateRuntimeMutation(world);
    ASSERT_FALSE(doc.Save());
    ASSERT_EQ(ReadAllBytes(scenePath), authored);

    // Stop play. The guard lifts and the same call now writes.
    playing = false;
    EXPECT_FALSE(doc.IsSaveBlockedByPlayMode());
    EXPECT_TRUE(doc.Save());
    EXPECT_NE(ReadAllBytes(scenePath), authored);
}

TEST_F(SaveBlockedDuringPlayTests, SaveAsRefusedWhilePlaying_NoFileAndDocumentKeepsItsPath)
{
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "original"));

    bool playing = true;
    doc.SetPlayModeProbe([&playing]() { return playing; });

    // Save-As is refused too. It would otherwise adopt the new path as the
    // document identity, leaving the document naming a runtime snapshot once
    // play restores the authored world.
    const std::filesystem::path snapshotPath = m_Dir / "runtime-snapshot.scene";
    EXPECT_FALSE(doc.SaveAs(snapshotPath));
    EXPECT_FALSE(std::filesystem::exists(snapshotPath));
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(*doc.GetScenePath(), scenePath);

    playing = false;
    EXPECT_TRUE(doc.SaveAs(snapshotPath));
    EXPECT_TRUE(std::filesystem::exists(snapshotPath));
}

TEST_F(SaveBlockedDuringPlayTests, EditStateProbeNeverBlocks)
{
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "editing"));

    doc.SetPlayModeProbe([]() { return false; }); // play mode present, in Edit
    EXPECT_FALSE(doc.IsSaveBlockedByPlayMode());
    EXPECT_TRUE(doc.Save());
}

TEST_F(SaveBlockedDuringPlayTests, UnsetProbeNeverBlocks)
{
    // A headless or standalone document has no play mode to ask.
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "headless"));

    EXPECT_FALSE(doc.IsSaveBlockedByPlayMode());
    EXPECT_TRUE(doc.Save());
}

// ---------------------------------------------------------------------------
// Entry-point plumbing. The refusal is enforced at the document manager above;
// these pin that each manual entry point asks BEFORE it acts, so the message
// reaches the surface the user acted on (and *outError for MCP save_scene)
// instead of surfacing as a generic "save failed".
//
// Source-level, matching SceneSwapOrderingTests: SceneEditorController's save
// entry points need AttachToRoot's UI tree and its modals, which a headless
// suite has no way to stand up.
// ---------------------------------------------------------------------------

std::filesystem::path EditorSourceRoot()
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR);
}

// Body of the member definition starting at `signature`: from its opening brace
// to the first closing brace at column 0, which is where this file (and the
// house style) ends a function definition.
std::string FunctionBody(const std::string& src, const std::string& signature)
{
    const size_t start = src.find(signature);
    if (start == std::string::npos)
        return {};
    const size_t open = src.find('{', start);
    if (open == std::string::npos)
        return {};
    const size_t close = src.find("\n}", open);
    if (close == std::string::npos)
        return {};
    return src.substr(open, close - open);
}

TEST(SaveBlockedDuringPlayWiring, ControllerSaveEntryPointsAskBeforeTheyAct)
{
    const std::string src =
        ReadFile(EditorSourceRoot() / "Source" / "Scene" / "SceneEditorController.cpp");
    ASSERT_FALSE(src.empty()) << "SceneEditorController.cpp not found under " << GE_EDITOR_SOURCE_DIR;

    // Instrument check first: an over-reaching extractor would return the rest of
    // the file and make every assertion below pass on someone else's guard.
    // RequestRevertScene is a neighbouring definition that must NOT contain the token: it asks
    // the document directly (IsSaveBlockedByPlayMode) rather than through the refusal helper.
    const std::string control = FunctionBody(src, "void SceneEditorController::RequestRevertScene(");
    ASSERT_FALSE(control.empty()) << "extractor found no RequestRevertScene body";
    ASSERT_EQ(control.find("RefuseSaveDuringPlay"), std::string::npos)
        << "FunctionBody over-reached past the definition it was asked for";

    struct Entry
    {
        const char* signature;
        const char* what;
    };
    const Entry entries[] = {
        {"void SceneEditorController::DoSave(", "Ctrl+S / toolbar / File menu / universal search"},
        {"void SceneEditorController::DoSaveAsFlow(", "Save As dialog"},
        {"bool SceneEditorController::SaveActiveScene(", "MCP save_scene (no path)"},
        {"bool SceneEditorController::SaveActiveSceneAs(", "MCP save_scene (with path)"},
    };

    for (const Entry& e : entries)
    {
        const std::string body = FunctionBody(src, e.signature);
        ASSERT_FALSE(body.empty()) << "no definition found for " << e.signature;
        EXPECT_NE(body.find("RefuseSaveDuringPlay"), std::string::npos)
            << e.signature << " (" << e.what << ") does not refuse during play mode";
    }
}

TEST(SaveBlockedDuringPlayWiring, EditorApplicationSuppliesThePlayStateProbe)
{
    // Without this the probe is never set, every IsSaveBlockedByPlayMode() is
    // false, and all of the arms above pass while the editor still overwrites.
    const std::string src = ReadFile(EditorSourceRoot() / "Source" / "EditorApplication.cpp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("m_SceneEditor->SetPlayModeProbe("), std::string::npos)
        << "EditorApplication never wires the scene editor's play-state probe";
}

TEST(SaveBlockedDuringPlayWiring, BothWriteInhibitsReadOnePredicate)
{
    // The manual-save probe and the autosave tick must not carry two textual copies of
    // "is play mode running" — that is how one gains a state the other does not.
    const std::string src = ReadFile(EditorSourceRoot() / "Source" / "EditorApplication.cpp");
    ASSERT_FALSE(src.empty());

    const std::string probeBody =
        FunctionBody(src, "bool EditorApplication::IsPlayModeBlockingSceneWrites(");
    ASSERT_FALSE(probeBody.empty()) << "no single write-inhibit predicate exists";

    // Exactly one place compares play state against Edit for write purposes: the predicate
    // itself. The two consumers name the symbol instead of restating the comparison.
    EXPECT_NE(src.find("SetPlayModeProbe([this]() { return IsPlayModeBlockingSceneWrites(); })"),
              std::string::npos)
        << "the manual-save probe does not read the shared predicate";
    EXPECT_NE(src.find("TickAutoSave(static_cast<float>(deltaTime), IsPlayModeBlockingSceneWrites())"),
              std::string::npos)
        << "the autosave inhibit does not read the shared predicate";
}

// ---------------------------------------------------------------------------
// The two invariants of composing this refusal with main's degraded-save guard.
// ---------------------------------------------------------------------------

TEST_F(SaveBlockedDuringPlayTests, SaveAnywayDoesNotDefeatThePlayModeRefusal)
{
    // DegradedSavePolicy::SaveAnyway is the user confirming a DEGRADED overwrite. It must never
    // become a way to write a simulated world over the authored one: that write has no
    // confirmed form, so play mode is asked first and ignores the policy entirely.
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "absolute"));

    const std::vector<char> authored = ReadAllBytes(scenePath);
    doc.SetPlayModeProbe([]() { return true; });
    SimulateRuntimeMutation(world);

    EXPECT_FALSE(doc.Save(Editor::DegradedSavePolicy::SaveAnyway));
    EXPECT_EQ(ReadAllBytes(scenePath), authored);

    const std::filesystem::path other = m_Dir / "elsewhere.scene";
    EXPECT_FALSE(doc.SaveAs(other, Editor::DegradedSavePolicy::SaveAnyway));
    EXPECT_FALSE(std::filesystem::exists(other));
}

TEST_F(SaveBlockedDuringPlayTests, PlayModeReasonIsReportedAheadOfTheDegradedReason)
{
    // One refusal query, and play mode wins it. A caller that surfaced the degraded reason
    // during play would tell the user to "Save As to another file" — advice that is refused too.
    World world;
    Editor::SceneDocumentManager doc;
    std::filesystem::path scenePath;
    ASSERT_NO_FATAL_FAILURE(MakeSavedDocument(world, doc, scenePath, "precedence"));

    EXPECT_TRUE(doc.DescribeSaveRefusal(scenePath, Editor::DegradedSavePolicy::Refuse).empty())
        << "a clean document in Edit mode must not be refused";

    doc.SetPlayModeProbe([]() { return true; });
    EXPECT_EQ(doc.DescribeSaveRefusal(scenePath, Editor::DegradedSavePolicy::Refuse),
              std::string(Editor::kSaveBlockedDuringPlayMessage));
    EXPECT_EQ(doc.DescribeSaveRefusal(scenePath, Editor::DegradedSavePolicy::SaveAnyway),
              std::string(Editor::kSaveBlockedDuringPlayMessage));
}

TEST(SaveBlockedDuringPlayWiring, DoSaveRefusesBeforeTheDegradedPromptCanOpen)
{
    // Ordering, not presence: during play the write the degraded prompt exists to confirm is
    // refused whichever button the user picks, so the prompt must never open.
    const std::string src =
        ReadFile(EditorSourceRoot() / "Source" / "Scene" / "SceneEditorController.cpp");
    ASSERT_FALSE(src.empty());

    const std::string body = FunctionBody(src, "void SceneEditorController::DoSave(");
    ASSERT_FALSE(body.empty());

    const size_t refuse = body.find("RefuseSaveDuringPlay");
    const size_t prompt = body.find("PromptDegradedThenSave");
    ASSERT_NE(refuse, std::string::npos) << "DoSave does not refuse during play";
    ASSERT_NE(prompt, std::string::npos) << "DoSave no longer reaches the degraded prompt";
    EXPECT_LT(refuse, prompt) << "the degraded prompt can open during play mode";
}

} // namespace
