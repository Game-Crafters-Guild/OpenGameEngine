// Phase 2a: GE_REFLECT field-table reflection.
//
// Exercises the macro on a comprehensive local probe struct (every FieldTypeId
// path: scalars by width, enum->underlying, C arrays, char[]->String, Color,
// EntityHandle, and a composite via the FieldTypeTraits customization point) and
// on four real engine components (Transform, Camera, Light, Parent) to prove the
// macro works on production shapes. Also pins the C-ABI mirror parity, the JSON
// dump, a field-walk snapshot round-trip, and GE_REFLECT_VERSION.

#include "ECS/Reflection.h"
#include "ECS/ReflectionJson.h"
#include "ECS/ComponentFieldRegistry.h"  // Register / Get / SetFieldFlags
#include "ECS/ECS.h"            // EntityHandle (complete) for offsetof/sizeof
#include "ECS/Entity.h"         // World + Entity for the handler-integration test
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"   // World::AddComponent<T> implementations for JsonProbe
#include "Types/Color.h"
#include <utility> // std::as_const
#include "Components/MathReflectionTraits.h"  // Vector2/3/4 + Quaternion field traits

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Hierarchy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine::ECS;
using GameEngine::ColorLinear;

// ---- Fixture types -------------------------------------------------------

enum class ProbeMode : std::uint16_t { Idle = 0, Run = 1, Halt = 2 };

// Local composite type, taught to reflection purely via the trait customization
// point (no edit to FieldTypeIdOf needed).
struct ProbeVec3 { float x; float y; float z; };

struct ReflectProbe
{
    bool          Flag;
    std::int8_t   I8;
    std::int16_t  I16;
    std::int32_t  I32;
    std::int64_t  I64;
    std::uint8_t  U8;
    std::uint16_t U16;
    std::uint32_t U32;
    std::uint64_t U64;
    float         F;
    double        D;
    ProbeMode     Mode;        // enum -> UInt16
    float         Triple[3];   // array -> Float, Size 12
    char          Label[16];   // char[] -> String
    ColorLinear   Tint;        // -> Color
    EntityHandle  Owner;       // -> EntityHandle
    ProbeVec3     Position;    // -> Vec3 (via trait)
    float         Matrix[16];  // -> Float, Size 64 (opaque large array)
};

struct VersionedProbe { std::int32_t value; };

// Teach reflection about ProbeVec3 before GE_REFLECT(ReflectProbe, ...) uses it.
template <>
struct GameEngine::ECS::FieldTypeTraits<ProbeVec3>
{
    static constexpr GameEngine::ECS::FieldTypeId kType = GameEngine::ECS::FieldTypeId::Vec3;
};

GE_REFLECT(ReflectProbe,
    Flag, I8, I16, I32, I64, U8, U16, U32, U64, F, D,
    Mode, Triple, Label, Tint, Owner, Position, Matrix);

GE_REFLECT_VERSION(VersionedProbe, 7);
GE_REFLECT(VersionedProbe, value);

// Real Mathematics composites, mapped via the FieldTypeTraits specializations in
// Components/MathReflectionTraits.h (no per-test trait needed).
struct MathReflectProbe
{
    GameEngine::Mathematics::Vector2    V2;
    GameEngine::Mathematics::Vector3    V3;
    GameEngine::Mathematics::Vector4    V4;
    GameEngine::Mathematics::Quaternion Rot;
};
GE_REFLECT(MathReflectProbe, V2, V3, V4, Rot);

GE_REFLECT(GameEngine::Components::Camera,
    Perspective, FovY, OrthographicSize, NearZ, FarZ, CullingMask,
    PostProcessProfileId, MSAASamples, PostProcessMask, AspectPreset,
    CustomAspectWidth, CustomAspectHeight);

GE_REFLECT(GameEngine::Components::Light,
    Type, Color, Intensity, Range, InnerAngle, OuterAngle, AreaShape,
    AreaWidth, AreaHeight, AreaRadius, Decay, CastsLight, CastsShadows, CascadeCount);

