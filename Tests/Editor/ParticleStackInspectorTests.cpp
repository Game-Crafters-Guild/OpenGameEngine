// The particle stack asset inspector with a processor type registered from outside the particle
// module: its section, its parameter rows and its entry in the Add Processor picker all come from
// the descriptor it registered, and editing a row saves the stack file with one undo step.

#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include <gtest/gtest.h>

#include "ECS/Reflection.h"
#include "Input/KeyCodes.h"
#include "InspectorRegistry.h"
#include "Inspectors/Particles/ParticleProcessorSearchProvider.h"
#include "Inspectors/Particles/ParticleStackEditor.h"
#include "Inspectors/Particles/ParticleStackInspector.h"
#include "Inspectors/Particles/ParticleStackReference.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/ParticleStackAuthoring.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <any>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace ParticleStackInspectorTestTypes
{
struct WindParameters
{
    float Strength = 1.0f;
};
} // namespace ParticleStackInspectorTestTypes

GE_REFLECT(ParticleStackInspectorTestTypes::WindParameters, Strength);

using namespace GameEngine;
using namespace GameEngine::Particles;

namespace
{
constexpr int kControlModifier = 0x0002;

constexpr ParticleParameterField kWindFields[] = {
    {.Name = "Strength", .Label = "Wind Strength", .Tooltip = "Units per second squared", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
};

void ExecuteWind(ParticleProcessorContext& context)
{
    const float push = context.Params<ParticleStackInspectorTestTypes::WindParameters>().Strength * context.DeltaTime;
    auto velocities = context.Channels.Velocities();
    for (const uint32 index : context.Particles)
        velocities[index].x += push;
}

void RegisterWindProcessor()
{
    if (ParticleProcessorRegistry::Find("test.wind"))
        return;
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "test.wind";
    descriptor.DisplayName = "Test Wind";
    descriptor.Description = "Pushes particles along X";
    descriptor.IconClass = "particle-choice-noise";
    descriptor.Category = "Tests";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Reads = ChannelBits(ParticleChannel::Velocity);
    descriptor.Writes = ChannelBits(ParticleChannel::Velocity);
    descriptor.Execute = ExecuteWind;
    BindParticleParameters<ParticleStackInspectorTestTypes::WindParameters>(descriptor, kWindFields);
    ParticleProcessorRegistry::Register(descriptor);
}

template <typename T>
T* FindUnder(UIElement& node, bool (*matches)(const T&, const std::string&), const std::string& text)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* typed = dynamic_cast<T*>(child.get()); typed && matches(*typed, text))
            return typed;
        if (T* found = FindUnder<T>(*child, matches, text))
            return found;
    }
    return nullptr;
}

bool HasTitle(const Foldout& section, const std::string& title)
{
    return section.GetTitle() == title;
}

bool HasText(const Label& label, const std::string& text)
{
    return label.GetText() == text;
}

bool IsAny(const FloatField&, const std::string&)
{
    return true;
}

bool IsAnyInput(const TextInput&, const std::string&)
{
    return true;
}

// The float field of the row `rowLabel` names, under `section`.
FloatField* RowField(Foldout& section, const std::string& rowLabel)
{
    Label* label = FindUnder<Label>(section, HasText, rowLabel);
    for (UIElement* around = label ? label->GetParent() : nullptr; around && around != &section;
         around = around->GetParent())
        if (FloatField* field = FindUnder<FloatField>(*around, IsAny, {}))
            return field;
    return nullptr;
}

// The wind strength the stack file holds.
float StoredStrength(const std::filesystem::path& file)
{
    ParticleStackAsset stored(GUID::Generate(), file);
    if (!stored.Load())
        return -1.0f;
    const auto* wind = FindProcessor(*stored.Document(), 3);
    return wind ? wind->Params<ParticleStackInspectorTestTypes::WindParameters>().Strength : -1.0f;
}

void CollectResults(std::vector<SearchResultItem>& into, std::vector<SearchResultItem> results, bool)
{
    into = std::move(results);
}

void SetGustToFour(StackDocument& document)
{
    FindProcessor(document, 3)->Params<ParticleStackInspectorTestTypes::WindParameters>().Strength = 4.0f;
}
} // namespace

