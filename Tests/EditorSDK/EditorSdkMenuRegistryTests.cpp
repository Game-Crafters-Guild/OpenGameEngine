// EditorMenuRegistry semantics — the contracts native menu-contributing
// Editor-kind package modules rely on:
//   * RegisterMenuItem REPLACES a same-Path registration (replace-forward
//     module hot-reload) and the command id stays STABLE across the replace
//     (an un-rebuilt toolbar and a rebuilt one dispatch the same item),
//   * command ids live in the native range, disjoint from built-in and
//     script commands, and are 16-bit (Win32 WM_COMMAND carries LOWORD),
//   * TryInvoke dispatches the registered action and rejects unknown ids,
//   * registration flips the dirty flag exactly once per consume (the editor
//     polls it to rebuild the toolbar after packages load at project open),
//   * items without an Action, without a Path, or without at least
//     "Menu/Item" are rejected loudly,
//   * snapshot ordering is (Priority, Path),
//   * module-stamped registrations pin the module (C12 unload refusal),
//   * folder context-menu items dispatch with their folder, in a range of
//     their own.
//
// Links EditorSDK.dll — the same registry Editor.exe and module DLLs share.
// The registry is process-wide: every test uses its own paths.

#include "ECS/ModuleRegistration.h"
#include "Editor/Registries/EditorMenuRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;
namespace ecs = GameEngine::ECS;

namespace
{

const ed::EditorMenuItemSnapshot* FindByPath(const std::vector<ed::EditorMenuItemSnapshot>& items,
                                             std::string_view path)
{
    const auto it = std::find_if(items.begin(), items.end(),
                                 [&](const ed::EditorMenuItemSnapshot& i) { return i.Path == path; });
    return it == items.end() ? nullptr : &*it;
}

} // namespace

TEST(EditorMenuRegistry, SamePathRegistrationReplacesAndKeepsCommandIdStable)
{
    auto& registry = ed::EditorMenuRegistry::Get();

    int firstCalls = 0;
    int secondCalls = 0;
    registry.RegisterMenuItem({"Tools/SdkTest Replace", 0, [&] { ++firstCalls; }});
    const auto beforeReplace = registry.Snapshot();
    const auto* first = FindByPath(beforeReplace, "Tools/SdkTest Replace");
    ASSERT_NE(first, nullptr);
    const uint32_t id = first->CommandId;

    registry.RegisterMenuItem({"Tools/SdkTest Replace", 0, [&] { ++secondCalls; }});
    const auto snapshot = registry.Snapshot();
    EXPECT_EQ(std::count_if(snapshot.begin(), snapshot.end(),
                            [](const ed::EditorMenuItemSnapshot& i) {
                                return i.Path == "Tools/SdkTest Replace";
                            }),
              1)
        << "same-path re-registration must not grow the registry";
    const auto* second = FindByPath(snapshot, "Tools/SdkTest Replace");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->CommandId, id) << "the command id must survive replace-forward";

    EXPECT_TRUE(registry.TryInvoke(id));
    EXPECT_EQ(firstCalls, 0) << "the stale action must stop being dispatched";
    EXPECT_EQ(secondCalls, 1) << "the replacement action must win";
}

TEST(EditorMenuRegistry, CommandIdsAreSixteenBitAndInTheNativeRange)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    registry.RegisterMenuItem({"Tools/SdkTest Range", 0, [] {}});
    const auto snapshot = registry.Snapshot();
    const auto* item = FindByPath(snapshot, "Tools/SdkTest Range");
    ASSERT_NE(item, nullptr);
    // 0x6000-0x6FFF: clear of built-in editor commands (0x1000/0x2000
    // utilities, 0x3000 panel openers), context-menu/UI-replay ids (0x4000-0x5Bxx), and
    // script items (0xA000-0xEFFF).
    EXPECT_GE(item->CommandId, 0x6000u);
    EXPECT_LE(item->CommandId, 0x6FFFu);
}

TEST(EditorMenuRegistry, TryInvokeRejectsUnknownIds)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    EXPECT_FALSE(registry.TryInvoke(0x0001u));
    EXPECT_FALSE(registry.TryInvoke(0xA000u)) << "script-range ids are not native items";
}

TEST(EditorMenuRegistry, RegistrationMarksDirtyOncePerConsume)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    (void)registry.ConsumeDirty(); // drain other tests' registrations

    registry.RegisterMenuItem({"Tools/SdkTest Dirty", 0, [] {}});
    EXPECT_TRUE(registry.ConsumeDirty());
    EXPECT_FALSE(registry.ConsumeDirty()) << "consume must clear the flag";
}

TEST(EditorMenuRegistry, RejectsInvalidDescriptors)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    const size_t before = registry.Snapshot().size();

    registry.RegisterMenuItem({"", 0, [] {}});
    registry.RegisterMenuItem({"Tools/SdkTest NoAction", 0, nullptr});
    registry.RegisterMenuItem({"NoMenuSeparator", 0, [] {}});

    EXPECT_EQ(registry.Snapshot().size(), before)
        << "invalid descriptors must not enter the registry";
}

TEST(EditorMenuRegistry, SnapshotOrdersByPriorityThenPath)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    registry.RegisterMenuItem({"Tools/SdkTest Order B", 5, [] {}});
    registry.RegisterMenuItem({"Tools/SdkTest Order A", 5, [] {}});
    registry.RegisterMenuItem({"Tools/SdkTest Order C", -5, [] {}});

    const auto snapshot = registry.Snapshot();
    const auto index = [&](std::string_view path) {
        const auto it = std::find_if(snapshot.begin(), snapshot.end(),
                                     [&](const ed::EditorMenuItemSnapshot& i) { return i.Path == path; });
        return static_cast<size_t>(it - snapshot.begin());
    };
    EXPECT_LT(index("Tools/SdkTest Order C"), index("Tools/SdkTest Order A"));
    EXPECT_LT(index("Tools/SdkTest Order A"), index("Tools/SdkTest Order B"));
}

