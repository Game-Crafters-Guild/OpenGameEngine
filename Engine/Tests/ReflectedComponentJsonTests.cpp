// The read half of the debug server's component vocabulary: what a reflected component looks
// like when it is dumped by field name, driven by the RUNTIME field tables the engine actually
// registers rather than a table built for the test.
//
// These run against the real ComponentFieldRegistry, which is why they live in a suite that
// links Engine — the generated reflection TU's static initializers are what populate it. The
// same tests in a suite without Engine would sweep an empty registry and pass having examined
// nothing, so each one states the population it needs before asserting anything about it.
//
// They assert on the emitted TEXT rather than a parsed tree, because the wire form is the thing
// under test: "" and {"size","data"} are chosen for what a WRITER accepts back, and a parse
// would erase exactly the distinctions that matters.
//
// What they do NOT cover: the write half. set_component lives in a translation unit no test
// binary compiles, so read -> write -> ok is a live IPC probe, not a unit test. These pin the
// half that decides whether such a round-trip is expressible at all.

#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Reflection.h"
#include "ECS/ReflectionJson.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{

// Every component type carrying a reflected field table, resolved through the name registry
// (the field registry has no enumeration of its own).
std::vector<ECS::ComponentTypeId> ReflectedComponentTypeIds()
{
    std::vector<ECS::ComponentTypeId> ids;
    for (const std::string& name : ECS::ComponentRegistry::GetAllComponentNames())
        if (const ECS::ComponentTypeId id = ECS::ComponentFieldRegistry::FindByName(name); id != 0)
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
    return ids;
}

std::string DumpComponent(ECS::ComponentTypeId typeId, const void* bytes)
{
    std::string text;
    ECS::AppendComponentJson(text, ECS::ComponentFieldRegistry::Get(typeId),
                             reinterpret_cast<const std::byte*>(bytes));
    return text;
}

bool IsLowerEqual(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto la = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto lb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (la != lb)
            return false;
    }
    return true;
}

} // namespace

// An unset AssetRef<> used to dump as 32 zero hex digits, which is not a value any writer reads
// back: it parses as neither a GUID nor an asset path, so feeding a DEFAULT component's own read
// output into set_component failed on every asset field it had. "" is what the hand-written
// asset-ref serializers already emit for unset, and it is the form the writer clears on.
TEST(ReflectedComponentJson, UnsetAssetRefEmitsEmptyStringNotZeroGuid)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("TerrainGrass");
    ASSERT_NE(typeId, 0u) << "TerrainGrass is not reflected in this binary — the registry is not "
                             "populated and this test would assert nothing.";

    const Components::TerrainGrass grass{};
    const std::string dumped = DumpComponent(typeId, &grass);

    for (const char* field :
         {"AlbedoTextureAssetGuid", "AlphaTextureAssetGuid", "NormalTextureAssetGuid"})
    {
        const std::string expected = std::string("\"") + field + "\":\"\"";
        EXPECT_NE(dumped.find(expected), std::string::npos)
            << field << " did not dump as \"\"; a default component's read output cannot be "
                        "written back. Dump was: "
            << dumped;
    }
    EXPECT_EQ(dumped.find("00000000000000000000000000000000"), std::string::npos)
        << "a zero GUID still reaches the wire";
}

// A SET reference still dumps as hex, so the "" rule is about the unset case only and does not
// quietly swallow real references.
TEST(ReflectedComponentJson, SetAssetRefStillEmitsItsGuid)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("TerrainGrass");
    ASSERT_NE(typeId, 0u);

    Components::TerrainGrass grass{};
    grass.AlbedoTextureAssetGuid.Set(GUID("1a2b3c4d-5e6f-7081-9203-a4b5c6d7e8f9"));
    const std::string dumped = DumpComponent(typeId, &grass);

    EXPECT_NE(dumped.find("\"AlbedoTextureAssetGuid\":\"1a2b3c4d5e6f70819203a4b5c6d7e8f9\""),
              std::string::npos)
        << "dump was: " << dumped;
    // Its two unset siblings are unaffected.
    EXPECT_NE(dumped.find("\"AlphaTextureAssetGuid\":\"\""), std::string::npos);
}