GE_REFLECT(GameEngine::Components::Parent, parent);

// Fixtures for JSON edge cases and the through-the-handler integration test.
struct UnmappedThing { std::int32_t a; std::int32_t b; };  // no trait -> Unknown -> hex

struct JsonEdgeProbe
{
    char          Text[16];
    double        Big;
    std::int32_t  Neg;
    EntityHandle  Ref;
    UnmappedThing Opaque;
};
GE_REFLECT(JsonEdgeProbe, Text, Big, Neg, Ref, Opaque);

struct JsonProbe { bool On; std::int32_t Count; float Ratio; };
GE_REFLECT(JsonProbe, On, Count, Ratio);

// Field-count cap probes. CapProbe30 exercises the variadic GE_REFLECT with 30
// fields (would have failed under the old 24-field FOR_EACH cap; now valid up to
// 64). CapFreeProbe30 exercises the cap-free GE_REFLECT_BEGIN/FIELD/END form the
// component scanner emits, which has no arg-count cap at all.
struct CapProbe30 {
    std::int32_t f1, f2, f3, f4, f5, f6, f7, f8, f9, f10;
    std::int32_t f11, f12, f13, f14, f15, f16, f17, f18, f19, f20;
    std::int32_t f21, f22, f23, f24, f25, f26, f27, f28, f29, f30;
};
GE_REFLECT(CapProbe30,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10,
    f11, f12, f13, f14, f15, f16, f17, f18, f19, f20,
    f21, f22, f23, f24, f25, f26, f27, f28, f29, f30);

struct CapFreeProbe30 {
    std::int32_t g1, g2, g3, g4, g5, g6, g7, g8, g9, g10;
    std::int32_t g11, g12, g13, g14, g15, g16, g17, g18, g19, g20;
    std::int32_t g21, g22, g23, g24, g25, g26, g27, g28, g29, g30;
};
GE_REFLECT_BEGIN(CapFreeProbe30)
    GE_REFLECT_FIELD(CapFreeProbe30, g1)
    GE_REFLECT_FIELD(CapFreeProbe30, g2)
    GE_REFLECT_FIELD(CapFreeProbe30, g3)
    GE_REFLECT_FIELD(CapFreeProbe30, g4)
    GE_REFLECT_FIELD(CapFreeProbe30, g5)
    GE_REFLECT_FIELD(CapFreeProbe30, g6)
    GE_REFLECT_FIELD(CapFreeProbe30, g7)
    GE_REFLECT_FIELD(CapFreeProbe30, g8)
    GE_REFLECT_FIELD(CapFreeProbe30, g9)
    GE_REFLECT_FIELD(CapFreeProbe30, g10)
    GE_REFLECT_FIELD(CapFreeProbe30, g11)
    GE_REFLECT_FIELD(CapFreeProbe30, g12)
    GE_REFLECT_FIELD(CapFreeProbe30, g13)
    GE_REFLECT_FIELD(CapFreeProbe30, g14)
    GE_REFLECT_FIELD(CapFreeProbe30, g15)
    GE_REFLECT_FIELD(CapFreeProbe30, g16)
    GE_REFLECT_FIELD(CapFreeProbe30, g17)
    GE_REFLECT_FIELD(CapFreeProbe30, g18)
    GE_REFLECT_FIELD(CapFreeProbe30, g19)
    GE_REFLECT_FIELD(CapFreeProbe30, g20)
    GE_REFLECT_FIELD(CapFreeProbe30, g21)
    GE_REFLECT_FIELD(CapFreeProbe30, g22)
    GE_REFLECT_FIELD(CapFreeProbe30, g23)
    GE_REFLECT_FIELD(CapFreeProbe30, g24)
    GE_REFLECT_FIELD(CapFreeProbe30, g25)
    GE_REFLECT_FIELD(CapFreeProbe30, g26)
    GE_REFLECT_FIELD(CapFreeProbe30, g27)
    GE_REFLECT_FIELD(CapFreeProbe30, g28)
    GE_REFLECT_FIELD(CapFreeProbe30, g29)
    GE_REFLECT_FIELD(CapFreeProbe30, g30)
