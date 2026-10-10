#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneDocumentManager.h"
#include "Scene/SceneEditorController.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// A scene open can fail on either side of World::Clear, and the two failures
// leave the editor in states that must not look alike. A pre-clear failure is
// harmless — the open document is untouched. A failure after the clear has
// already destroyed the open scene and half-filled the world with a scene that
// did not load, and until it is recorded NOTHING says so: the title keeps
// naming the old scene, get_editor_state keeps reporting an entity count that
// looks fine, and Save writes the wreckage over the file it is still named for.
//
// These tests pin the loud half of that: what the document reports after each
// kind of failure. What the editor should DO about a half-loaded world (leave
// it, empty it, reload the outgoing scene) is a separate, open question — no
// test here asserts a recovery, because none is implemented.

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
namespace fs = std::filesystem;

// Parses and validates, then fails inside LoadParsedIntoWorld — after the clear — because the
// blueprint it instantiates cannot be read. ValidateDocument only checks that the [resource] id
// exists, so nothing catches this until the world is already gone.
//
// This used to be an unknown component property. It cannot be any more: a component or field this
// build cannot apply is now collected and skipped rather than aborting the load (that is what the
// degraded-document tests below cover), so reaching the post-clear failure path takes a failure the
// loader genuinely cannot continue past. "good_one" still precedes it, so it lands in the world.
constexpr const char* kFailsAfterClear =
    "[scene name=\"PartialLoad\" version=1]\n"
    "[resource id=\"bp\" path=\"no_such_blueprint.blueprint\"]\n"
    "\n"
    "[entity id=\"good_one\"]\n"
    "Transform.position = (1, 2, 3)\n"
    "\n"
    "[blueprint id=\"bad_one\" source=\"bp\"]\n";

// Loads, but one assignment cannot be applied: SplineFence.SpanGrade is a reflected enum whose
// enumerator names this build knows (Racked/Stepped/Sheared), and "Cantilevered" is what a scene
// written by a NEWER build looks like. The document is real and keeps its identity — it is simply
// missing that one value, and saving it back is what would destroy the user's data.
constexpr const char* kLoadsDegraded =
    "[scene name=\"Degraded\" version=1]\n"
    "\n"
    "[entity id=\"fence\"]\n"
    "Transform.position = (1, 2, 3)\n"
    "SplineFence.SpanGrade = Cantilevered\n"
    "\n"
    "[entity id=\"bystander\"]\n"
    "Transform.position = (9, 9, 9)\n";

// Duplicate ids fail ValidateDocument, which runs before the clear.
constexpr const char* kFailsBeforeClear =
    "[scene name=\"Dupes\" version=1]\n"
    "\n"
    "[entity id=\"same\"]\n"
    "Transform.position = (0, 0, 0)\n"
    "\n"
    "[entity id=\"same\"]\n"
    "Transform.position = (1, 1, 1)\n";

// Loads without complaint, and brings an entity with it — an additive load of
// this into a wrecked world grows the wreck.
constexpr const char* kLoadsCleanly =
    "[scene name=\"Fine\" version=1]\n"
    "\n"
    "[entity id=\"fine_one\"]\n"
    "Transform.position = (4, 5, 6)\n";

// A scratch root that belongs to THIS PROCESS. A fixed name is shared by every
// EditorTests process on the machine, and SetUp's remove_all then deletes a
// concurrent run's scene out from under it: the loser fails once and passes on
// every retry. Mirrors SceneBackupRecoveryTests, where that race was caught.
//
// The process id still repeats — it is recycled, and a crashed run leaves a
// directory no TearDown removed — so SetUp keeps clearing the root before use.
fs::path ProcessScratchRoot()
{
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return fs::temp_directory_path() / ("GameEngine_SceneLoadFailureTests_" + std::to_string(pid));
}

// Recent-scene bookkeeping writes Preferences.json on a successful open; keep it
// out of the developer's real profile. Mirrors OpenSceneResolutionTests.
class ScopedUserDataRedirect
{
  public:
    explicit ScopedUserDataRedirect(const fs::path& dir)
    {
#if defined(_WIN32)
        Capture("APPDATA");
        Capture("LOCALAPPDATA");
        _putenv_s("APPDATA", dir.string().c_str());
        _putenv_s("LOCALAPPDATA", dir.string().c_str());
#elif defined(__APPLE__)
        Capture("HOME");
        setenv("HOME", dir.string().c_str(), 1);
#else
        Capture("XDG_DATA_HOME");
        setenv("XDG_DATA_HOME", dir.string().c_str(), 1);
#endif
    }

    ~ScopedUserDataRedirect()
    {
        for (const auto& [name, value] : m_Saved)
        {
#if defined(_WIN32)
            _putenv_s(name.c_str(), value.c_str());
#else
            if (value.empty())
                unsetenv(name.c_str());
            else
                setenv(name.c_str(), value.c_str(), 1);
#endif
        }
    }

  private:
    void Capture(const char* name)
    {
        const char* v = std::getenv(name);
        m_Saved.emplace_back(name, v ? v : "");
    }

    std::vector<std::pair<std::string, std::string>> m_Saved;
};

class SceneLoadFailureTests : public ::testing::Test
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
        fs::remove_all(m_Dir, ec);
        fs::create_directories(m_Dir, ec);
        ASSERT_TRUE(fs::is_directory(m_Dir));
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    fs::path WriteScene(const char* name, const char* text) const
    {
        const fs::path p = m_Dir / name;
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << text;
        f.close();
        EXPECT_TRUE(fs::exists(p));
        return p;
    }

    // A real, loadable scene minted through the save path.
    fs::path MintGoodScene(const char* name) const
    {
        const fs::path p = m_Dir / name;
        World world;
        Editor::SceneDocumentManager doc;
        doc.SetWorld(&world);
        EXPECT_TRUE(doc.SaveAs(p));
        return p;
    }

    static std::vector<char> ReadAllBytes(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    fs::path m_Dir;
};