TEST(ParticleStackInspector, AProcessorRegisteredOutsideTheModuleGetsItsSectionRowsAndPickerEntry)
{
    RegisterWindProcessor();
    RegisterParticleStackInspector();

    const char* source =
        R"({"version":1,"entryPhase":1,"lifetime":2,"phases":[{"id":1,"label":"Spawn","processors":[)"
        R"({"type":"emitRate","id":2,"stage":"emission"},)"
        R"({"type":"test.wind","id":3,"label":"Gust","stage":"update","parameters":{"strength":2.5}}]}]})";
    const auto file = std::filesystem::temp_directory_path() / "ge_particle_stack_inspector.particlestack";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << source;
    }
    auto asset = std::make_shared<ParticleStackAsset>(GUID::Generate(), file);
    ASSERT_TRUE(asset->Load()) << asset->Diagnostics().front().Message;

    Editor::UndoRedoService undo;
    UIElement root;
    InspectorContext context{};
    context.Parent = &root;
    context.Object = asset.get();
    context.Undo = &undo;
    auto* inspector = InspectorRegistry::Get().TryGetAssetInspector(AssetType::ParticleStack);
    ASSERT_NE(inspector, nullptr);
    (*inspector)(context);

    Foldout* section = FindUnder<Foldout>(root, HasTitle, "Update \xC2\xB7 Gust");
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(section->GetIconClass(), "particle-choice-noise");
    FloatField* strength = RowField(*section, "Wind Strength");
    ASSERT_NE(strength, nullptr);
    EXPECT_FLOAT_EQ(strength->GetValue(), 2.5f);

    TextInput* editor = FindUnder<TextInput>(*strength, IsAnyInput, {});
    ASSERT_NE(editor, nullptr);
    strength->OnFocusChanged(true);
    editor->OnKey(Input::kKeyCode_A, kControlModifier, nullptr);
    editor->OnChar(static_cast<unsigned char>('4'));
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_FLOAT_EQ(StoredStrength(file), 4.0f);
    EXPECT_FLOAT_EQ(FindProcessor(*asset->Document(), 3)->Params<ParticleStackInspectorTestTypes::WindParameters>().Strength,
                    4.0f);
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    undo.Undo();
    EXPECT_FLOAT_EQ(StoredStrength(file), 2.5f);
    EXPECT_FLOAT_EQ(FindProcessor(*asset->Document(), 3)->Params<ParticleStackInspectorTestTypes::WindParameters>().Strength,
                    2.5f);

    ParticleInspectors::ParticleProcessorSearchProvider picker;
    std::vector<SearchResultItem> results;
    picker.BeginSearch("wind", [&results](std::vector<SearchResultItem> found, bool complete)
                       { CollectResults(results, std::move(found), complete); });
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().Label, "Test Wind");
    EXPECT_EQ(results.front().Detail, "Tests");
    EXPECT_EQ(results.front().Icon.CssClass, "particle-choice-noise");
    const auto* type = std::any_cast<std::string>(&results.front().UserData);
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(*type, "test.wind");
    std::filesystem::remove(file);
}

// A colour's channel rows sit under its swatch row. Committing a channel asks the inspector to rebuild,
// and the rebuilt swatch row prints the colour the file now stores, not the one it was built with.
TEST(ParticleStackInspector, AColourChannelEditShowsTheStoredColourBesideItsSwatch)
{
    RegisterParticleStackInspector();
    const char* source =
        R"({"version":1,"entryPhase":1,"lifetime":2,"phases":[{"id":1,"label":"Spawn","processors":[)"
        R"({"type":"emitRate","id":2,"stage":"emission"},)"
        R"({"type":"property","id":3,"label":"Tint","stage":"birth","parameters":{"target":"color","operation":"set",)"
        R"("value":[{"mode":"constant","value":0.55},{"mode":"constant","value":0.55},)"
        R"({"mode":"constant","value":0.55},{"mode":"constant","value":1}]}}]}]})";
    const auto file = std::filesystem::temp_directory_path() / "ge_particle_stack_swatch.particlestack";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << source;
    }
    auto asset = std::make_shared<ParticleStackAsset>(GUID::Generate(), file);
    ASSERT_TRUE(asset->Load()) << asset->Diagnostics().front().Message;

    Editor::UndoRedoService undo;
    UIElement root;
    int refreshes = 0;
    InspectorContext context{};
    context.Parent = &root;
    context.Object = asset.get();
    context.Undo = &undo;
    context.RequestInspectorRefresh = [&refreshes] { ++refreshes; };
    auto* inspector = InspectorRegistry::Get().TryGetAssetInspector(AssetType::ParticleStack);
    ASSERT_NE(inspector, nullptr);
    (*inspector)(context);

    const std::string before = "(0.55, 0.55, 0.55) \xC2\xB7 Alpha 1.00";
    ASSERT_NE(FindUnder<Label>(root, HasText, before), nullptr);
    Foldout* section = FindUnder<Foldout>(root, HasTitle, "Birth \xC2\xB7 Tint");
    ASSERT_NE(section, nullptr);
    FloatField* red = RowField(*section, "Value Red");
    ASSERT_NE(red, nullptr);
    TextInput* editor = FindUnder<TextInput>(*red, IsAnyInput, {});
    ASSERT_NE(editor, nullptr);
    red->OnFocusChanged(true);
    editor->OnKey(Input::kKeyCode_A, kControlModifier, nullptr);
    editor->OnChar(static_cast<unsigned char>('0'));
    editor->OnChar(static_cast<unsigned char>('.'));
    editor->OnChar(static_cast<unsigned char>('5'));
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(refreshes, 1) << "a committed colour channel asks for the rebuild that redraws the swatch row";

    root.RemoveAllChildren();
    (*inspector)(context);
    EXPECT_NE(FindUnder<Label>(root, HasText, "(0.50, 0.55, 0.55) \xC2\xB7 Alpha 1.00"), nullptr);
    EXPECT_EQ(FindUnder<Label>(root, HasText, before), nullptr);
    std::filesystem::remove(file);
}

