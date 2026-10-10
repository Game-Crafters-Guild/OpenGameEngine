// The pre-swap contract: everything scoped to the outgoing scene is released
// immediately BEFORE the world is swapped, at both swap sites and at neither
// other path.
//
// Ordering, not presence, is the property — and it is a source-level one. The
// hazard it prevents is an ABA: World::Clear recycles entity indices and
// restarts versions in place, so an HLOD cluster's proxy handle carried across
// the Clear names an unrelated freshly-loaded entity, and the generational
// IsValid check cannot tell. HlodRuntime::Retire then destroys it. The
// companion test below reproduces exactly that on a live world, which is what
// makes the ordering assertions here worth pinning rather than hypothetical.
//
// The third assertion is the one with teeth: calling the step from
// LoadSceneAdditive would be user data loss. That path never clears the world,
// so retiring proxies there destroys the SURVIVING scene's far-field geometry
// and the payload reset discards its live terrain zone edits.

#include <gtest/gtest.h>

#include "Assets/PolyhavenDownloadManager.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Engine/Rendering/MeshGPURegistry.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;

std::filesystem::path EditorSourceRoot()
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR);
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string SceneDocumentManagerSource()
{
    return ReadFile(EditorSourceRoot() / "Source" / "Scene" / "SceneDocumentManager.cpp");
}

std::string SceneEditorControllerSource()
{
    return ReadFile(EditorSourceRoot() / "Source" / "Scene" / "SceneEditorController.cpp");
}

// Offset of the next member definition of `qualifier` ("Foo::") at or after
// `from`. A definition starts at column 0; an indented occurrence is a call and
// a leading "//" is prose, so both are skipped.
size_t NextMemberDefinition(const std::string& src, const std::string& qualifier, size_t from)
{
    for (size_t at = src.find(qualifier, from); at != std::string::npos;
         at = src.find(qualifier, at + qualifier.size()))
    {
        const size_t newline = src.rfind('\n', at);
        if (newline == std::string::npos || newline + 1 >= src.size())
            continue;
        const size_t lineStart = newline + 1;
        const char first = src[lineStart];
        if (first == ' ' || first == '\t' || first == '\n')
            continue;
        if (src.compare(lineStart, 2, "//") == 0)
            continue;
        return lineStart;
    }
    return std::string::npos;
}

// Text of one member function's body, from its definition to the start of the
// next member definition of the same class. Good enough to ask "does this
// function mention X, and does it mention X before Y" without a C++ parser.
//
// Body-scoped, not file-scoped, and that distinction is load-bearing: a literal
// that also appears in a sibling function makes a whole-file grep pass while the
// line it exists to pin is gone.
std::string FunctionBody(const std::string& src, const std::string& qualifier,
                         const std::string& signature)
{
    const size_t start = src.find(signature);
    if (start == std::string::npos)
        return {};
    const size_t end = NextMemberDefinition(src, qualifier, start + signature.size());
    return src.substr(start, (end == std::string::npos ? src.size() : end) - start);
}

} // namespace

// The replace-open swap site. LoadSceneFromFile is what reaches World::Clear,
// so the release has to precede the call, not merely appear in the function.
TEST(SceneSwapOrdering, ReplaceOpenReleasesSceneScopedStateBeforeTheLoad)
{
    const std::string src = SceneDocumentManagerSource();
    ASSERT_FALSE(src.empty()) << "SceneDocumentManager.cpp not found under " << GE_EDITOR_SOURCE_DIR;

    const std::string body = FunctionBody(src, "SceneDocumentManager::", "bool SceneDocumentManager::OpenSceneFromFile(");
    ASSERT_FALSE(body.empty());

    const size_t release = body.find("ReleaseSceneScopedState(");
    const size_t load = body.find("Scene::LoadSceneFromFile(");
    ASSERT_NE(release, std::string::npos)
        << "the replace-open must release the outgoing scene's state through the shared step";
    ASSERT_NE(load, std::string::npos);
    EXPECT_LT(release, load)
        << "the release must precede LoadSceneFromFile — that call is what reaches World::Clear, "
           "and a Retire after it destroys entities of the scene being loaded";
}