GE_REFLECT_END(CapFreeProbe30);

// ---- Compile-time invariants ---------------------------------------------

static_assert(FieldTypeIdOf<bool>()            == FieldTypeId::Bool);
static_assert(FieldTypeIdOf<std::int8_t>()     == FieldTypeId::Int8);
static_assert(FieldTypeIdOf<std::int32_t>()    == FieldTypeId::Int32);
static_assert(FieldTypeIdOf<std::int64_t>()    == FieldTypeId::Int64);
static_assert(FieldTypeIdOf<std::uint32_t>()   == FieldTypeId::UInt32);
static_assert(FieldTypeIdOf<std::uint64_t>()   == FieldTypeId::UInt64);
static_assert(FieldTypeIdOf<float>()           == FieldTypeId::Float);
static_assert(FieldTypeIdOf<double>()          == FieldTypeId::Double);
static_assert(FieldTypeIdOf<ProbeMode>()       == FieldTypeId::UInt16);  // enum -> underlying
static_assert(FieldTypeIdOf<float[3]>()        == FieldTypeId::Float);   // array -> element
static_assert(FieldTypeIdOf<char[16]>()        == FieldTypeId::String);  // char[] -> String
static_assert(FieldTypeIdOf<ColorLinear>()     == FieldTypeId::Color);
static_assert(FieldTypeIdOf<EntityHandle>()    == FieldTypeId::EntityHandle);
static_assert(FieldTypeIdOf<ProbeVec3>()       == FieldTypeId::Vec3);    // via trait

// Real Mathematics types via Components/MathReflectionTraits.h.
static_assert(FieldTypeIdOf<GameEngine::Mathematics::Vector2>()    == FieldTypeId::Vec2);
static_assert(FieldTypeIdOf<GameEngine::Mathematics::Vector3>()    == FieldTypeId::Vec3);
static_assert(FieldTypeIdOf<GameEngine::Mathematics::Vector4>()    == FieldTypeId::Vec4);
static_assert(FieldTypeIdOf<GameEngine::Mathematics::Quaternion>() == FieldTypeId::Quat);

static_assert(HasReflection<ReflectProbe>);
static_assert(!HasReflection<int>);
static_assert(GameEngine::ECS::Reflection<ReflectProbe>::FieldCount == 18);

// Field-count cap: >24 fields must reflect (was capped at 24 before the FOR_EACH
// unroll was extended to 64). Both the variadic and cap-free forms must agree.
static_assert(GameEngine::ECS::Reflection<CapProbe30>::FieldCount == 30);
static_assert(GameEngine::ECS::Reflection<CapFreeProbe30>::FieldCount == 30);

// ---- Helpers -------------------------------------------------------------

static const FieldInfo* FindField(std::span<const FieldInfo> fields, std::string_view name)
{
    for (const FieldInfo& f : fields)
        if (f.Name == name)
            return &f;
    return nullptr;
}

// ---- Tests ---------------------------------------------------------------

