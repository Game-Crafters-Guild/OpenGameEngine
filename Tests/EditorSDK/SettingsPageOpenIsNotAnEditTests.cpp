#include <gtest/gtest.h>

#include "Core/Engine.h"
#include "Editor/Settings/DynamicResolutionProjectSettings.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/GameUIScaleSettingsPage.h"
#include "Editor/Settings/LodSettingsPage.h"
#include "Editor/Settings/SettingsStore.h"

#include "../TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

namespace
{
using namespace GameEngine;
namespace fs = std::filesystem;

// Opening a settings page must be byte-neutral to the project file.
//
// The shared row builders in SettingsPanel.cpp seed each row from stored data
// and then hand that same value straight to the row's change callback, so
// building a page runs every field's Set against that field's own Get. Seeding
// a widget is not an edit: it must not rewrite the file, must not materialize
// engine defaults into it, and must not bump its mtime -- a settings write
// wakes the file watcher and dirties version control.
//
// Reproduced against the editor before this suite existed: opening
// Project Settings -> Scene View on a project holding rendering.drsTargetFps
// 200 rewrote it to 144 (the widget's old max), invented rendering.
// taaRenderScale, and reordered every key.

// Literal fixtures rather than values built from the implementation's
// constants: a fixture that derives its expectations from the code under test
// cannot fail when that code changes its mind.
constexpr const char* kSeedWithLodBlock = R"({
  "schemaVersion": 1,
  "rendering": {
    "lodMode": "sse",
    "lodErrorBudgetPx": 3.5,
    "lodSkinnedBudgetScale": 0.75,
    "lodGameViewBudgetEnabled": true,
    "lodGameViewBudgetPercent": 80.0,
    "lodSceneViewBudgetEnabled": false,
    "lodSceneViewBudgetPercent": 120.0,
    "lodCrossfadeDuration": 0.25,
    "lodHysteresisBand": 0.2
  },
  "zzzHandEditedSibling": "must-survive"
})";

// A project that chose a reference-size policy: the Game UI Scaling sliders
// seed from these and must hand them back without a write.
constexpr const char* kSeedWithUIScaleBlock = R"({
  "schemaVersion": 1,
  "uiScale": {
    "mode": "fit",
    "referenceWidth": 1600.0,
    "referenceHeight": 800.0
  },
  "zzzHandEditedSibling": "must-survive"
})";

// The common case, and the one that exposes default materialization: every Get
// returns an engine default, and a Set that writes unconditionally invents a
// block the user never authored.
constexpr const char* kSeedWithoutLodBlock = R"({
  "schemaVersion": 1,
  "rendering": {
    "msaa": "off"
  },
  "zzzHandEditedSibling": "must-survive"
})";

struct FileSnapshot
{
    bool Exists = false;
    std::string Bytes;
    fs::file_time_type Mtime{};
};

FileSnapshot Snapshot(const fs::path& path)
{
    FileSnapshot snapshot;
    std::error_code ec;
    snapshot.Exists = fs::exists(path, ec);
    if (!snapshot.Exists)
        return snapshot;

    std::ifstream in(path, std::ios::binary);
    snapshot.Bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    snapshot.Mtime = fs::last_write_time(path, ec);
    return snapshot;
}

class SettingsPageOpenIsNotAnEditTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_WorkspaceRoot = TestUtils::MakeUniqueTempDirectory("ge_settings_page_open");
        fs::create_directories(m_WorkspaceRoot / ".Editor");
        EngineCore::GetInstance().SetWorkspaceRoot(m_WorkspaceRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_WorkspaceRoot, ec);
    }

    fs::path SettingsFile() const { return m_WorkspaceRoot / ".Editor" / "ProjectSettings.json"; }

    void WriteSettings(const char* json) const
    {
        std::ofstream out(SettingsFile(), std::ios::binary | std::ios::trunc);
        out << json;
    }

    // What CreateSettingsSliderRow / CreateSettingsToggleRow do when a page is
    // built: read the stored value, clamp it to the control's range, hand it
    // back to the change callback. The clamp is part of the reproduction --
    // it is how a widget narrower than its backing store turns a seed into a
    // value the file never held. Dropdown rows install their callback after
    // seeding the control and so never fire here.
    static void SeedEveryFieldOf(const Editor::SettingsCategoryDescriptor& category)
    {
        using Editor::SettingsFieldDescriptor;
        for (const SettingsFieldDescriptor& field : category.Fields)
        {
            if (const auto* toggle = std::get_if<SettingsFieldDescriptor::ToggleField>(&field.Control))
            {
                if (toggle->Get && toggle->Set)
                    toggle->Set(toggle->Get());
            }
            else if (const auto* slider = std::get_if<SettingsFieldDescriptor::SliderField>(&field.Control))
            {
                if (slider->Get && slider->Set)
                    slider->Set(std::clamp(slider->Get(), slider->MinValue, slider->MaxValue));
            }
        }
    }

    fs::path m_WorkspaceRoot;
};

// The instrument: prove the snapshot comparison can actually see a write.
// Without this, every "unchanged" assertion below could be passing vacuously.
TEST_F(SettingsPageOpenIsNotAnEditTest, SnapshotDetectsAGenuineWrite)
{
    WriteSettings(kSeedWithoutLodBlock);
    const FileSnapshot before = Snapshot(SettingsFile());

    Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
    std::string err;
    ASSERT_TRUE(store.Load(&err)) << err;
    store.Json()["rendering"]["msaa"] = "4";
    ASSERT_TRUE(store.Save(&err)) << err;

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_NE(before.Bytes, after.Bytes);
}