// The new-document swap site clears the world itself.
TEST(SceneSwapOrdering, NewSceneReleasesSceneScopedStateBeforeTheClear)
{
    const std::string src = SceneDocumentManagerSource();
    const std::string body = FunctionBody(src, "SceneDocumentManager::", "void SceneDocumentManager::NewSceneUntitled(");
    ASSERT_FALSE(body.empty());

    const size_t release = body.find("ReleaseSceneScopedState(");
    const size_t clear = body.find("m_World->Clear()");
    ASSERT_NE(release, std::string::npos);
    ASSERT_NE(clear, std::string::npos);
    EXPECT_LT(release, clear);
}

// The exclusion that keeps the fix from becoming a data-loss bug.
TEST(SceneSwapOrdering, AdditiveLoadNeverReleasesSceneScopedState)
{
    const std::string src = SceneDocumentManagerSource();
    const std::string body = FunctionBody(src, "SceneDocumentManager::", "bool SceneDocumentManager::LoadSceneAdditive(");
    ASSERT_FALSE(body.empty());

    EXPECT_EQ(body.find("ReleaseSceneScopedState("), std::string::npos)
        << "an additive load keeps the world: retiring HLOD there destroys the surviving scene's "
           "proxies and the payload reset discards its live terrain edits";
    EXPECT_NE(body.find("CancelSceneBuild()"), std::string::npos)
        << "it must still drop an in-flight replace-open build, which is the one part it shares";
}

// The shared step must actually carry the HLOD retire — the ordering assertions
// above are worthless if the step it orders does nothing.
TEST(SceneSwapOrdering, TheSharedStepRetiresHlod)
{
    const std::string src = SceneDocumentManagerSource();
    const std::string body =
        FunctionBody(src, "SceneDocumentManager::", "void SceneDocumentManager::ReleaseSceneScopedState(");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("Retire("), std::string::npos);
    EXPECT_NE(body.find("CancelSceneBuild()"), std::string::npos);
    EXPECT_NE(body.find("ClearZonePayloads()"), std::string::npos);
}

// The spline generators (placement / fence / extrude / wall) cache handles to the
// entities they generate and retire them on a later sweep, keyed off the spline
// entity still being present. An incoming scene with no splines makes the whole
// cached set unvisited, so the sweep retires it against the NEW world — the same
// ABA the HLOD retire above answers, and the mechanism behind the reproduced
// "scene load settles short" (a large island scene after RiverVignette lost exactly the two
// entities that recycled the two extrude-chunk indices).
TEST(SceneSwapOrdering, TheSharedStepReleasesTheSplineGenerators)
{
    const std::string src = SceneDocumentManagerSource();
    const std::string body =
        FunctionBody(src, "SceneDocumentManager::", "void SceneDocumentManager::ReleaseSceneScopedState(");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("m_SceneScopedGeneratorRelease"), std::string::npos)
        << "the shared step must release the spline generators, or their cached handles "
           "survive the swap and the next sweep destroys entities of the incoming scene";
}

// The release is supplied by the generators' owner, so the wiring is part of the
// contract: an unwired hook is a silent no-op that reads exactly like a fix. The
// editor wires it to RecipeControllers, which owns the generators, and that
// release must reach every one of them.
TEST(SceneSwapOrdering, EditorApplicationWiresTheSplineGeneratorRelease)
{
    const std::string src = ReadFile(EditorSourceRoot() / "Source" / "EditorApplication.cpp");
    ASSERT_FALSE(src.empty());

    const size_t wire = src.find("SetSceneScopedGeneratorRelease(");
    ASSERT_NE(wire, std::string::npos)
        << "EditorApplication must supply the release to the scene editor";
    EXPECT_NE(src.substr(wire).find("m_RecipeControllers->ReleaseSceneScoped("), std::string::npos)
        << "the release must reach the owner of the spline generators";

    const std::string owner =
        ReadFile(EditorSourceRoot() / "Source" / "Placement" / "RecipeControllers.cpp");
    const std::string body = FunctionBody(owner, "RecipeControllers::",
                                          "void RecipeControllers::ReleaseSceneScoped(");
    ASSERT_FALSE(body.empty()) << "RecipeControllers::ReleaseSceneScoped not found";
    // Every generator caches handles; releasing only some still drops entities.
    EXPECT_NE(body.find("m_Placement->Shutdown("), std::string::npos);
    EXPECT_NE(body.find("m_Fence->Shutdown("), std::string::npos);
    EXPECT_NE(body.find("m_Extrude->Shutdown("), std::string::npos);
    EXPECT_NE(body.find("m_Wall->Shutdown("), std::string::npos);
}