TEST(ComponentReflectionTests, ProbeFieldTableShapeAndTypes)
{
    const auto fields = GetReflectedFields<ReflectProbe>();
    ASSERT_EQ(fields.size(), 18u);

    // Names appear in declaration order.
    EXPECT_EQ(fields[0].Name, "Flag");
    EXPECT_EQ(fields[16].Name, "Position");
    EXPECT_EQ(fields[17].Name, "Matrix");

    struct Expect { const char* name; FieldTypeId type; std::uint32_t size; };
    const Expect expects[] = {
        {"Flag", FieldTypeId::Bool, 1},
        {"I8", FieldTypeId::Int8, 1},
        {"I16", FieldTypeId::Int16, 2},
        {"I32", FieldTypeId::Int32, 4},
        {"I64", FieldTypeId::Int64, 8},
        {"U8", FieldTypeId::UInt8, 1},
        {"U16", FieldTypeId::UInt16, 2},
        {"U32", FieldTypeId::UInt32, 4},
        {"U64", FieldTypeId::UInt64, 8},
        {"F", FieldTypeId::Float, 4},
        {"D", FieldTypeId::Double, 8},
        {"Mode", FieldTypeId::UInt16, 2},
        {"Triple", FieldTypeId::Float, 12},
        {"Label", FieldTypeId::String, 16},
        {"Tint", FieldTypeId::Color, 16},
        {"Owner", FieldTypeId::EntityHandle, 4},
        {"Position", FieldTypeId::Vec3, 12},
        {"Matrix", FieldTypeId::Float, 64},
    };
    for (const auto& e : expects)
    {
        const FieldInfo* f = FindField(fields, e.name);
        ASSERT_NE(f, nullptr) << "missing field " << e.name;
        EXPECT_EQ(f->Type, e.type) << "type mismatch for " << e.name;
        EXPECT_EQ(f->Size, e.size) << "size mismatch for " << e.name;
    }
}

TEST(ComponentReflectionTests, MathCompositesMapToVecAndQuat)
{
    const auto fields = GetReflectedFields<MathReflectProbe>();
    ASSERT_EQ(fields.size(), 4u);

    struct Expect { const char* name; FieldTypeId type; std::uint32_t size; };
    const Expect expects[] = {
        {"V2",  FieldTypeId::Vec2, 8},
        {"V3",  FieldTypeId::Vec3, 12},
        {"V4",  FieldTypeId::Vec4, 16},
        {"Rot", FieldTypeId::Quat, 16},
    };
    for (const auto& e : expects)
    {
        const FieldInfo* f = FindField(fields, e.name);
        ASSERT_NE(f, nullptr) << "missing field " << e.name;
        EXPECT_EQ(f->Type, e.type) << "type mismatch for " << e.name;
        EXPECT_EQ(f->Size, e.size) << "size mismatch for " << e.name;
        // A composite is ONE element: count = Size / FieldElementSize == 1.
        EXPECT_EQ(f->Size, FieldElementSize(e.type)) << "element size mismatch for " << e.name;
    }
}

TEST(ComponentReflectionTests, FieldFlagsRegisterAndApply)
{
    // Owned-copy registration: Get returns the registered fields, and per-field
    // flags can be applied afterwards (the mechanism behind GE_REFLECT_FIELD_FLAGS).
    const ComponentTypeId id = GetComponentTypeId<MathReflectProbe>();
    ComponentFieldRegistry::Register(id, GetReflectedFields<MathReflectProbe>(),
                                     "Test::MathReflectProbe");

    // Default: every field is None.
    for (const FieldInfo& f : ComponentFieldRegistry::Get(id))
        EXPECT_EQ(f.Flags, FieldFlags::None) << f.Name;

    ComponentFieldRegistry::SetFieldFlags(id, "V3", FieldFlags::ReadOnly);
    ComponentFieldRegistry::SetFieldFlags(id, "Rot", FieldFlags::Hidden | FieldFlags::ReadOnly);
    ComponentFieldRegistry::SetFieldFlags(id, "DoesNotExist", FieldFlags::Hidden);  // no-op, no crash

    const auto fields = ComponentFieldRegistry::Get(id);
    const FieldInfo* v3 = FindField(fields, "V3");
    ASSERT_NE(v3, nullptr);
    EXPECT_TRUE(HasAnyFlag(v3->Flags, FieldFlags::ReadOnly));
    EXPECT_FALSE(HasAnyFlag(v3->Flags, FieldFlags::Hidden));

    const FieldInfo* rot = FindField(fields, "Rot");
    ASSERT_NE(rot, nullptr);
    EXPECT_TRUE(HasAnyFlag(rot->Flags, FieldFlags::Hidden));
    EXPECT_TRUE(HasAnyFlag(rot->Flags, FieldFlags::ReadOnly));

    const FieldInfo* v2 = FindField(fields, "V2");
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v2->Flags, FieldFlags::None);  // untouched field stays None
}