TEST_F(SettingsPageOpenIsNotAnEditTest, StoreSaveOfUnchangedContentLeavesFileUntouched)
{
    WriteSettings(kSeedWithLodBlock);

    // Normalize first: the store rewrites formatting on its first real save, so
    // the second save is the one that must be a no-op.
    {
        Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
        std::string err;
        ASSERT_TRUE(store.Load(&err)) << err;
        ASSERT_TRUE(store.Save(&err)) << err;
    }

    const FileSnapshot before = Snapshot(SettingsFile());

    Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
    std::string err;
    ASSERT_TRUE(store.Load(&err)) << err;
    ASSERT_TRUE(store.Save(&err)) << err;

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_EQ(before.Bytes, after.Bytes);
    EXPECT_EQ(before.Mtime, after.Mtime) << "an unchanged save must not touch the file";
}

TEST_F(SettingsPageOpenIsNotAnEditTest, SeedingRegisteredProjectFieldsLeavesStoredValuesUntouched)
{
    Editor::RegisterLodSettingsCategory();
    Editor::RegisterGameUIScaleSettingsCategory();
    WriteSettings(kSeedWithLodBlock);

    const FileSnapshot before = Snapshot(SettingsFile());
    ASSERT_TRUE(before.Exists);

    for (const auto& category : Editor::EditorSettingsRegistry::Get().Snapshot())
    {
        if (category.Group != Editor::SettingsCategoryGroup::ProjectSettings)
            continue;
        SeedEveryFieldOf(category);
    }

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_EQ(before.Bytes, after.Bytes)
        << "building a project-settings page rewrote " << SettingsFile().string();
    EXPECT_EQ(before.Mtime, after.Mtime);
}

TEST_F(SettingsPageOpenIsNotAnEditTest, SeedingRegisteredProjectFieldsMaterializesNoDefaults)
{
    Editor::RegisterLodSettingsCategory();
    Editor::RegisterGameUIScaleSettingsCategory();
    WriteSettings(kSeedWithoutLodBlock);

    const FileSnapshot before = Snapshot(SettingsFile());
    ASSERT_TRUE(before.Exists);

    for (const auto& category : Editor::EditorSettingsRegistry::Get().Snapshot())
    {
        if (category.Group != Editor::SettingsCategoryGroup::ProjectSettings)
            continue;
        SeedEveryFieldOf(category);
    }

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_EQ(before.Bytes, after.Bytes)
        << "building a page wrote engine defaults into a project that never set them";
    EXPECT_EQ(before.Mtime, after.Mtime);
}

TEST_F(SettingsPageOpenIsNotAnEditTest, SeedingTheGameUIScalePageLeavesAnAuthoredPolicyUntouched)
{
    Editor::RegisterGameUIScaleSettingsCategory();
    WriteSettings(kSeedWithUIScaleBlock);

    const FileSnapshot before = Snapshot(SettingsFile());
    ASSERT_TRUE(before.Exists);

    Editor::SettingsCategoryDescriptor page;
    ASSERT_TRUE(Editor::EditorSettingsRegistry::Get().TryGet("gameUiScaling", page));
    SeedEveryFieldOf(page);

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_EQ(before.Bytes, after.Bytes) << "opening Game UI Scaling rewrote the authored policy";
    EXPECT_EQ(before.Mtime, after.Mtime);
}

// The LOD page returns early when the seeded value already matches what is
// stored, which is what the registry contract asks of a persisting Set. A page
// that skips that guard must still not damage the file: the store's write path
// is the backstop. Shaped like the built-in Scene View rows -- read the value,
// hand it straight back, write unconditionally.
TEST_F(SettingsPageOpenIsNotAnEditTest, SeedingAnUnguardedPageStillLeavesTheFileUntouched)
{
    WriteSettings(kSeedWithLodBlock);

    // Normalize formatting first, as a real project file already saved once by
    // the editor would be.
    {
        Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
        std::string err;
        ASSERT_TRUE(store.Load(&err)) << err;
        ASSERT_TRUE(store.Save(&err)) << err;
    }

    const FileSnapshot before = Snapshot(SettingsFile());

    const auto readErrorBudget = [this]() -> float
    {
        Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
        std::string err;
        store.Load(&err);
        return store.Json()["rendering"]["lodErrorBudgetPx"].get<float>();
    };
    const auto writeErrorBudgetUnconditionally = [this](float value)
    {
        Editor::SettingsStore store = Editor::OpenProjectSettings(m_WorkspaceRoot);
        std::string err;
        store.Load(&err);
        store.Json()["rendering"]["lodErrorBudgetPx"] = value;
        store.Save(&err);
    };

    writeErrorBudgetUnconditionally(readErrorBudget());

    const FileSnapshot after = Snapshot(SettingsFile());
    EXPECT_EQ(before.Bytes, after.Bytes);
    EXPECT_EQ(before.Mtime, after.Mtime)
        << "an unguarded page's seed write must still not touch the file";
}

// The reported defect: the Dynamic Target FPS widget clamped to [30,144] while
// every loader accepted [15,240], so seeding the row from a stored 200 fed 144
// to the persisting callback. The widget and the loaders now share one range,
// which is what keeps a legitimately stored value representable.
TEST_F(SettingsPageOpenIsNotAnEditTest, DrsTargetFpsRangeAdmitsEveryStorableValue)
{
    EXPECT_LE(Editor::kMinDrsTargetFps, Editor::kDefaultDrsTargetFps);
    EXPECT_GE(Editor::kMaxDrsTargetFps, Editor::kDefaultDrsTargetFps);
    EXPECT_GE(Editor::kMaxDrsTargetFps, 200.0f)
        << "a target the loaders accept must survive being shown in the widget";
}

} // namespace