// The core of it: after a failure that cleared the world, the document must not
// go on naming a scene. An empty path is not a cosmetic detail — it is what
// stops the title, the debug server and Save from all agreeing on a scene that
// is not loaded.
TEST_F(SceneLoadFailureTests, FailureAfterClearLeavesNoScenePathAndRecordsWhy)
{
    const fs::path good = MintGoodScene("outgoing.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(good));
    ASSERT_TRUE(doc.GetScenePath().has_value());
    ASSERT_FALSE(doc.GetLastLoadFailure().has_value());

    EXPECT_FALSE(doc.OpenSceneReplace(bad));

    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    const auto& failure = *doc.GetLastLoadFailure();
    EXPECT_TRUE(failure.WorldCleared);
    EXPECT_EQ(failure.DocumentPath, bad);
    EXPECT_NE(failure.Message.find("Blueprint file not found"), std::string::npos) << failure.Message;
    EXPECT_GT(failure.EntitiesInWorld, 0u) << "the half-loaded entities are the whole problem";

    EXPECT_FALSE(doc.GetScenePath().has_value())
        << "the outgoing scene is gone from the world; the document must stop naming it";
}

// The consequence that costs data. With the stale path still in place, one
// Ctrl+S writes a half-loaded world over a scene the user never meant to touch.
// ---------------------------------------------------------------------------
// Degraded documents: the scene loaded, but not all of it.
//
// The failure tests above are about a world that is NOT the scene. These are
// about one that IS — which is exactly why they are dangerous. Nothing looks
// wrong: the title is right, the entity count is right, the document has its
// path. The damage happens later, when a reflex Ctrl+S writes the file back
// with the unreadable data replaced by whatever the fields defaulted to.
// ---------------------------------------------------------------------------

// The document survives intact. Losing part of a scene must not cost its identity the way a real
// failure does — the user has to be able to keep working.
TEST_F(SceneLoadFailureTests, DegradedLoadKeepsTheDocumentAndRecordsWhatWasSkipped)
{
    const fs::path scene = WriteScene("degraded.scene", kLoadsDegraded);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);

    ASSERT_TRUE(doc.OpenSceneReplace(scene)) << "a scene with one unreadable value must still open";

    EXPECT_TRUE(doc.HasScenePath()) << "a degraded scene is still a document — keep its identity";
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(doc.GetScenePath()->lexically_normal(), scene.lexically_normal());
    EXPECT_FALSE(doc.GetLastLoadFailure().has_value()) << "this is not a failure; it loaded";

    const auto& degraded = doc.GetLastLoadDegraded();
    ASSERT_TRUE(degraded.has_value()) << "and it must not be silent about what it dropped";
    EXPECT_EQ(degraded->DocumentPath.lexically_normal(), scene.lexically_normal());
    EXPECT_EQ(degraded->SkippedCount(), 1u);
    ASSERT_EQ(degraded->Census.skips.size(), 1u);
    EXPECT_EQ(degraded->Census.skips[0].component, "SplineFence");
    EXPECT_EQ(degraded->Census.skips[0].field, "SpanGrade");

    // Components skip; entities do not. Both entities in the file are in the world.
    EXPECT_GE(world.GetEntityCount(), 2u);

    // And the one thing the user cannot miss says so, because a scene quietly missing part of
    // itself is otherwise indistinguishable from a healthy one.
    const auto shown = doc.GetDisplaySceneName();
    ASSERT_TRUE(shown.has_value());
    EXPECT_NE(shown->find("(PARTIAL LOAD)"), std::string::npos) << *shown;
}

// The load-bearing one. A degraded document must not write itself over its own source on a plain
// Save — that file is the only copy of the data this build cannot read.
TEST_F(SceneLoadFailureTests, DegradedDocumentRefusesToSaveOverItsOwnSource)
{
    const fs::path scene = WriteScene("degraded_guard.scene", kLoadsDegraded);
    const std::vector<char> before = ReadAllBytes(scene);
    ASSERT_FALSE(before.empty());

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    EXPECT_FALSE(doc.Save()) << "the guard, not the untitled check: this document HAS a path";
    EXPECT_EQ(ReadAllBytes(scene), before) << "the source file must be untouched, byte for byte";
}

// Consent is what lifts the guard, and once the file has been rewritten it agrees with the world
// again, so the warning must stop.
TEST_F(SceneLoadFailureTests, DegradedDocumentSavesWhenTheUserSaysSoAndStopsWarning)
{
    const fs::path scene = WriteScene("degraded_anyway.scene", kLoadsDegraded);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    EXPECT_TRUE(doc.Save(Editor::DegradedSavePolicy::SaveAnyway));
    EXPECT_FALSE(doc.GetLastLoadDegraded().has_value())
        << "after an accepted overwrite the file and the world agree; nothing left to guard";
    EXPECT_TRUE(doc.Save()) << "and a later plain Save is no longer refused";
}