TEST(ComponentReflectionTests, FieldCountCapExceedsTwentyFour)
{
    // Variadic GE_REFLECT with 30 fields (past the old 24 cap, within the new 64).
    {
        const auto fields = GetReflectedFields<CapProbe30>();
        ASSERT_EQ(fields.size(), 30u);
        EXPECT_EQ(fields[0].Name, "f1");
        EXPECT_EQ(fields[29].Name, "f30");
        EXPECT_EQ(fields[29].Offset, offsetof(CapProbe30, f30));
        for (const FieldInfo& f : fields)
            EXPECT_EQ(f.Type, FieldTypeId::Int32);
    }
    // Cap-free GE_REFLECT_BEGIN/FIELD/END form (what the scanner emits) — no cap.
    {
        const auto fields = GetReflectedFields<CapFreeProbe30>();
        ASSERT_EQ(fields.size(), 30u);
        EXPECT_EQ(fields[0].Name, "g1");
        EXPECT_EQ(fields[29].Name, "g30");
        EXPECT_EQ(fields[29].Offset, offsetof(CapFreeProbe30, g30));
    }
}

TEST(ComponentReflectionTests, OffsetsMatchOffsetof)
{
    const auto fields = GetReflectedFields<ReflectProbe>();
    EXPECT_EQ(FindField(fields, "Flag")->Offset,     offsetof(ReflectProbe, Flag));
    EXPECT_EQ(FindField(fields, "I64")->Offset,      offsetof(ReflectProbe, I64));
    EXPECT_EQ(FindField(fields, "Triple")->Offset,   offsetof(ReflectProbe, Triple));
    EXPECT_EQ(FindField(fields, "Owner")->Offset,    offsetof(ReflectProbe, Owner));
    EXPECT_EQ(FindField(fields, "Position")->Offset, offsetof(ReflectProbe, Position));
}

TEST(ComponentReflectionTests, RealComponentsReflectCorrectly)
{
    // Camera: all scalars.
    {
        const auto f = GetReflectedFields<GameEngine::Components::Camera>();
        EXPECT_EQ(f.size(), 12u);
        EXPECT_EQ(FindField(f, "Perspective")->Type, FieldTypeId::Bool);
        EXPECT_EQ(FindField(f, "FovY")->Type, FieldTypeId::Float);
        EXPECT_EQ(FindField(f, "CullingMask")->Type, FieldTypeId::UInt32);
    }
    // Light: enum -> UInt32, raw RGB float[3].
    {
        const auto f = GetReflectedFields<GameEngine::Components::Light>();
        EXPECT_EQ(f.size(), 14u);
        EXPECT_EQ(FindField(f, "Type")->Type, FieldTypeId::UInt32);       // LightType enum
        EXPECT_EQ(FindField(f, "AreaShape")->Type, FieldTypeId::UInt32);  // AreaLightShape enum
        const FieldInfo* color = FindField(f, "Color");
        ASSERT_NE(color, nullptr);
        EXPECT_EQ(color->Type, FieldTypeId::Float);
        EXPECT_EQ(color->Size, 12u);
    }
    // Parent: entity reference.
    {
        const auto f = GetReflectedFields<GameEngine::Components::Parent>();
        ASSERT_EQ(f.size(), 1u);
        EXPECT_EQ(f[0].Name, "parent");
        EXPECT_EQ(f[0].Type, FieldTypeId::EntityHandle);
        EXPECT_EQ(f[0].Size, 4u);
    }
}

