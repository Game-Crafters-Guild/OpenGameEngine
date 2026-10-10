// The debug server's drop: the payload reaches the drop target under the point through the window's
// drag-and-drop manager, so the target's own CanDrop and PerformDrop run as for a drop with the mouse,
// at the point given; a point over no target is reported, not dropped. A file outside the asset
// sources, a missing file, or a drag already in progress is refused before any drop.

#include <gtest/gtest.h>

#include "../TestTempDir.h"
#include "DebugServer/PerformDropRequest.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using GameEngine::UIElement;
using GameEngine::UIManager;
using GameEngine::TestUtils::MakeUniqueTempDirectory;
using GameEngine::TestUtils::ScopedTempDir;
namespace Interaction = GameEngine::UI::Interaction;

namespace
{

// A target that takes asset paths anywhere in its box and records each drop. With MoveInto set
// it moves the dropped files into that folder, as the Assets panel's drop does.
class RecordingDropTarget final : public UIElement, public Interaction::IDropTarget
{
  public:
    bool AcceptsPayload(Interaction::PayloadTypeId typeId) const override
    {
        return typeId == Interaction::GetPayloadTypeId<GameEngine::Editor::AssetPathsDragPayload>();
    }
    bool HitTestDropTarget(float, float, Interaction::DropHit& out) const override
    {
        out = Interaction::DropHit{};
        return true;
    }
    Interaction::DropFeedback CanDrop(const Interaction::DropRequest&) const override
    {
        Interaction::DropFeedback feedback;
        feedback.Allowed = true;
        return feedback;
    }
    void PerformDrop(const Interaction::DropRequest& request) override
    {
        const auto* files = request.payload.TryGet<GameEngine::Editor::AssetPathsDragPayload>();
        if (!files)
            return;
        Dropped.push_back(files->paths);
        if (MoveInto.empty())
            return;
        for (const std::filesystem::path& path : files->paths)
        {
            std::error_code ec;
            std::filesystem::rename(path, MoveInto / path.filename(), ec);
        }
    }
    void SetDropPreview(const Interaction::DropPreviewState&) override {}

    std::vector<std::vector<std::filesystem::path>> Dropped;
    std::filesystem::path MoveInto;
};

// A 400x300 window whose top-left 200x100 is a RecordingDropTarget.
RecordingDropTarget* MountTarget(UIManager& ui)
{
    ui.SetLayoutSizeOverride(400, 300);
    auto root = std::make_unique<UIElement>();
    root->Overrides()
        .Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(400.0f))
        .Set(GameEngine::Style::Height, GameEngine::StyleLength::Px(300.0f));
    auto targetOwned = std::make_unique<RecordingDropTarget>();
    RecordingDropTarget* target = targetOwned.get();
    target->Overrides()
        .Set(GameEngine::Style::Width, GameEngine::StyleLength::Px(200.0f))
        .Set(GameEngine::Style::Height, GameEngine::StyleLength::Px(100.0f));
    root->AddChild(std::move(targetOwned));
    ui.SetRoot(std::move(root));
    ui.Update(0.0f, /*interactive=*/false);
    return target;
}

std::filesystem::path WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "probe";
    return path;
}

} // namespace

TEST(PerformDropRequestTests, ThePayloadReachesTheTargetUnderThePointOnly)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);
    const ScopedTempDir dir(MakeUniqueTempDirectory("ge_perform_drop"));
    const std::filesystem::path assets = dir.Path() / "project" / "Assets";
    const std::filesystem::path a = WriteFile(assets / "a.glb");
    const std::filesystem::path b = WriteFile(assets / "b.glb");

    const auto onTarget = GameEngine::Editor::PerformAssetFileDrop(ui, {a}, {assets}, 50.0f, 50.0f);
    EXPECT_TRUE(onTarget.Refusal.empty()) << onTarget.Refusal;
    EXPECT_TRUE(onTarget.Accepted) << onTarget.Reason;
    ASSERT_EQ(target->Dropped.size(), 1u) << "the target's PerformDrop did not run";
    EXPECT_EQ(target->Dropped.front().front(), a.lexically_normal());

    const auto offTarget = GameEngine::Editor::PerformAssetFileDrop(ui, {b}, {assets}, 350.0f, 250.0f);
    EXPECT_FALSE(offTarget.Accepted);
    EXPECT_FALSE(offTarget.Reason.empty()) << "a drop over no target did not say why";
    EXPECT_EQ(target->Dropped.size(), 1u) << "a drop beside the target reached it";
}