// Discarding is the OTHER way the disagreement gets resolved, and the guard has to see it. The
// census is the load's record and cannot change; asking it alone leaves the document reporting an
// override that is gone and refusing a save that would now lose nothing.
//
// Light, not kLoadsDegraded, for the same reason the accepted-save test below gives: SplineFence
// cannot be constructed in this binary, so its text is a DROP here and drops never retire.
TEST_F(SceneLoadFailureTests, DiscardingEveryOverrideLiftsTheGuardAndEmptiesTheOutstandingCensus)
{
    const fs::path scene = WriteScene("degraded_discarded.scene",
                                      "[scene name=\"Discarded\" version=1]\n"
                                      "\n"
                                      "[entity id=\"lamp\"]\n"
                                      "Light.enabled = true\n"
                                      "Light.type = NotANumber\n"
                                      "Light.intensity = 7\n");

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    const auto skips = doc.GetLastLoadDegraded()->Census.skips;
    ASSERT_EQ(skips.size(), 1u);
    ASSERT_TRUE(skips[0].preserved)
        << "precondition: only a PRESERVED skip can retire, and this one must be preserved here";
    ASSERT_EQ(doc.OutstandingDegradedSkipCount(), 1u);
    ASSERT_FALSE(doc.Save()) << "precondition: the guard refuses while the override stands";

    // The TypeId comes from the store's own entry rather than a second registry lookup: the entry
    // is what the discard has to find, so this asserts the census and the store agree on the row.
    const auto* fields = world.GetUnresolvedComponents().FieldsFor(skips[0].entityHandle);
    ASSERT_NE(fields, nullptr) << "the handle the census recorded must be the one the store keys by";
    ASSERT_EQ(fields->size(), 1u);
    ASSERT_TRUE(world.GetUnresolvedComponents().DiscardField(
        skips[0].entityHandle, fields->front().TypeId, skips[0].field));

    EXPECT_EQ(doc.OutstandingDegradedSkipCount(), 0u);
    EXPECT_FALSE(doc.GetOutstandingLoadDegraded().has_value())
        << "the outstanding VIEW is empty — nothing is left for a save to answer for";
    EXPECT_TRUE(doc.GetLastLoadDegraded().has_value())
        << "while the load-time record is a record and stays put";
    EXPECT_TRUE(doc.Save())
        << "and the guard lifts: the user adjudicated the only thing it was protecting";
}

// The two facts a reader gets must never contradict each other. The window title says PARTIAL LOAD
// for as long as the load-time record stands, so anything that reports degradation has to stay
// present on that same condition — reporting only the outstanding view had an agent seeing a healthy
// scene while a human saw a partial one, which is how two readers of one editor end up in an
// argument neither can win.
TEST_F(SceneLoadFailureTests, TheDegradedFactsAndTheTitleNeverDisagree)
{
    const fs::path scene = WriteScene("degraded_reconcile.scene",
                                      "[scene name=\"Reconcile\" version=1]\n"
                                      "\n"
                                      "[entity id=\"lamp\"]\n"
                                      "Light.enabled = true\n"
                                      "Light.type = NotANumber\n"
                                      "Light.intensity = 7\n");

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));

    const auto titleSaysPartial = [&doc]()
    {
        const auto shown = doc.GetDisplaySceneName();
        return shown.has_value() && shown->find("(PARTIAL LOAD)") != std::string::npos;
    };

    ASSERT_TRUE(titleSaysPartial()) << "precondition: the human-facing surface marks the scene";
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());
    ASSERT_EQ(doc.OutstandingDegradedSkipCount(), 1u);

    const auto skips = doc.GetLastLoadDegraded()->Census.skips;
    ASSERT_EQ(skips.size(), 1u);
    const auto* fields = world.GetUnresolvedComponents().FieldsFor(skips[0].entityHandle);
    ASSERT_NE(fields, nullptr);
    ASSERT_TRUE(world.GetUnresolvedComponents().DiscardField(
        skips[0].entityHandle, fields->front().TypeId, skips[0].field));

    // The reconciliation: outstanding drops to zero, and the record the title reads is STILL there,
    // so a reporting surface keyed on it stays present and can say "0 of 1 outstanding" instead of
    // going silent and calling the scene healthy.
    EXPECT_EQ(doc.OutstandingDegradedSkipCount(), 0u);
    EXPECT_TRUE(titleSaysPartial()) << "the title is unchanged — the load really was partial";
    EXPECT_TRUE(doc.GetLastLoadDegraded().has_value())
        << "so the fact the title is derived from must remain readable, or the two surfaces "
           "contradict each other";
    EXPECT_EQ(doc.GetLastLoadDegraded()->SkippedCount(), 1u)
        << "and the load-time count stays 1 — it is what that load did, not what is left";
}

// The guard must never be dropped by a question it cannot answer. With no world there is no store to
// ask what retired, and returning "nothing outstanding" would silently lift a guard protecting the
// user's file. Unreachable in the shipped editor — a degraded record only exists after a load, which
// needs a world — but it was a real defect on the way in, so it gets a pin.
TEST_F(SceneLoadFailureTests, WithNoWorldBothOutstandingPathsFallBackToTheLoadTimeAnswer)
{
    const fs::path scene = WriteScene("degraded_noworld.scene",
                                      "[scene name=\"NoWorld\" version=1]\n"
                                      "\n"
                                      "[entity id=\"lamp\"]\n"
                                      "Light.enabled = true\n"
                                      "Light.type = NotANumber\n"
                                      "Light.intensity = 7\n");

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_EQ(doc.OutstandingDegradedSkipCount(), 1u);

    doc.SetWorld(nullptr);

    EXPECT_EQ(doc.OutstandingDegradedSkipCount(), 1u)
        << "no world means no way to know what retired; keep the load's answer, never zero";
    const auto fallback = doc.GetOutstandingLoadDegraded();
    ASSERT_TRUE(fallback.has_value())
        << "and the reported view must not go empty either — that would call a degraded scene clean";
    EXPECT_EQ(fallback->SkippedCount(), 1u);
    EXPECT_FALSE(doc.Save()) << "the guard still holds";
}

// Save As to ANOTHER path is the escape hatch: the source is not the target, so nothing is at risk.
TEST_F(SceneLoadFailureTests, DegradedDocumentCanAlwaysSaveAsToADifferentFile)
{
    const fs::path scene = WriteScene("degraded_saveas.scene", kLoadsDegraded);
    const std::vector<char> before = ReadAllBytes(scene);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));

    const fs::path copy = m_Dir / "degraded_copy.scene";
    EXPECT_TRUE(doc.SaveAs(copy)) << "saving elsewhere risks nothing and must not be blocked";
    EXPECT_EQ(ReadAllBytes(scene), before) << "the original is not the target and must not change";
    EXPECT_TRUE(fs::exists(copy));
    EXPECT_FALSE(doc.GetLastLoadDegraded().has_value());
}