TEST(EditorMenuRegistry, ModuleStampedItemsPinTheModule)
{
    auto& registry = ed::EditorMenuRegistry::Get();

    ecs::SetActiveRegistrationModule("sdkTest.menu.module", 7);
    registry.RegisterMenuItem({"Tools/SdkTest Pin", 0, [] {}});
    ecs::ClearActiveRegistrationModule();

    std::vector<std::string> pins;
    registry.AppendModulePins("sdkTest.menu.module", pins);
    ASSERT_EQ(pins.size(), 1u);
    EXPECT_NE(pins[0].find("Tools/SdkTest Pin"), std::string::npos);

    pins.clear();
    registry.AppendModulePins("sdkTest.menu.otherModule", pins);
    EXPECT_TRUE(pins.empty());
}

TEST(EditorMenuRegistry, FixedIdCommandsDispatchAndReportFailure)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    constexpr uint32_t kSucceeds = 0xF0FE0001u;
    constexpr uint32_t kFails = 0xF0FE0002u;

    int calls = 0;
    registry.RegisterCommand({kSucceeds, [&](std::string*) { ++calls; return true; }});
    registry.RegisterCommand({kFails, [](std::string* outError) {
                                  if (outError)
                                      *outError = "sdkTest failure";
                                  return false;
                              }});

    std::string error;
    EXPECT_EQ(registry.InvokeCommand(kSucceeds, &error), ed::EditorCommandResult::Succeeded);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(registry.InvokeCommand(kFails, &error), ed::EditorCommandResult::Failed);
    EXPECT_EQ(error, "sdkTest failure");

    error = "untouched";
    EXPECT_EQ(registry.InvokeCommand(0xF0FE00FFu, &error), ed::EditorCommandResult::NotFound);
    EXPECT_EQ(error, "untouched") << "an unknown id must leave the caller's error alone";
    EXPECT_FALSE(registry.TryInvoke(kSucceeds)) << "fixed-id commands are not menu items";
}

TEST(EditorMenuRegistry, FixedIdCommandReRegistrationReplacesForward)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    constexpr uint32_t kId = 0xF0FE0003u;

    int firstCalls = 0;
    int secondCalls = 0;
    registry.RegisterCommand({kId, [&](std::string*) { ++firstCalls; return true; }});
    registry.RegisterCommand({kId, [&](std::string*) { ++secondCalls; return true; }});

    EXPECT_EQ(registry.InvokeCommand(kId, nullptr), ed::EditorCommandResult::Succeeded);
    EXPECT_EQ(firstCalls, 0);
    EXPECT_EQ(secondCalls, 1);
}

TEST(EditorMenuRegistry, RejectsFixedIdCommandsInTheNativeMenuRange)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    registry.RegisterCommand({0x6001u, [](std::string*) { return true; }});
    registry.RegisterCommand({0xF0FE0004u, nullptr});

    EXPECT_EQ(registry.InvokeCommand(0x6001u, nullptr), ed::EditorCommandResult::NotFound)
        << "the native menu range belongs to registry-assigned menu items";
    EXPECT_EQ(registry.InvokeCommand(0xF0FE0004u, nullptr), ed::EditorCommandResult::NotFound);
}

// A folder context-menu item dispatches with the folder the menu was opened on,
// keeps its command id across a same-Path re-registration, and takes ids no
// toolbar item or fixed-id command can.
TEST(EditorMenuRegistry, FolderMenuItemsDispatchTheFolderAndKeepTheirId)
{
    auto& registry = ed::EditorMenuRegistry::Get();
    // The registry outlives the test, so the actions own what they write.
    const auto received = std::make_shared<std::filesystem::path>();
    registry.RegisterDirectoryMenuItem({"SdkTest Folder Item", 0, {}, [received](const std::filesystem::path& folder) {
                                            *received = folder;
                                        }});
    const auto findItem = [&] {
        const auto items = registry.DirectoryMenuSnapshot();
        const auto it = std::find_if(items.begin(), items.end(), [](const ed::EditorDirectoryMenuItemSnapshot& i) {
            return i.Path == "SdkTest Folder Item";
        });
        return it == items.end() ? 0u : it->CommandId;
    };
    const uint32_t id = findItem();
    ASSERT_NE(id, 0u);
    for (const auto& toolbarItem : registry.Snapshot())
        EXPECT_NE(toolbarItem.CommandId, id);

    EXPECT_TRUE(registry.TryInvokeDirectoryItem(id, "Assets/Props"));
    EXPECT_EQ(*received, std::filesystem::path("Assets/Props"));
    EXPECT_FALSE(registry.TryInvokeDirectoryItem(id + 0x100u, "Assets/Props"));

    const auto replaced = std::make_shared<bool>(false);
    registry.RegisterDirectoryMenuItem(
        {"SdkTest Folder Item", 0, {}, [replaced](const std::filesystem::path&) { *replaced = true; }});
    EXPECT_EQ(findItem(), id);
    EXPECT_TRUE(registry.TryInvokeDirectoryItem(id, "Assets"));
    EXPECT_TRUE(*replaced);

    std::string error;
    registry.RegisterCommand({id, [](std::string*) { return true; }});
    EXPECT_EQ(registry.InvokeCommand(id, &error), ed::EditorCommandResult::NotFound)
        << "a fixed-id command may not take a folder item's id";
}