// The owner wires the release at service-construction time, which runs before
// AttachToRoot builds the document manager. A setter that forwards only when
// m_Doc already exists — the shape every sibling setter in that class uses —
// drops it on the floor, and an unarmed release is indistinguishable from a
// working one: scenes still swap, just without releasing anything. This escaped
// the two assertions above (both were green) and was caught only by counting
// entities in a running editor, so it is pinned here.
TEST(SceneSwapOrdering, SceneEditorControllerHoldsTheReleaseUntilTheDocumentExists)
{
    const std::string src = SceneEditorControllerSource();
    ASSERT_FALSE(src.empty());

    // Both halves must be asserted in their OWN function body. The forward
    // literal appears in the setter too, so a whole-file search is satisfied by
    // the setter alone and stays green with the AttachToRoot line deleted —
    // which is precisely the regression this test exists to catch.
    const std::string setter =
        FunctionBody(src, "SceneEditorController::",
                     "void SceneEditorController::SetSceneScopedGeneratorRelease(");
    ASSERT_FALSE(setter.empty()) << "setter definition not found";
    EXPECT_NE(setter.find("m_SceneScopedGeneratorRelease = std::move(release)"), std::string::npos)
        << "the setter must STORE the release rather than forward-or-drop it";

    const std::string attach = FunctionBody(src, "SceneEditorController::",
                                            "void SceneEditorController::AttachToRoot(");
    ASSERT_FALSE(attach.empty()) << "AttachToRoot definition not found";
    EXPECT_NE(attach.find("SetSceneScopedGeneratorRelease(m_SceneScopedGeneratorRelease)"),
              std::string::npos)
        << "AttachToRoot builds m_Doc, so it must hand the stored release over; without that "
           "line nothing ever arms the release and every scene swap silently skips it";
}

// The hazard itself, on a live world. Not a regression test for the fix — the
// fix is the ordering above — but the evidence that a Retire carried across a
// Clear destroys an unrelated entity, which is why the ordering is load-bearing.
TEST(SceneSwapOrdering, RetireAfterClearDestroysAnEntityOfTheIncomingScene)
{
    World world;
    GameEngine::Rendering::MeshGPURegistry meshRegistry; // uninitialized: no proxy mesh keys below

    const EntityHandle outgoingProxy = world.CreateEntity();
    world.AddComponentImmediate(outgoingProxy, GameEngine::Components::Transform{});

    GameEngine::Hlod::HlodRuntime hlod;
    GameEngine::Hlod::RuntimeCluster cluster{};
    cluster.Proxies.push_back(outgoingProxy);
    hlod.GetClusters().push_back(cluster);

    // The swap.
    world.Clear();
    world.ProcessCommands();

    // The incoming scene's first entity lands on the recycled index.
    const EntityHandle incoming = world.CreateEntity();
    world.AddComponentImmediate(incoming, GameEngine::Components::Transform{});
    ASSERT_TRUE(world.IsValid(incoming));

    // A stale proxy handle is indistinguishable from the live one it now aliases.
    EXPECT_TRUE(world.IsValid(outgoingProxy))
        << "if this ever fails the ABA is gone and the ordering constraint can be revisited";

    hlod.Retire(world, meshRegistry);
    world.ProcessCommands();

    EXPECT_FALSE(world.IsValid(incoming))
        << "a Retire deferred past World::Clear destroys an entity of the scene just loaded — "
           "this is the reason ReleaseSceneScopedState runs before the swap";
}