TEST(ComponentReflectionTests, JsonDumpUsesFieldNamesAndValues)
{
    ReflectProbe p{};
    p.Flag = true;
    p.I32 = -5;
    p.U32 = 42;
    p.F = 1.5f;
    p.Mode = ProbeMode::Halt;
    p.Triple[0] = 1.0f; p.Triple[1] = 2.0f; p.Triple[2] = 3.0f;
    std::strcpy(p.Label, "hello");
    p.Tint = ColorLinear(0.25f, 0.5f, 0.75f, 1.0f);

    const std::string json = ComponentToJson(p);

    EXPECT_NE(json.find("\"Flag\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"I32\":-5"), std::string::npos) << json;
    EXPECT_NE(json.find("\"U32\":42"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Mode\":2"), std::string::npos) << json;        // enum value
    EXPECT_NE(json.find("\"Label\":\"hello\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"Triple\":["), std::string::npos) << json;       // array
    EXPECT_NE(json.find("\"Tint\":["), std::string::npos) << json;         // color as floats
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

TEST(ComponentReflectionTests, FieldWalkSnapshotRoundTrip)
{
    ReflectProbe src;
    std::memset(&src, 0, sizeof(src));
    src.Flag = true;
    src.I64 = -123456789012345LL;
    src.U64 = 0xFEEDFACECAFEBEEFull;
    src.F = 3.14159f;
    src.D = 2.718281828;
    src.Mode = ProbeMode::Run;
    src.Triple[0] = 9.0f; src.Triple[1] = 8.0f; src.Triple[2] = 7.0f;
    std::strcpy(src.Label, "snapshot");
    src.Tint = ColorLinear(0.1f, 0.2f, 0.3f, 0.4f);
    src.Owner = EntityHandle(11, 3);
    src.Position = ProbeVec3{ -1.0f, -2.0f, -3.0f };

    // Serialize each field's bytes into a blob (the (name,type,bytes) snapshot model).
    const auto fields = GetReflectedFields<ReflectProbe>();
    const auto* srcBytes = reinterpret_cast<const std::byte*>(&src);
    std::vector<std::byte> blob;
    for (const FieldInfo& f : fields)
        blob.insert(blob.end(), srcBytes + f.Offset, srcBytes + f.Offset + f.Size);

    // Restore field-by-field into a zeroed destination.
    ReflectProbe dst;
    std::memset(&dst, 0, sizeof(dst));
    auto* dstBytes = reinterpret_cast<std::byte*>(&dst);
    std::size_t cursor = 0;
    for (const FieldInfo& f : fields)
    {
        std::memcpy(dstBytes + f.Offset, blob.data() + cursor, f.Size);
        cursor += f.Size;
    }

    EXPECT_EQ(std::memcmp(&src, &dst, sizeof(ReflectProbe)), 0);
}

TEST(ComponentReflectionTests, JsonEscapesStringsAndDecodesEdgeTypes)
{
    JsonEdgeProbe p{};
    std::strcpy(p.Text, "a\"b\\c\n");  // embeds a quote, backslash, and newline
    p.Big = 0.5;
    p.Neg = -42;
    p.Ref = EntityHandle(5, 1);
    // Opaque left zeroed -> Unknown -> self-describing byte blob.

    const std::string json = ComponentToJson(p);

    // Control chars and metacharacters in strings must be JSON-escaped.
    EXPECT_NE(json.find("\"Text\":\""), std::string::npos) << json;
    EXPECT_NE(json.find("\\\""), std::string::npos) << "escaped quote: " << json;
    EXPECT_NE(json.find("\\\\"), std::string::npos) << "escaped backslash: " << json;
    EXPECT_NE(json.find("\\n"), std::string::npos) << "escaped newline: " << json;
    // Locale-independent, round-trip-exact numbers (not "0.500000").
    EXPECT_NE(json.find("\"Big\":0.5"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Neg\":-42"), std::string::npos) << json;
    // EntityHandle decodes as its packed id. Unknown decodes as {"size","data"} rather than a
    // bare hex string: the bare form is indistinguishable from a GUID at 16 bytes and from an
    // ordinary string anywhere else, which matters now that consumers feed this shape back to a
    // writer. The size is asserted alongside the data so a truncated dump cannot pass.
    EXPECT_NE(json.find("\"Ref\":"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Opaque\":{\"size\":" + std::to_string(sizeof(JsonEdgeProbe::Opaque)) +
                        ",\"data\":\""),
              std::string::npos)
        << json;
}

TEST(ComponentReflectionTests, NonFiniteFloatsBecomeNull)
{
    ReflectProbe p{};
    p.F = std::numeric_limits<float>::infinity();
    const std::string json = ComponentToJson(p);
    EXPECT_NE(json.find("\"F\":null"), std::string::npos) << json;  // not "inf"
}

// A reflected EntityHandle field carries no liveness of its own. EntityHandle::IsValid
// only tests the kInvalidEntity sentinel, so a field left ZEROED by a raw byte path
// (memset, a snapshot restore into zeroed storage, a writer that stores an unchecked id)
// PASSES it while naming no entity: World bumps a slot's version before minting the
// handle, so (index 0, version 0) can never exist. Any writer accepting an entity id
// from outside must therefore ask the World, not the handle.
TEST(ComponentReflectionTests, ZeroedEntityHandleLooksValidButNamesNoEntity)
{
    World world;

    const EntityHandle sentinel{};
    const EntityHandle zeroed{0u};

    EXPECT_FALSE(sentinel.IsValid()) << "the default-constructed handle IS the sentinel";
    EXPECT_TRUE(zeroed.IsValid())
        << "a zeroed id is not the sentinel, so the handle alone cannot reject it";
    EXPECT_FALSE(world.IsValid(zeroed)) << "yet no entity is ever minted at (index 0, version 0)";

    // The first entity really does take index 0 — and is still not the zeroed handle.
    Entity first = world.Create();
    world.ProcessCommands();
    const EntityHandle firstHandle = first.GetHandle();

    EXPECT_TRUE(world.IsValid(firstHandle));
    EXPECT_EQ(firstHandle.index, 0u) << "index 0 is a real, reachable slot";
    EXPECT_GE(firstHandle.version, 1u) << "versions are minted from 1, which is what saves us";
    EXPECT_NE(firstHandle.id, zeroed.id);
}

// End-to-end: the reflection path is actually wired into the component handler's
// GetComponentAsJson (not just a standalone utility). A reflected component dumps
// its real field names through the handler the editor debug path uses.
TEST(ComponentReflectionTests, HandlerJsonUsesReflectionWhenAvailable)
{
    World world;
    Entity entity = world.Create();
    entity.Set(JsonProbe{ true, -7, 0.5f });
    world.ProcessCommands();  // Set is deferred; apply it before reading back.

    Archetype* archetype = world.GetEntityArchetype(entity.GetHandle());
    ASSERT_NE(archetype, nullptr);

    // Locate the entity within the archetype's chunks via the public table
    // surface (entity metadata is World-private).
    ASSERT_TRUE(archetype->HasTable());
    auto chunks = std::as_const(*archetype).GetTable().GetChunks();
    uint16_t chunkIdx = 0, idxInChunk = 0;
    bool found = false;
    for (uint16_t c = 0; c < chunks.size() && !found; ++c)
    {
        const EntityHandle* handles = chunks[c].GetEntityHandles();
        for (uint16_t i = 0; i < chunks[c].GetCount(); ++i)
        {
            if (handles[i] == entity.GetHandle())
            {
                chunkIdx = c;
                idxInChunk = i;
                found = true;
                break;
            }
        }
    }
    ASSERT_TRUE(found);

    auto* handler = ComponentRegistry::GetHandler(GetComponentTypeId<JsonProbe>());
    ASSERT_NE(handler, nullptr);
    const std::string json = handler->GetComponentAsJson(archetype, chunkIdx, idxInChunk);

    EXPECT_NE(json.find("\"On\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Count\":-7"), std::string::npos) << json;
    EXPECT_NE(json.find("\"Ratio\":0.5"), std::string::npos) << json;
}

TEST(ComponentReflectionTests, VersionDefaultsToZeroAndCanBeSet)
{
    EXPECT_EQ(GetReflectionVersion<ReflectProbe>(), 0u);
    EXPECT_EQ(GetReflectionVersion<VersionedProbe>(), 7u);
}