// ...but Save As aimed at the document's OWN path is a Save wearing a different name. The debug
// server can ask for exactly that, so the guard has to recognise it.
TEST_F(SceneLoadFailureTests, DegradedDocumentRefusesSaveAsOntoItsOwnPath)
{
    const fs::path scene = WriteScene("degraded_saveas_self.scene", kLoadsDegraded);
    const std::vector<char> before = ReadAllBytes(scene);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));

    EXPECT_FALSE(doc.SaveAs(scene)) << "same path = overwriting the source, whatever it is called";
    EXPECT_EQ(ReadAllBytes(scene), before);
}

// Autosave is unattended, so it cannot ask. It must not leave a stripped backup behind for crash
// recovery to restore as though it were the user's work.
TEST_F(SceneLoadFailureTests, AutosaveDoesNotBackUpADocumentThatWouldLoseData)
{
    // A component type this build does not know is preserved verbatim; a Transform property it does
    // not know has no line to survive in, so it is a genuine DROP.
    const fs::path scene = WriteScene("degraded_drop.scene",
                                      "[scene name=\"Dropper\" version=1]\n"
                                      "\n"
                                      "[entity id=\"a\"]\n"
                                      "Transform.position = (1, 2, 3)\n"
                                      "Transform.noSuchProperty = 7\n");

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));

    const auto& degraded = doc.GetLastLoadDegraded();
    ASSERT_TRUE(degraded.has_value());
    ASSERT_GT(degraded->DroppedCount(), 0u) << "this corpus must model real loss, not preservation";

    doc.MarkDirty();
    EXPECT_FALSE(doc.SaveBackupCopy()) << "an unattended backup must not write a stripped scene";
    EXPECT_FALSE(fs::exists(m_Dir / "degraded_drop.backup.scene"));
}

// A missing asset is the commonest partial load, and it opens without a degraded record: the
// reference loads, holding only its GUID. A plain Save must still write the path the scene authored,
// or the one thing that would heal the reference once the file returns is gone from the user's file.
TEST_F(SceneLoadFailureTests, SaveKeepsTheAuthoredPathOfAMissingAsset)
{
    const std::string referenceLine =
        "MeshRenderer.material = [path=\"Materials/NoSuchMaterial.material\" "
        "guid=\"5e1f0a6c-3b2d-4c8e-9f71-2a6d8b0c4e13\"]";
    const std::string text = "[scene name=\"MissingAsset\" version=1]\n"
                             "\n"
                             "[entity id=\"a\"]\n" +
                             referenceLine + "\n";
    const fs::path scene = WriteScene("missing_asset.scene", text.c_str());

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_FALSE(doc.GetLastLoadDegraded().has_value()) << "a missing asset is not a skipped value";

    ASSERT_TRUE(doc.Save());
    const std::vector<char> bytes = ReadAllBytes(scene);
    const std::string saved(bytes.begin(), bytes.end());
    EXPECT_NE(saved.find(referenceLine + "\n"), std::string::npos) << saved;
}

// The mirror of the failure record's rule, and the reason the degraded record cannot simply be
// dropped by the next successful load: additive entities join the WORLD, so they join the next
// Save. A clean additive recovers nothing.
TEST_F(SceneLoadFailureTests, CleanAdditiveLoadLeavesTheDegradedRecordStanding)
{
    const fs::path degradedScene = WriteScene("degraded_base.scene", kLoadsDegraded);
    const fs::path cleanScene = WriteScene("clean_add.scene", kLoadsCleanly);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(degradedScene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());
    const std::size_t before = doc.GetLastLoadDegraded()->SkippedCount();

    ASSERT_TRUE(doc.LoadSceneAdditive(cleanScene));

    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value())
        << "the unreadable data is still in this world; adding entities did not recover it";
    EXPECT_EQ(doc.GetLastLoadDegraded()->SkippedCount(), before);
    EXPECT_FALSE(doc.Save()) << "and Save is still guarded";
}

// A new scene is not the degraded one.
TEST_F(SceneLoadFailureTests, NewSceneClearsTheDegradedRecord)
{
    const fs::path scene = WriteScene("degraded_then_new.scene", kLoadsDegraded);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    doc.NewSceneUntitled();
    EXPECT_FALSE(doc.GetLastLoadDegraded().has_value());
}