// The second version-reset path, which no swap-site hook can reach.
// World::DeserializeWorld calls Clear() and rebuilds the serialized identities.
// Handles can therefore remain valid across play-mode exit/undo snapshot restore
// even though transient resources must be rebuilt. This path does not pass through
// ReleaseSceneScopedState; generators must still observe the reset generation.
TEST(SceneSwapOrdering, DeserializeWorldPreservesHandlesAndBumpsTheResetGeneration)
{
    World world;

    const EntityHandle cached = world.CreateEntity();
    world.AddComponentImmediate(cached, GameEngine::Components::Transform{});

    const std::vector<std::uint8_t> snapshot = world.SerializeWorld();
    const GameEngine::uint64 generationBefore = world.GetLifecycleResetGeneration();

    world.DeserializeWorld(snapshot);
    world.ProcessCommands();

    // Fact 1 — a handle cached before restore still passes IsValid. Serialized
    // identity is preserved, but cached runtime resources do not survive Clear.
    EXPECT_TRUE(world.IsValid(cached))
        << "restoring a snapshot preserves its entity identities";

    // Fact 2 — the signal the generators key their drop off actually fires here.
    // Without it the guard is inert on exactly the path it exists to cover.
    EXPECT_NE(world.GetLifecycleResetGeneration(), generationBefore)
        << "World::Clear signals a lifecycle reset, and DeserializeWorld goes through Clear";
}

namespace
{
std::string PlacementSource(const char* fileName)
{
    return ReadFile(EditorSourceRoot() / "Source" / "Placement" / fileName);
}
} // namespace

// Each generator must drop its cached handles when the world is reset under it.
// The swap-site release covers scene open / new scene; this covers the
// deserialize-driven Clears, and it has to live in every generator because the
// three keep independent caches.
TEST(SceneSwapOrdering, EveryGeneratorDropsItsCacheOnAWorldReset)
{
    struct Generator
    {
        const char* File;
        const char* Qualifier;
        const char* UpdateSignature;
    };
    const Generator generators[] = {
        {"SplinePlacementController.cpp", "SplinePlacementController::",
         "void SplinePlacementController::Update("},
        {"SplineFenceController.cpp", "SplineFenceController::",
         "void SplineFenceController::Update("},
        {"SplineExtrudeController.cpp", "SplineExtrudeController::",
         "void SplineExtrudeController::Update("},
    };

    for (const Generator& g : generators)
    {
        const std::string src = PlacementSource(g.File);
        ASSERT_FALSE(src.empty()) << g.File << " not found under " << GE_EDITOR_SOURCE_DIR;

        const std::string body = FunctionBody(src, g.Qualifier, g.UpdateSignature);
        ASSERT_FALSE(body.empty()) << "Update definition not found in " << g.File;

        const size_t read = body.find("GetLifecycleResetGeneration()");
        EXPECT_NE(read, std::string::npos)
            << g.File << ": Update must compare the world's lifecycle reset generation — a "
                         "deserialize-driven Clear reaches no swap-site hook";

        const size_t sweep = body.find("VisitStamp");
        ASSERT_NE(sweep, std::string::npos) << g.File << ": expected the visit-stamp sweep";
        EXPECT_LT(read, sweep)
            << g.File << ": the reset check must run BEFORE the sweep, or the sweep retires "
                         "stale handles against the new world first";
    }
}