TEST(PerformDropRequestTests, AFileOutsideTheAssetSourcesIsRefusedAndStaysWhereItIs)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);
    const ScopedTempDir dir(MakeUniqueTempDirectory("ge_perform_drop_outside"));
    const std::filesystem::path assets = dir.Path() / "project" / "Assets";
    std::filesystem::create_directories(assets);
    target->MoveInto = assets;
    const std::filesystem::path outside = WriteFile(dir.Path() / "outside" / "probe.txt");

    const auto outcome = GameEngine::Editor::PerformAssetFileDrop(ui, {outside}, {assets}, 50.0f, 50.0f);
    EXPECT_NE(outcome.Refusal.find("outside the asset sources"), std::string::npos) << outcome.Refusal;
    EXPECT_NE(outcome.Refusal.find("Copy the file under one of them"), std::string::npos) << outcome.Refusal;
    EXPECT_TRUE(target->Dropped.empty()) << "the target's PerformDrop ran for a file outside the asset sources";
    EXPECT_TRUE(std::filesystem::exists(outside)) << "the file outside the asset sources was moved";
    EXPECT_FALSE(std::filesystem::exists(assets / "probe.txt"));
}

TEST(PerformDropRequestTests, AMissingFileIsRefusedBeforeTheDrop)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);
    const ScopedTempDir dir(MakeUniqueTempDirectory("ge_perform_drop_missing"));
    const std::filesystem::path assets = dir.Path() / "project" / "Assets";
    std::filesystem::create_directories(assets);

    const auto outcome =
        GameEngine::Editor::PerformAssetFileDrop(ui, {assets / "does_not_exist.glb"}, {assets}, 50.0f, 50.0f);
    EXPECT_NE(outcome.Refusal.find("No file or folder at"), std::string::npos) << outcome.Refusal;
    EXPECT_FALSE(outcome.Accepted);
    EXPECT_TRUE(target->Dropped.empty());
}

TEST(PerformDropRequestTests, ADragInProgressIsNotReplaced)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);
    const ScopedTempDir dir(MakeUniqueTempDirectory("ge_perform_drop_dragging"));
    const std::filesystem::path assets = dir.Path() / "project" / "Assets";
    const std::filesystem::path a = WriteFile(assets / "a.glb");
    Interaction::DragDropManager* dragDrop = ui.GetDragDropManager();
    ASSERT_NE(dragDrop, nullptr);
    dragDrop->BeginDrag(Interaction::DragPayload::Create(GameEngine::Editor::AssetPathsDragPayload{}));

    const auto outcome = GameEngine::Editor::PerformAssetFileDrop(ui, {a}, {assets}, 50.0f, 50.0f);
    EXPECT_NE(outcome.Refusal.find("in progress"), std::string::npos) << outcome.Refusal;
    EXPECT_TRUE(target->Dropped.empty());
    EXPECT_TRUE(dragDrop->IsDragging()) << "the session in progress was replaced or ended";
}

TEST(PerformDropRequestTests, AFileReachedThroughAJunctionThatLeavesTheAssetSourcesIsRefused)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);
    const ScopedTempDir dir(MakeUniqueTempDirectory("ge_perform_drop_junction"));
    const std::filesystem::path assets = dir.Path() / "project" / "Assets";
    std::filesystem::create_directories(assets);
    target->MoveInto = assets;
    const std::filesystem::path outside = dir.Path() / "outside";
    const std::filesystem::path outsideFile = WriteFile(outside / "probe.txt");
    const std::filesystem::path link = assets / "escape";
#if defined(_WIN32)
    const std::wstring command = L"cmd.exe /C mklink /J \"" + link.wstring() + L"\" \"" + outside.wstring() + L"\" >NUL";
    ASSERT_EQ(_wsystem(command.c_str()), 0);
#else
    std::filesystem::create_directory_symlink(outside, link);
#endif

    const auto outcome = GameEngine::Editor::PerformAssetFileDrop(ui, {link / "probe.txt"}, {assets}, 50.0f, 50.0f);
    EXPECT_NE(outcome.Refusal.find("outside the asset sources"), std::string::npos) << outcome.Refusal;
    EXPECT_TRUE(target->Dropped.empty()) << "the target's PerformDrop ran for a file reached through the junction";
    EXPECT_TRUE(std::filesystem::exists(outsideFile)) << "the file behind the junction was moved";
}

TEST(PerformDropRequestTests, AnEmptyPathListIsRefused)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "no Vulkan device";
    UIManager ui(device.get());
    RecordingDropTarget* target = MountTarget(ui);

    const auto outcome = GameEngine::Editor::PerformAssetFileDrop(ui, {}, {"C:/project/Assets"}, 50.0f, 50.0f);
    EXPECT_NE(outcome.Refusal.find("no paths to drop"), std::string::npos) << outcome.Refusal;
    EXPECT_TRUE(target->Dropped.empty());
}