// Opening a clean scene afterwards must not leave the previous scene's warning standing.
TEST_F(SceneLoadFailureTests, ACleanOpenClearsTheDegradedRecord)
{
    const fs::path degradedScene = WriteScene("degraded_first.scene", kLoadsDegraded);
    const fs::path cleanScene = WriteScene("clean_second.scene", kLoadsCleanly);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(degradedScene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    ASSERT_TRUE(doc.OpenSceneReplace(cleanScene));
    EXPECT_FALSE(doc.GetLastLoadDegraded().has_value());
    EXPECT_TRUE(doc.Save()) << "a clean document saves without a prompt";
}

// A load that fails after the clear destroys the document the degraded record described. Leaving
// the record standing reports two different files as the current state at once — a degradation for
// a scene that is no longer open, beside a failure for the one that replaced it.
TEST_F(SceneLoadFailureTests, PostClearFailureClearsTheDegradedRecordOfTheSceneItDestroyed)
{
    const fs::path degradedScene = WriteScene("degraded_before_fail.scene", kLoadsDegraded);
    const fs::path failingScene = WriteScene("fails_after_clear.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(degradedScene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    EXPECT_FALSE(doc.OpenSceneReplace(failingScene));

    const auto& failure = doc.GetLastLoadFailure();
    ASSERT_TRUE(failure.has_value());
    ASSERT_TRUE(failure->WorldCleared) << "this test is only meaningful if the clear ran";
    EXPECT_EQ(failure->DocumentPath.lexically_normal(), failingScene.lexically_normal());

    EXPECT_FALSE(doc.GetLastLoadDegraded().has_value())
        << "the degraded scene is gone; only the failure describes what the world now holds";
}

// The other half of the rule, and the reason the reset is not unconditional: a failure BEFORE the
// clear leaves the degraded document open and still degraded, so its record — and the save guard
// that depends on it — must survive.
TEST_F(SceneLoadFailureTests, PreClearFailureKeepsTheDegradedRecordOfTheSceneStillOpen)
{
    const fs::path degradedScene = WriteScene("degraded_kept.scene", kLoadsDegraded);
    const fs::path failingScene = WriteScene("fails_before_clear.scene", kFailsBeforeClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(degradedScene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());

    EXPECT_FALSE(doc.OpenSceneReplace(failingScene));

    const auto& failure = doc.GetLastLoadFailure();
    ASSERT_TRUE(failure.has_value());
    ASSERT_FALSE(failure->WorldCleared) << "this test needs the world left untouched";

    const auto& degraded = doc.GetLastLoadDegraded();
    ASSERT_TRUE(degraded.has_value())
        << "the degraded scene is still the open document — its warning still applies";
    EXPECT_EQ(degraded->DocumentPath.lexically_normal(), degradedScene.lexically_normal());
    EXPECT_FALSE(doc.Save()) << "and the guard it feeds must still refuse";
}

// The whole point of preserving the authored text, end to end through the editor's own save path:
// open a scene this build cannot fully read, accept the overwrite, and the value is still in the
// file. Without preservation this is where the user's data would quietly become a default.
TEST_F(SceneLoadFailureTests, AnAcceptedSaveWritesThePreservedTextBackNotTheDefault)
{
    // Deliberately NOT kLoadsDegraded: SplineFence cannot be constructed in this test binary, so
    // its authored text has no serialized line to survive in and the loader correctly reports it as
    // a DROP. Light is a hand-written schema whose component this binary does build, which is the
    // shape that can actually round-trip.
    const fs::path scene = WriteScene("degraded_preserve.scene",
                                      "[scene name=\"Preserve\" version=1]\n"
                                      "\n"
                                      "[entity id=\"lamp\"]\n"
                                      "Light.enabled = true\n"
                                      "Light.type = NotANumber\n"
                                      "Light.intensity = 7\n");

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(scene));
    ASSERT_TRUE(doc.GetLastLoadDegraded().has_value());
    std::string census;
    for (const auto& sk : doc.GetLastLoadDegraded()->Census.skips)
        census += " [" + sk.component + "." + sk.field + " preserved=" +
                  (sk.preserved ? "1" : "0") + " msg=" + sk.message + "]";
    ASSERT_EQ(doc.GetLastLoadDegraded()->DroppedCount(), 0u)
        << "a known field's text is preserved, so this save loses nothing:" << census;

    ASSERT_TRUE(doc.Save(Editor::DegradedSavePolicy::SaveAnyway));

    const std::vector<char> after = ReadAllBytes(scene);
    const std::string text(after.begin(), after.end());
    EXPECT_NE(text.find("Light.type = NotANumber"), std::string::npos)
        << "the authored value must survive the round trip:\n"
        << text;
}

TEST_F(SceneLoadFailureTests, FailureAfterClearCannotSaveOverTheOutgoingScene)
{
    const fs::path good = MintGoodScene("precious.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);
    const std::vector<char> before = ReadAllBytes(good);
    ASSERT_FALSE(before.empty());

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(good));
    ASSERT_FALSE(doc.OpenSceneReplace(bad));

    EXPECT_FALSE(doc.Save()) << "an untitled document cannot Save; that is the point";
    EXPECT_EQ(ReadAllBytes(good), before) << "the outgoing scene file must be untouched";
}

// The visible half. GetDisplaySceneName feeds the main window title, which is
// otherwise the loudest possible lie: it keeps showing a scene that is gone.
TEST_F(SceneLoadFailureTests, FailureAfterClearShowsInTheDisplayName)
{
    const fs::path good = MintGoodScene("outgoing.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(good));
    ASSERT_FALSE(doc.OpenSceneReplace(bad));

    const auto name = doc.GetDisplaySceneName();
    ASSERT_TRUE(name.has_value());
    EXPECT_NE(name->find("LOAD FAILED"), std::string::npos) << *name;
    EXPECT_NE(name->find("half.scene"), std::string::npos)
        << "name the scene that failed, not the one that is gone: " << *name;
    EXPECT_EQ(name->find("outgoing"), std::string::npos) << *name;
}

// The mirror case, and the reason the record cannot simply be "an open
// returned false": a failure raised before the clear costs the user nothing,
// and taking their document away for it would be the worse bug.
//
// The post-clear failure first is not scene-setting, it is the whole arm.
// WorldCleared is default-false, so a pre-clear failure that never ran through a
// cleared one reports false whether the flag works or not; only a stale true
// left over from the previous load can prove the false is computed.
TEST_F(SceneLoadFailureTests, FailureBeforeClearKeepsTheOpenDocument)
{
    const fs::path good = MintGoodScene("kept.scene");
    const fs::path cleared = WriteScene("half.scene", kFailsAfterClear);
    const fs::path bad = WriteScene("dupes.scene", kFailsBeforeClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);

    ASSERT_FALSE(doc.OpenSceneReplace(cleared));
    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    ASSERT_TRUE(doc.GetLastLoadFailure()->WorldCleared)
        << "arms the stale true that the EXPECT_FALSE below has to survive";

    ASSERT_TRUE(doc.OpenSceneReplace(good));

    EXPECT_FALSE(doc.OpenSceneReplace(bad));

    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    EXPECT_FALSE(doc.GetLastLoadFailure()->WorldCleared)
        << "this load never reached the clear; the previous load's flag must not carry over";
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(*doc.GetScenePath(), good);
}

// A recorded failure that outlives its world is a false alarm — worse than
// silence, because the next reader stops believing the field.
TEST_F(SceneLoadFailureTests, TheNextSuccessfulOpenClearsTheRecord)
{
    const fs::path good = MintGoodScene("recover.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_FALSE(doc.OpenSceneReplace(bad));
    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());

    ASSERT_TRUE(doc.OpenSceneReplace(good));
    EXPECT_FALSE(doc.GetLastLoadFailure().has_value());
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(*doc.GetScenePath(), good);
}

TEST_F(SceneLoadFailureTests, NewSceneClearsTheRecord)
{
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_FALSE(doc.OpenSceneReplace(bad));
    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());

    doc.NewSceneUntitled(nullptr);
    EXPECT_FALSE(doc.GetLastLoadFailure().has_value());
}

// An additive load never clears, so the open document survives — but the
// entities the failed file did create are still there, and the record is the
// only thing that says the world is not what the scene file describes.
TEST_F(SceneLoadFailureTests, AdditiveFailureRecordsWithoutTakingTheDocument)
{
    const fs::path good = MintGoodScene("host.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(good));

    EXPECT_FALSE(doc.LoadSceneAdditive(bad));

    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    EXPECT_FALSE(doc.GetLastLoadFailure()->WorldCleared);
    EXPECT_EQ(doc.GetLastLoadFailure()->DocumentPath, bad);
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(*doc.GetScenePath(), good);
}

// A load that SUCCEEDS on top of a wrecked world has recovered nothing. An
// additive open restores no outgoing scene and adopts no document identity — it
// only adds more entities to the partial load already there. The record and the
// failed name are the only things saying so, so a success that is not a document
// open must not clear either.
TEST_F(SceneLoadFailureTests, AdditiveSuccessLeavesTheClearedWorldRecordStanding)
{
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);
    const fs::path added = WriteScene("added.scene", kLoadsCleanly);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_FALSE(doc.OpenSceneReplace(bad));
    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    ASSERT_TRUE(doc.GetLastLoadFailure()->WorldCleared);

    ASSERT_TRUE(doc.LoadSceneAdditive(added));

    ASSERT_TRUE(doc.GetLastLoadFailure().has_value())
        << "the partial load is still in the world; nothing has put the outgoing scene back";
    EXPECT_TRUE(doc.GetLastLoadFailure()->WorldCleared);
    EXPECT_EQ(doc.GetLastLoadFailure()->DocumentPath, bad);
    EXPECT_FALSE(doc.GetScenePath().has_value());

    const auto name = doc.GetDisplaySceneName();
    ASSERT_TRUE(name.has_value());
    EXPECT_NE(name->find("LOAD FAILED"), std::string::npos) << *name;
    EXPECT_EQ(name->find("added.scene"), std::string::npos)
        << "an additive file is not the document, and naming the title after it while the wreck is "
           "still loaded is the impersonation this suite exists to end: "
        << *name;
}

// The control that keeps the rule above from becoming "an additive load never
// clears anything". A failure that did NOT clear left the document intact, so it
// is ordinary state that the next load supersedes.
TEST_F(SceneLoadFailureTests, AdditiveSuccessStillClearsAFailureThatKeptTheDocument)
{
    const fs::path host = MintGoodScene("host.scene");
    const fs::path bad = WriteScene("dupes.scene", kFailsBeforeClear);
    const fs::path added = WriteScene("added.scene", kLoadsCleanly);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    ASSERT_TRUE(doc.OpenSceneReplace(host));
    ASSERT_FALSE(doc.LoadSceneAdditive(bad));
    ASSERT_TRUE(doc.GetLastLoadFailure().has_value());
    ASSERT_FALSE(doc.GetLastLoadFailure()->WorldCleared);

    ASSERT_TRUE(doc.LoadSceneAdditive(added));

    EXPECT_FALSE(doc.GetLastLoadFailure().has_value());
    ASSERT_TRUE(doc.GetScenePath().has_value());
    EXPECT_EQ(*doc.GetScenePath(), host);
}

// The controller is what the debug server and the window title read; a record
// stranded on the document manager would be invisible to both.
//
// The successful open first is load-bearing. Asserting an absent path on a
// controller that never had one is a tautology: it holds whether or not the
// failure drops the identity, so it guards nothing at the layer that matters.
TEST_F(SceneLoadFailureTests, ControllerSurfacesTheFailureThroughTheOpenFunnel)
{
    const fs::path good = MintGoodScene("outgoing.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    EXPECT_FALSE(controller.GetLastSceneLoadFailure().has_value());

    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_FALSE(controller.GetLastSceneLoadFailure().has_value());
    ASSERT_TRUE(controller.GetActiveScenePath().has_value())
        << "the drop below is only meaningful if the controller had a path to drop";
    ASSERT_EQ(*controller.GetActiveScenePath(), good);

    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    ASSERT_TRUE(controller.GetLastSceneLoadFailure().has_value());
    EXPECT_TRUE(controller.GetLastSceneLoadFailure()->WorldCleared);
    EXPECT_EQ(controller.GetLastSceneLoadFailure()->DocumentPath, bad);
    EXPECT_FALSE(controller.GetActiveScenePath().has_value())
        << "a scenePath alongside a cleared-world failure is exactly the impersonation this closes";
    const auto name = controller.GetActiveSceneDisplayName();
    ASSERT_TRUE(name.has_value());
    EXPECT_NE(name->find("LOAD FAILED"), std::string::npos) << *name;
}

// Two failed opens in a row are two different failures, and only the display
// name can say which one you are looking at — the path is empty for both.
// Anything caching the title has to watch the name for that reason; watching the
// path alone leaves the window naming the FIRST failed file indefinitely.
TEST_F(SceneLoadFailureTests, ConsecutiveFailuresMoveTheDisplayNameNotThePath)
{
    const fs::path first = WriteScene("first_half.scene", kFailsAfterClear);
    const fs::path second = WriteScene("second_half.scene", kFailsAfterClear);

    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);

    ASSERT_FALSE(doc.OpenSceneReplace(first));
    const auto afterFirst = doc.GetDisplaySceneName();
    ASSERT_TRUE(afterFirst.has_value());
    EXPECT_NE(afterFirst->find("first_half.scene"), std::string::npos) << *afterFirst;

    ASSERT_FALSE(doc.OpenSceneReplace(second));
    const auto afterSecond = doc.GetDisplaySceneName();
    ASSERT_TRUE(afterSecond.has_value());

    EXPECT_EQ(doc.GetScenePath(), std::optional<fs::path>())
        << "both failures cleared the world, so the path is empty across the pair — which is "
           "exactly why it cannot be the only thing a title cache watches";
    EXPECT_NE(*afterSecond, *afterFirst);
    EXPECT_NE(afterSecond->find("second_half.scene"), std::string::npos)
        << "the record names the second file; the display name must follow it: " << *afterSecond;
    EXPECT_EQ(doc.GetLastLoadFailure()->DocumentPath, second);
}

namespace
{
// Does nothing to the world; the undo STACK is the state under test. What a
// real command would carry is EntityHandles into the world the failed open
// destroyed — SceneSwapOrdering.RetireAfterClearDestroysAnEntityOfTheIncomingScene
// is the live-world proof that such a handle silently aliases an entity of
// whatever loads next, which is why the stack cannot be allowed to survive.
class NoopCommand final : public Editor::IEditorCommand
{
  public:
    const char* GetName() const override { return "Noop"; }
    void Do() override {}
    void Undo() override {}
};
} // namespace

// A post-clear failure destroys the world the undo history was recorded against.
// Every successful swap site clears the stack for exactly this reason; the
// failure that clears the world and then stops is the one path that did not.
TEST_F(SceneLoadFailureTests, FailureAfterClearClearsTheUndoStack)
{
    const fs::path good = MintGoodScene("outgoing.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::UndoRedoService undo;
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, &undo);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());

    undo.Execute(std::make_unique<NoopCommand>());
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetLastSceneLoadFailure().has_value());
    ASSERT_TRUE(controller.GetLastSceneLoadFailure()->WorldCleared);

    EXPECT_EQ(undo.GetUndoCount(), 0u)
        << "the world those commands were recorded against is gone; replaying one restores a "
           "snapshot of a destroyed world over the partial load, and its entity handles alias "
           "whatever recycled their indices";
}