// The enumerator name is the form the scene format uses and the form the writer resolves, so it
// is what a read has to emit for the two directions to share one vocabulary.
TEST(ReflectedComponentJson, EnumFieldEmitsItsEnumeratorName)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("TerrainGrass");
    ASSERT_NE(typeId, 0u);

    Components::TerrainGrass grass{};
    grass.RenderMode = Components::TerrainGrassRenderMode::Dither;
    EXPECT_NE(DumpComponent(typeId, &grass).find("\"RenderMode\":\"Dither\""), std::string::npos);

    grass.RenderMode = Components::TerrainGrassRenderMode::Blend;
    EXPECT_NE(DumpComponent(typeId, &grass).find("\"RenderMode\":\"Blend\""), std::string::npos);
}

// A field whose type has no JSON form dumps as {"size", "data"}, not a bare hex string. At 16
// bytes a bare hex string is indistinguishable from a GUID, and a 32-hex-character one is
// indistinguishable from the GUID form this same dump emits for asset refs.
TEST(ReflectedComponentJson, OpaqueFieldSelfDescribes)
{
    const std::vector<std::byte> zeros(8192, std::byte{0});
    std::size_t opaqueFieldsSeen = 0;

    for (const ECS::ComponentTypeId typeId : ReflectedComponentTypeIds())
    {
        const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
        const bool fits = std::all_of(fields.begin(), fields.end(), [&](const ECS::FieldInfo& f) {
            return static_cast<std::size_t>(f.Offset) + f.Size <= zeros.size();
        });
        if (!fits)
            continue;

        std::string dumped;
        for (const ECS::FieldInfo& f : fields)
        {
            // Opaque == no element size. String has one (1) and is emitted as text, so it is
            // already excluded here rather than by name.
            if (ECS::FieldElementSize(f.Type) != 0)
                continue;
            if (dumped.empty())
                dumped = DumpComponent(typeId, zeros.data());
            ++opaqueFieldsSeen;

            const std::string expected = "\"" + std::string(f.Name) +
                                         "\":{\"size\":" + std::to_string(f.Size) + ",\"data\":\"";
            EXPECT_NE(dumped.find(expected), std::string::npos)
                << ECS::ComponentFieldRegistry::GetCanonicalName(typeId) << "." << f.Name
                << " did not dump as a self-describing byte blob";
        }
    }

    EXPECT_GT(opaqueFieldsSeen, 0u)
        << "no reflected component in this binary has an opaque field — this test asserted "
           "nothing. Re-point it before trusting it.";
}

// The precondition the case-insensitive enum matcher rests on. Matching "blend" to Blend is only
// unambiguous while no enum has two enumerators differing by case alone; if one ever does, the
// matcher silently resolves to whichever the table lists first.
TEST(ReflectedEnumNameDrift, NoReflectedEnumHasCaseCollidingNames)
{
    std::size_t enumFieldsSeen = 0;

    for (const ECS::ComponentTypeId typeId : ReflectedComponentTypeIds())
    {
        for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(typeId))
        {
            if (f.EnumNames.empty())
                continue;
            ++enumFieldsSeen;

            for (std::size_t i = 0; i < f.EnumNames.size(); ++i)
            {
                for (std::size_t j = i + 1; j < f.EnumNames.size(); ++j)
                {
                    // Two spellings of ONE value are aliases, not an ambiguity: either resolves
                    // to the same integer, so the matcher cannot pick wrong.
                    if (f.EnumNames[i].Value == f.EnumNames[j].Value)
                        continue;
                    EXPECT_FALSE(IsLowerEqual(f.EnumNames[i].Name, f.EnumNames[j].Name))
                        << ECS::ComponentFieldRegistry::GetCanonicalName(typeId) << "." << f.Name
                        << " has enumerators '" << f.EnumNames[i].Name << "' and '"
                        << f.EnumNames[j].Name
                        << "' differing only by case — the case-insensitive enum match in "
                           "set_component cannot tell them apart.";
                }
            }
        }
    }

    EXPECT_GT(enumFieldsSeen, 20u)
        << "far fewer reflected enum fields than this engine has — the registry is under-"
           "populated and this sweep would pass without examining anything.";
}

} // namespace GameEngine