// Every generator that reads the ground must hold its FIRST build until that
// ground has been provisioned.
//
// This is a source scan because it has to be: none of the controllers is
// compiled into any test target (they need live RenderServices, and every
// RenderServices fixture in the tree needs a real Vulkan device), so the gate's
// PRESENCE at the call sites has no other instrument. The hold itself lives in
// SplineRebuildGate::ShouldRebuild and is unit-tested over the real class in
// SplineRebuildGateTests (and DeferFirstBuild in SplineSurfaceObservationTests);
// this pins the wiring.
TEST(SceneSwapOrdering, EveryGroundReadingGeneratorHoldsItsFirstBuildForProvisioning)
{
    struct Generator
    {
        const char* File;
        const char* Qualifier;
        const char* UpdateSignature;
    };
    const Generator generators[] = {
        {"SplinePlacementController.cpp", "SplinePlacementController::",
         "void SplinePlacementController::Update("},
        {"SplineFenceController.cpp", "SplineFenceController::",
         "void SplineFenceController::Update("},
        {"SplineExtrudeController.cpp", "SplineExtrudeController::",
         "void SplineExtrudeController::Update("},
        {"SplineWallController.cpp", "SplineWallController::",
         "void SplineWallController::Update("},
    };

    for (const Generator& g : generators)
    {
        const std::string src = PlacementSource(g.File);
        ASSERT_FALSE(src.empty()) << g.File << " not found under " << GE_EDITOR_SOURCE_DIR;

        const std::string body = FunctionBody(src, g.Qualifier, g.UpdateSignature);
        ASSERT_FALSE(body.empty()) << "Update definition not found in " << g.File;

        const size_t readiness = body.find("ConformSurfaceState(");
        EXPECT_NE(readiness, std::string::npos)
            << g.File << ": Update must ask whether the ground exists yet — the surface DIGEST "
                         "cannot answer it, an unprovisioned terrain folds no terms";

        const size_t hold = body.find("Gate.ShouldRebuild(");
        ASSERT_NE(hold, std::string::npos)
            << g.File << ": Update must ask the rebuild gate, which defers the first build "
                         "while the ground is not final";

        // The hold has to gate the build, not trail it.
        const size_t rebuild = body.find("if (Rebuild(");
        ASSERT_NE(rebuild, std::string::npos) << g.File << ": expected a Rebuild call";
        EXPECT_LT(hold, rebuild)
            << g.File << ": the rebuild gate must run BEFORE the rebuild it gates";
        EXPECT_LT(readiness, hold)
            << g.File << ": readiness must be read before it is acted on";
    }
}

// ---------------------------------------------------------------------------
// The Polyhaven download manager: the same hazard, held across a download
// rather than across a frame.
//
// The generators above cache handles between Updates, so comparing one cached
// generation at the top of Update covers every handle they hold. The download
// manager instead records a handle when the user drops an asset and spends it
// when the transfer finishes — an unbounded gap, over which the user is free to
// open another scene, leave play mode or undo. Each recorded handle therefore
// carries the generation current when IT was recorded, and PolyhavenDownloadManager
// ::ScopedEntity is what pairs them.
// ---------------------------------------------------------------------------

namespace
{
using ScopedEntity = GameEngine::PolyhavenDownloadManager::ScopedEntity;

std::string PolyhavenManagerSource()
{
    return ReadFile(EditorSourceRoot() / "Source" / "Assets" / "PolyhavenDownloadManager.cpp");
}

// Body of the SECOND definition matching `signature`. AssignHDRIToSkyboxOnComplete
// is overloaded — the first definition delegates, the second is the one that
// stores — and both share their opening line, so FunctionBody alone lands on the
// delegating one. Anchoring on the qualified name rather than on a parameter that
// could migrate to another function keeps this pointed at the storing overload.
std::string SecondDefinitionBody(const std::string& src, const std::string& qualifier,
                                 const std::string& signature)
{
    const size_t first = src.find(signature);
    if (first == std::string::npos)
        return {};
    return FunctionBody(src.substr(first + signature.size()), qualifier, signature);
}
} // namespace

// The mechanism, on a live world: the raw handle survives the reset and the
// guarded one does not. Without the generation there is nothing to tell them
// apart, which is the whole reason the download completion could destroy or
// mutate an entity of the incoming scene.
TEST(SceneSwapOrdering, ScopedEntityDropsAHandleTheWorldResetInvalidated)
{
    World world;

    const EntityHandle placeholder = world.CreateEntity();
    world.AddComponentImmediate(placeholder, GameEngine::Components::Transform{});
    const ScopedEntity recorded = ScopedEntity::Capture(placeholder, &world);

    // A download is in flight while the user opens another scene.
    world.Clear();
    world.ProcessCommands();

    const EntityHandle incoming = world.CreateEntity();
    world.AddComponentImmediate(incoming, GameEngine::Components::Transform{});

    // The hazard: the stale handle is indistinguishable from the live one.
    ASSERT_TRUE(world.IsValid(recorded.Entity))
        << "if this ever fails the ABA is gone and the guard can be revisited";
    ASSERT_EQ(recorded.Entity.index, incoming.index)
        << "expected the incoming entity to land on the recycled index";

    // The guard: recorded, but no longer spendable.
    EXPECT_TRUE(recorded.WasRecorded());
    EXPECT_TRUE(recorded.IsStale(&world));
    EXPECT_FALSE(recorded.Resolve(&world).IsValid())
        << "spending this handle would destroy or mutate an entity of the scene just loaded";
}