// The control: a failure that never reached the clear left the world — and so
// the history — exactly as it was. Dropping undo for it would be its own data
// loss, and would make the fix above indistinguishable from "clear on any
// failed open".
TEST_F(SceneLoadFailureTests, FailureBeforeClearKeepsTheUndoStack)
{
    const fs::path good = MintGoodScene("kept.scene");
    const fs::path bad = WriteScene("dupes.scene", kFailsBeforeClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::UndoRedoService undo;
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, &undo);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    undo.Execute(std::make_unique<NoopCommand>());
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetLastSceneLoadFailure().has_value());
    ASSERT_FALSE(controller.GetLastSceneLoadFailure()->WorldCleared);

    EXPECT_EQ(undo.GetUndoCount(), 1u) << "the document survived; its history still applies to it";
}

// ---------------------------------------------------------------------------
// What the user sees. The record above is read by the debug server and the
// window title; a person at the editor reads neither, so a failed open has to
// put the loader's own words in front of them and say which world they are
// looking at.
// ---------------------------------------------------------------------------

namespace
{
// The text of the modal on screen, top to bottom: every Label carrying
// "modal-message" inside the overlay carrying "visible", one per line, skipping
// blocks taken out of layout with the editor's "hidden" class. Empty when no
// modal is showing.
void AppendVisibleModalText(const UIElement& element, bool insideVisibleModal, std::string& out)
{
    if (element.HasClass("hidden"))
        return;
    const bool visible =
        insideVisibleModal || (element.HasClass("modal-overlay") && element.HasClass("visible"));
    if (visible && element.HasClass("modal-message"))
    {
        if (const auto* label = dynamic_cast<const Label*>(&element))
            out += label->GetText() + "\n";
    }
    for (const auto& child : element.GetChildren())
        AppendVisibleModalText(*child, visible, out);
}

std::string VisibleModalMessage(const UIElement& root)
{
    std::string text;
    AppendVisibleModalText(root, false, text);
    return text;
}

// The labels of the buttons the visible modal shows, skipping any its owner took
// out of layout with the editor's "hidden" class.
void AppendVisibleModalButtons(const UIElement& element, bool insideVisibleModal, std::vector<std::string>& out)
{
    if (element.HasClass("hidden"))
        return;
    const bool visible =
        insideVisibleModal || (element.HasClass("modal-overlay") && element.HasClass("visible"));
    if (visible)
    {
        if (const auto* button = dynamic_cast<const Button*>(&element))
        {
            out.push_back(button->GetText());
            return;
        }
    }
    for (const auto& child : element.GetChildren())
        AppendVisibleModalButtons(*child, visible, out);
}

std::string EntityCountText(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " entity" : " entities");
}
} // namespace