// The emitter inspector says why an emitter emits nothing: a reference no asset carries is missing,
// not loading forever.
TEST(ParticleStackInspector, AStackReferenceNoAssetCarriesIsMissing)
{
    AssetManager assets;
    EXPECT_EQ(ParticleInspectors::ResolveParticleStackReference(assets, GUID{}), ParticleInspectors::ParticleStackReference::Default);
    const GUID missing = GUID::Generate();
    EXPECT_EQ(ParticleInspectors::ResolveParticleStackReference(assets, missing), ParticleInspectors::ParticleStackReference::Missing);

    const GUID loaded = GUID::Generate();
    auto asset = std::make_shared<ParticleStackAsset>(loaded, "loaded.particlestack");
    const std::string json = R"({"version":1,"entryPhase":1,"lifetime":1,"phases":[{"id":1,"label":"Spawn","processors":[]}]})";
    ASSERT_TRUE(asset->LoadFromData(Vector<uint8>(json.begin(), json.end())));
    assets.RegisterLoadedAsset(loaded, asset);
    EXPECT_EQ(ParticleInspectors::ResolveParticleStackReference(assets, loaded), ParticleInspectors::ParticleStackReference::Loaded);

    const GUID other = GUID::Generate();
    assets.RegisterLoadedAsset(other, std::make_shared<BinaryAsset>(other, "other.bin", AssetType::Unknown));
    EXPECT_EQ(ParticleInspectors::ResolveParticleStackReference(assets, other),
              ParticleInspectors::ParticleStackReference::NotAStack)
        << "a loaded asset of another type is not a stack still loading";
}

// An undo entry outlives the stack object it edited: the asset manager may release it and load the
// file again as a new object. Undo restores the file and reloads the object loaded now.
TEST(ParticleStackInspector, AnUndoOutlivesTheStackObjectItEdited)
{
    RegisterWindProcessor();
    const char* source =
        R"({"version":1,"entryPhase":1,"lifetime":2,"phases":[{"id":1,"label":"Spawn","processors":[)"
        R"({"type":"emitRate","id":2,"stage":"emission"},)"
        R"({"type":"test.wind","id":3,"label":"Gust","stage":"update","parameters":{"strength":2.5}}]}]})";
    const auto file = std::filesystem::temp_directory_path() / "ge_particle_stack_undo.particlestack";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << source;
    }
    AssetManager assets;
    const GUID guid = GUID::Generate();
    auto edited = std::make_shared<ParticleStackAsset>(guid, file);
    ASSERT_TRUE(edited->Load());
    assets.RegisterLoadedAsset(guid, edited);

    Editor::UndoRedoService undo;
    {
        ParticleInspectors::ParticleStackEditor editor(*edited, &assets, &undo, {});
        editor.Commit("Set Gust", SetGustToFour, false);
    }
    ASSERT_FLOAT_EQ(StoredStrength(file), 4.0f);
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    assets.UnregisterLoadedAsset(guid);
    edited.reset();
    auto reloaded = std::make_shared<ParticleStackAsset>(guid, file);
    ASSERT_TRUE(reloaded->Load());
    assets.RegisterLoadedAsset(guid, reloaded);

    undo.Undo();
    EXPECT_FLOAT_EQ(StoredStrength(file), 2.5f);
    EXPECT_FLOAT_EQ(FindProcessor(*reloaded->Document(), 3)->Params<ParticleStackInspectorTestTypes::WindParameters>().Strength,
                    2.5f);
    std::filesystem::remove(file);
}