// The deserialize path — play-mode exit and undo snapshot restore — reaches
// World::Clear without passing any swap-site hook, and a download in flight
// spans it just as easily as it spans a scene open.
TEST(SceneSwapOrdering, ScopedEntityDropsAHandleAcrossDeserializeWorld)
{
    World world;

    const EntityHandle placeholder = world.CreateEntity();
    world.AddComponentImmediate(placeholder, GameEngine::Components::Transform{});
    const ScopedEntity recorded = ScopedEntity::Capture(placeholder, &world);

    const std::vector<std::uint8_t> snapshot = world.SerializeWorld();
    world.DeserializeWorld(snapshot);
    world.ProcessCommands();

    ASSERT_TRUE(world.IsValid(recorded.Entity))
        << "if this ever fails the ABA is gone on the deserialize path";
    EXPECT_FALSE(recorded.Resolve(&world).IsValid())
        << "play-mode exit and undo restore must invalidate a recorded placeholder";
}

// The positive control. A guard that never resolves would pass both tests above
// while silently breaking every download, so pin that an untouched world still
// spends the handle — and that a placeholder the user simply deleted resolves
// invalid WITHOUT being stale, which is what keeps that case on its existing
// path (the model still spawns at the saved transform).
TEST(SceneSwapOrdering, ScopedEntityStillResolvesWhenTheWorldHasNotMoved)
{
    World world;

    const EntityHandle placeholder = world.CreateEntity();
    world.AddComponentImmediate(placeholder, GameEngine::Components::Transform{});
    const ScopedEntity recorded = ScopedEntity::Capture(placeholder, &world);

    EXPECT_EQ(recorded.Resolve(&world), placeholder)
        << "an in-flight download over an untouched world must still find its placeholder";

    world.DestroyEntity(placeholder);
    world.ProcessCommands();

    EXPECT_FALSE(recorded.IsStale(&world))
        << "deleting one entity is not a world reset — the download still has a scene to land in";
    EXPECT_FALSE(recorded.Resolve(&world).IsValid());
    EXPECT_TRUE(recorded.WasRecorded())
        << "WasRecorded distinguishes 'no placeholder was ever assigned' from 'it is gone'";
}

// A default-constructed guard is the early-download case: nothing was recorded,
// so nothing resolves, whatever the world's generation happens to be. Pinned
// because generation 0 is also a fresh world's generation.
TEST(SceneSwapOrdering, ScopedEntityWithNoRecordedTargetNeverResolves)
{
    World world;
    const ScopedEntity none{};

    EXPECT_FALSE(none.WasRecorded());
    EXPECT_FALSE(none.Resolve(&world).IsValid());
    EXPECT_FALSE(none.Resolve(nullptr).IsValid());
}