// The common failure: the file is rejected before World::Clear, and the scene
// that was open is still the one on screen. Without the notice the user sees
// that scene and believes the open did nothing, or that their file is gone.
TEST_F(SceneLoadFailureTests, FailedOpenShowsTheLoaderErrorAndSaysThePreviousSceneStaysOpen)
{
    const fs::path good = MintGoodScene("kept.scene");
    const fs::path bad = WriteScene("dupes.scene", kFailsBeforeClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetActiveModalKind().empty()) << "a clean open raises nothing";

    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    const auto& failure = controller.GetLastSceneLoadFailure();
    ASSERT_TRUE(failure.has_value());
    ASSERT_FALSE(failure->WorldCleared);
    ASSERT_FALSE(failure->Message.empty()) << "the loader must have said why for the notice to carry it";

    EXPECT_EQ(controller.GetActiveModalKind(), "loadFailed");
    const std::string shown = VisibleModalMessage(root);
    // The cause first, where it is second, what is on screen last.
    const std::size_t cause = shown.find(failure->Message);
    ASSERT_GT(failure->ErrorLine, 0);
    const std::string where = failure->ErrorFile.filename().string() + ":" + std::to_string(failure->ErrorLine);
    const std::size_t location = shown.find(where);
    const std::string state = "dupes.scene did not load. kept.scene stays open.";
    const std::size_t stateAt = shown.find(state);
    EXPECT_NE(cause, std::string::npos) << "the loader's own words, not a paraphrase: " << shown;
    EXPECT_NE(location, std::string::npos) << "expected '" << where << "' in: " << shown;
    EXPECT_NE(stateAt, std::string::npos) << "the scene still open is named: " << shown;
    EXPECT_LT(cause, location) << shown;
    EXPECT_LT(location, stateAt) << shown;
    std::vector<std::string> buttons;
    AppendVisibleModalButtons(root, false, buttons);
    EXPECT_EQ(buttons, std::vector<std::string>{"OK"})
        << "a notice offers one acknowledgement; Cancel is taken out of layout";
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());
    EXPECT_EQ(*controller.GetActiveScenePath(), good) << "and that is the scene the document still names";

    std::string error;
    ASSERT_TRUE(controller.RespondToActiveModal("ok", &error)) << error;
    EXPECT_TRUE(controller.GetActiveModalKind().empty());
    EXPECT_TRUE(VisibleModalMessage(root).empty());
}