// Every site that spends a recorded handle must go through Resolve(). This is
// the contract a behavioural test cannot reach: driving a real completion needs
// a live HTTP transfer, an AssetManager and RenderServices, so what is pinned
// here is that the guard is actually consulted at each spend site rather than
// that the download works. The body-scoped check matters — Resolve appears
// several times in this file, and a whole-file grep would pass while any single
// site still read the raw handle.
TEST(SceneSwapOrdering, EveryPolyhavenSpendSiteResolvesThroughTheGuard)
{
    const std::string src = PolyhavenManagerSource();
    ASSERT_FALSE(src.empty()) << "PolyhavenDownloadManager.cpp not found under " << GE_EDITOR_SOURCE_DIR;

    struct SpendSite
    {
        const char* What;
        const char* Signature;
    };
    const SpendSite sites[] = {
        {"placeholder replacement (destroys entities)",
         "void PolyhavenDownloadManager::ReplacePlaceholder("},
        {"multi-map PBR assignment",
         "void PolyhavenDownloadManager::ProcessMultiTextureAssignment("},
        {"HDRI skybox assignment",
         "void PolyhavenDownloadManager::ProcessHDRISkyboxAssignment("},
        {"completion dispatch",
         "void PolyhavenDownloadManager::PollAndProcess("},
    };

    for (const SpendSite& site : sites)
    {
        const std::string body = FunctionBody(src, "PolyhavenDownloadManager::", site.Signature);
        ASSERT_FALSE(body.empty()) << "definition not found: " << site.Signature;

        EXPECT_NE(body.find("Resolve("), std::string::npos)
            << site.What << ": must resolve the recorded handle against the current world — a "
                            "handle cached across a download outlives the scene it named";
    }

    // The two sites that decide policy on a reset rather than merely skipping
    // work: the dispatch drops the placeholder reference (the reset already
    // destroyed the entity — destroying anything here would hit the new scene),
    // and the replacement refuses to spawn the model into a scene the user never
    // dropped it into.
    const std::string dispatch =
        FunctionBody(src, "PolyhavenDownloadManager::", "void PolyhavenDownloadManager::PollAndProcess(");
    EXPECT_NE(dispatch.find("IsStale("), std::string::npos)
        << "the completion dispatch must recognise a world reset explicitly, not just fail to resolve";

    const std::string replace =
        FunctionBody(src, "PolyhavenDownloadManager::", "void PolyhavenDownloadManager::ReplacePlaceholder(");
    EXPECT_NE(replace.find("IsStale("), std::string::npos)
        << "ReplacePlaceholder must refuse to place a model whose target scene is gone";
}

// Every site that records a handle must capture the generation with it. A record
// site that stored a bare handle would leave Generation at 0 and read as stale
// forever — the download would silently stop working rather than corrupt a
// scene, but it would be just as broken.
TEST(SceneSwapOrdering, EveryPolyhavenRecordSiteCapturesTheGeneration)
{
    const std::string src = PolyhavenManagerSource();
    ASSERT_FALSE(src.empty()) << "PolyhavenDownloadManager.cpp not found under " << GE_EDITOR_SOURCE_DIR;

    struct RecordSite
    {
        const char* What;
        const char* Signature;
    };
    const RecordSite sites[] = {
        {"drop-driven download", "uint32_t PolyhavenDownloadManager::StartDownloadForPlaceholder("},
        {"placeholder attached to an in-flight download",
         "void PolyhavenDownloadManager::AssignPlaceholder("},
        {"multi-map PBR drop", "bool PolyhavenDownloadManager::DownloadAllTextureMaps("},
    };

    for (const RecordSite& site : sites)
    {
        const std::string body = FunctionBody(src, "PolyhavenDownloadManager::", site.Signature);
        ASSERT_FALSE(body.empty()) << "definition not found: " << site.Signature;

        EXPECT_NE(body.find("ScopedEntity::Capture("), std::string::npos)
            << site.What << ": must record the world's reset generation alongside the handle";
    }

    // The storing HDRI overload, reached by name rather than by parameter.
    const std::string hdri = SecondDefinitionBody(
        src, "PolyhavenDownloadManager::", "void PolyhavenDownloadManager::AssignHDRIToSkyboxOnComplete(");
    ASSERT_FALSE(hdri.empty())
        << "expected two AssignHDRIToSkyboxOnComplete definitions, the second being the storing one";
    EXPECT_NE(hdri.find("ScopedEntity::Capture("), std::string::npos)
        << "HDRI dropped on a skybox: must record the world's reset generation alongside the handle";
}

// Nothing may reach past the guard to the raw handle. This is the invariant that
// keeps the fix from eroding: ScopedEntity's whole point is that the only way to
// obtain a spendable handle is Resolve(), so a direct read of .Entity anywhere in
// the implementation is a spend site that skipped the check.
TEST(SceneSwapOrdering, PolyhavenNeverReadsARecordedHandleDirectly)
{
    const std::string src = PolyhavenManagerSource();
    ASSERT_FALSE(src.empty()) << "PolyhavenDownloadManager.cpp not found under " << GE_EDITOR_SOURCE_DIR;

    for (const char* raw : {".Placeholder.Entity", ".Target.Entity"})
    {
        EXPECT_EQ(src.find(raw), std::string::npos)
            << "PolyhavenDownloadManager.cpp reads " << raw << " directly; spend the handle through "
               "Resolve(world) so a world reset since it was recorded is caught";
    }
}