// After the clear the previous scene is gone, and saying it stays open would be
// the same impersonation the record exists to end.
TEST_F(SceneLoadFailureTests, FailedOpenAfterClearSaysThePreviousSceneIsGone)
{
    const fs::path good = MintGoodScene("outgoing.scene");
    const fs::path bad = WriteScene("half.scene", kFailsAfterClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    const auto& failure = controller.GetLastSceneLoadFailure();
    ASSERT_TRUE(failure.has_value());
    ASSERT_TRUE(failure->WorldCleared);

    EXPECT_EQ(controller.GetActiveModalKind(), "loadFailed");
    const std::string shown = VisibleModalMessage(root);
    EXPECT_NE(shown.find(failure->Message), std::string::npos) << shown;
    EXPECT_NE(shown.find("half.scene was only partly loaded. outgoing.scene closed before the error."),
              std::string::npos)
        << shown;
    EXPECT_NE(shown.find("The world now holds " + EntityCountText(failure->EntitiesInWorld) +
                         " from half.scene. The file on disk is unchanged."),
              std::string::npos)
        << "the count agrees with its noun, and the file is not described as lost: " << shown;
    EXPECT_EQ(shown.find("stays open"), std::string::npos)
        << "the outgoing scene was cleared; the notice must not claim it survived: " << shown;
    EXPECT_EQ(shown.find("did not load"), std::string::npos)
        << "part of the file did load; the notice must not say none of it did: " << shown;
    EXPECT_FALSE(controller.IsSceneDirty())
        << "the partial content is not a document to save; marking it dirty invites a Save of it";
}

// After a failure that cleared the world there is no document, only the partial
// load it left. A later failed open must say what the world still holds rather
// than claim a "previous scene" that does not exist — and that partial load
// outlives the next failure's record, so a third failed open still names it.
TEST_F(SceneLoadFailureTests, FailedOpenAfterAPartialLoadNamesWhatTheWorldStillHolds)
{
    const fs::path half = WriteScene("half.scene", kFailsAfterClear);
    const fs::path dupes = WriteScene("dupes.scene", kFailsBeforeClear);
    const fs::path halfAgain = WriteScene("half_again.scene", kFailsAfterClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);
    ScopedUserDataRedirect prefsGuard(m_Dir);
    std::string error;

    controller.RequestOpenScene(half, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetLastSceneLoadFailure().has_value());
    ASSERT_TRUE(controller.GetLastSceneLoadFailure()->WorldCleared);
    ASSERT_FALSE(controller.GetActiveScenePath().has_value());
    ASSERT_TRUE(controller.RespondToActiveModal("ok", &error)) << error;

    // Fails before the clear: the world is still the partial load of half.scene.
    controller.RequestOpenScene(dupes, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    const auto& beforeClear = controller.GetLastSceneLoadFailure();
    ASSERT_TRUE(beforeClear.has_value());
    ASSERT_FALSE(beforeClear->WorldCleared);
    const std::string shownBefore = VisibleModalMessage(root);
    EXPECT_NE(shownBefore.find("dupes.scene did not load. The world still holds " +
                               EntityCountText(beforeClear->EntitiesInWorld) + " from half.scene."),
              std::string::npos)
        << shownBefore;
    EXPECT_EQ(shownBefore.find("stays open"), std::string::npos)
        << "no document is open, so none can stay open: " << shownBefore;
    EXPECT_EQ(shownBefore.find("previous scene"), std::string::npos) << shownBefore;
    ASSERT_TRUE(controller.RespondToActiveModal("ok", &error)) << error;

    // Fails after the clear: what it cleared was that same partial load.
    controller.RequestOpenScene(halfAgain, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetLastSceneLoadFailure().has_value());
    ASSERT_TRUE(controller.GetLastSceneLoadFailure()->WorldCleared);
    const std::string shownAfter = VisibleModalMessage(root);
    EXPECT_NE(shownAfter.find("half_again.scene was only partly loaded. The partial load of half.scene was "
                              "cleared before the error."),
              std::string::npos)
        << shownAfter;
    EXPECT_EQ(shownAfter.find("previous scene"), std::string::npos) << shownAfter;
}

// A notice describes the open that raised it. A later open that succeeds makes
// it false, so it goes with that open rather than waiting to be dismissed.
TEST_F(SceneLoadFailureTests, ALaterOpenTakesTheFailureNoticeDown)
{
    const fs::path good = MintGoodScene("later.scene");
    const fs::path bad = WriteScene("dupes.scene", kFailsBeforeClear);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(bad, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_EQ(controller.GetActiveModalKind(), "loadFailed")
        << "the take-down below is only meaningful if the notice went up";

    controller.RequestOpenScene(good, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    EXPECT_FALSE(controller.GetLastSceneLoadFailure().has_value());
    EXPECT_TRUE(controller.GetActiveModalKind().empty());
    EXPECT_TRUE(VisibleModalMessage(root).empty());
}
} // namespace
