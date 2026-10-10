#include "Scripting/ECSABI.h"

#include "AbiHandles.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "Logger/Logger.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECS/BlobComponent.h"
#include "ECS/CachedQuery.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentMigration.h"
#include "ECS/ComponentRegistry.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "Components/Rendering/MeshRenderer.h"

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"

#include "AssetCore/AssetTypes.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <string_view>
#include <vector>

// The ABI must not let native exceptions cross into managed code, but a
// silent catch(...) swallows real failures with no trace. Log the first
// occurrence per entry point and count the rest (these are per-frame call
// sites — they must not spam the log).
#define GE_ECSABI_CATCH_ALL_FAIL                                               \
    catch (...)                                                                \
    {                                                                          \
        static std::atomic<uint64_t> s_CatchCount{0};                          \
        if (s_CatchCount.fetch_add(1, std::memory_order_relaxed) == 0)         \
            Logger::Log::Error("[ECSABI] {} swallowed a native exception "     \
                               "(logged once; further occurrences counted)",   \
                               __func__);                                      \
        return GE_Result_Fail;                                                 \
    }

namespace
{
using GameEngine::ScriptingAbi::EntityFromId;
using GameEngine::ScriptingAbi::WorldFromHandle;

static GE_Result EnsureEngineInitializedForEcsAbi()
{
    return GameEngine::DllBootstrap::EnsureEngineInitialized();
}

// WarnIfEcsStateNotRedirected used to fire when a DLL's type-id allocator
// diverged from the host's. With consteval ComponentTypeId hashes (Phase 1b),
// type IDs are compile-time-constant and cross-DLL-stable by construction,
// so the warning has no business condition left. The function and its three
// call sites were removed in the C7e cleanup.

static GameEngine::Components::Name MakeNameFromUtf8(const char* utf8, uint32_t len, std::string_view fallback)
{
    using GameEngine::Components::Name;
    Name n{};
    std::memset(n.value, 0, sizeof(n.value));

    std::string_view src;
    if (utf8 != nullptr && len > 0)
    {
        src = std::string_view(utf8, static_cast<size_t>(len));
    }
    else
    {
        src = fallback;
    }

    if (src.empty())
        return n;

    const size_t maxCopy = sizeof(n.value) - 1;
    const size_t toCopy = std::min(maxCopy, src.size());
    std::memcpy(n.value, src.data(), toCopy);
    n.value[toCopy] = '\0';
    return n;
}

static bool ResolveParent(GameEngine::ECS::World* w, GE_ECS_Entity parentEntity, GameEngine::ECS::EntityHandle& outParent)
{
    outParent = GameEngine::ECS::EntityHandle::Invalid();
    if (parentEntity == 0)
    {
        return true; // no parent
    }

    const GameEngine::ECS::EntityHandle h = EntityFromId(parentEntity);
    if (!w || !w->IsValid(h))
    {
        return false;
    }
    outParent = h;
    return true;
}

static GameEngine::GUID ResolvePrimitiveMeshGuid(GE_ECS_PrimitiveType primitive)
{
    using namespace GameEngine::Engine::Renderer;
    switch (primitive)
    {
    case GE_ECS_Primitive_Cube:
        return PrimitiveGenerator::CubeGuid();
    case GE_ECS_Primitive_Sphere:
        return PrimitiveGenerator::SphereGuid();
    case GE_ECS_Primitive_Capsule:
        return PrimitiveGenerator::CapsuleGuid();
    case GE_ECS_Primitive_Plane:
        return PrimitiveGenerator::PlaneGuid();
    default:
        return GameEngine::GUID::Null();
    }
}

} // namespace

extern "C"
{

GE_API GE_Result GE_CDECL GE_ECSABI_GetPrimaryWorld(GE_Handle* outWorld)
{
    try
    {
        if (!outWorld)
            return GE_Result_InvalidArg;
        if (EnsureEngineInitializedForEcsAbi() != GE_Result_Ok)
            return GE_Result_Fail;
        auto& eng = GameEngine::EngineCore::GetInstance();
        auto* world = eng.EnsurePrimaryWorld();
        if (!world)
            return GE_Result_Fail;
        *outWorld = static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(world));
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetEntityCount(GE_Handle world, int32_t* outCount)
{
    try
    {
        if (!outCount)
            return GE_Result_InvalidArg;
        if (world == 0)
            return GE_Result_InvalidArg;
        auto* w = WorldFromHandle(world);
        *outCount = static_cast<int32_t>(w->GetEntityCount());
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CreateEmptyEntity(GE_Handle world,
                                                      const char* nameUtf8,
                                                      uint32_t nameLen,
                                                      GE_ECS_Entity parentEntity,
                                                      GE_ECS_Entity* outEntity)
{
    try
    {
        if (!outEntity)
            return GE_Result_InvalidArg;
        if (world == 0)
            return GE_Result_InvalidArg;
        if ((nameUtf8 == nullptr) && nameLen != 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        GameEngine::ECS::EntityHandle parentHandle{};
        if (!ResolveParent(w, parentEntity, parentHandle))
            return GE_Result_InvalidArg;

        GameEngine::ECS::EntityHandle created = w->CreateEntity();
        if (!created.IsValid())
            return GE_Result_Fail;

        GameEngine::Components::Transform t{};
        t.SetIdentity();
        w->AddComponentImmediate(created, t);

        GameEngine::Components::Name n = MakeNameFromUtf8(nameUtf8, nameLen, "Entity");
        w->AddComponentImmediate(created, n);

        if (parentHandle.IsValid())
        {
            GameEngine::Components::Parent p{};
            p.parent = parentHandle;
            w->AddComponentImmediate(created, p);
        }

        *outEntity = static_cast<GE_ECS_Entity>(created.id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CreatePrimitive(GE_Handle world,
                                                    GE_ECS_PrimitiveType primitive,
                                                    const char* nameUtf8,
                                                    uint32_t nameLen,
                                                    GE_ECS_Entity parentEntity,
                                                    GE_ECS_Entity* outEntity)
{
    try
    {
        if (!outEntity)
            return GE_Result_InvalidArg;
        if (world == 0)
            return GE_Result_InvalidArg;
        if ((nameUtf8 == nullptr) && nameLen != 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        GameEngine::ECS::EntityHandle parentHandle{};
        if (!ResolveParent(w, parentEntity, parentHandle))
            return GE_Result_InvalidArg;

        GameEngine::GUID meshGuid = ResolvePrimitiveMeshGuid(primitive);
        if (meshGuid.IsNull())
            return GE_Result_InvalidArg;

        auto* rs = GameEngine::EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return GE_Result_Fail;

        using MeshGPUKey = GameEngine::Rendering::MeshGPUKey;
        auto meshHandle = rs->GetMeshGPURegistry().FindHandle(MeshGPUKey{meshGuid, 0u});
        if (!meshHandle.IsValid())
            return GE_Result_Fail;

        GameEngine::ECS::EntityHandle created = w->CreateEntity();
        if (!created.IsValid())
            return GE_Result_Fail;

        GameEngine::Components::Transform t{};
        t.SetIdentity();
        w->AddComponentImmediate(created, t);

        std::string_view fallbackName = "Primitive";
        switch (primitive)
        {
        case GE_ECS_Primitive_Cube:
            fallbackName = "Cube";
            break;
        case GE_ECS_Primitive_Sphere:
            fallbackName = "Sphere";
            break;
        case GE_ECS_Primitive_Capsule:
            fallbackName = "Capsule";
            break;
        case GE_ECS_Primitive_Plane:
            fallbackName = "Plane";
            break;
        default:
            break;
        }

        GameEngine::Components::Name n = MakeNameFromUtf8(nameUtf8, nameLen, fallbackName);
        w->AddComponentImmediate(created, n);

        if (parentHandle.IsValid())
        {
            GameEngine::Components::Parent p{};
            p.parent = parentHandle;
            w->AddComponentImmediate(created, p);
        }

        auto materialGuid = GameEngine::Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();

        GameEngine::Components::MeshRenderer mr{};
        mr.meshGpuHandleId = static_cast<uint64_t>(meshHandle);
        mr.materialAssetGuid.Set(materialGuid);
        mr.renderLayerMask = 1u;
        w->AddComponentImmediate(created, mr);

        // LocalBounds so the entity is pickable + cullable from day one
        // without per-system fallback rules. Mirrors what
        // BuiltInSceneSchemas + DefaultSceneEntities + HierarchyPanel do.
        w->AddComponentImmediate(created,
            GameEngine::Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(rs, meshGuid));

        *outEntity = static_cast<GE_ECS_Entity>(created.id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_IsEntityValid(GE_Handle world, GE_ECS_Entity entity, int32_t* outValid)
{
    try
    {
        if (!outValid)
            return GE_Result_InvalidArg;
        *outValid = 0;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_Ok;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        *outValid = w->IsValid(h) ? 1 : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_DestroyEntity(GE_Handle world, GE_ECS_Entity entity)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        w->DestroyEntity(h);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_SetName(GE_Handle world, GE_ECS_Entity entity, const char* nameUtf8, uint32_t nameLen)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;
        if ((nameUtf8 == nullptr) && nameLen != 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        GameEngine::Components::Name n = MakeNameFromUtf8(nameUtf8, nameLen, "");
        if (auto* existing = w->GetComponentForWrite<GameEngine::Components::Name>(h))
        {
            *existing = n;
        }
        else
        {
            w->AddComponentImmediate(h, n);
        }
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetName(GE_Handle world, GE_ECS_Entity entity, char* outNameUtf8, uint32_t outCap, uint32_t* outLen)
{
    try
    {
        if (!outLen)
            return GE_Result_InvalidArg;
        *outLen = 0;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;
        if (outCap != 0 && outNameUtf8 == nullptr)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        auto* name = w->GetComponent<GameEngine::Components::Name>(h);
        if (!name)
            return GE_Result_NotFound;

        const std::string_view src = name->View();
        *outLen = static_cast<uint32_t>(src.size());

        if (outNameUtf8 && outCap > 0)
        {
            const size_t maxCopy = static_cast<size_t>(outCap - 1);
            const size_t toCopy = std::min(maxCopy, src.size());
            if (toCopy > 0)
                std::memcpy(outNameUtf8, src.data(), toCopy);
            outNameUtf8[toCopy] = '\0';
        }

        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetTransform(GE_Handle world, GE_ECS_Entity entity, GE_ECS_Transform* outTransform)
{
    try
    {
        if (!outTransform)
            return GE_Result_InvalidArg;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        auto* t = w->GetComponent<GameEngine::Components::Transform>(h);
        if (!t)
            return GE_Result_NotFound;

        std::memcpy(outTransform->matrix, t->matrix, sizeof(outTransform->matrix));
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_SetTransform(GE_Handle world, GE_ECS_Entity entity, const GE_ECS_Transform* transform)
{
    try
    {
        if (!transform)
            return GE_Result_InvalidArg;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        if (auto* existing = w->GetComponentForWrite<GameEngine::Components::Transform>(h))
        {
            std::memcpy(existing->matrix, transform->matrix, sizeof(transform->matrix));
        }
        else
        {
            GameEngine::Components::Transform t{};
            std::memcpy(t.matrix, transform->matrix, sizeof(transform->matrix));
            w->AddComponentImmediate(h, t);
        }
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_RegisterBlobComponent(const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes, GE_ECS_ComponentTypeId* outTypeId)
{
    try
    {
        if (!outTypeId)
            return GE_Result_InvalidArg;
        *outTypeId = 0;
        if ((nameUtf8 == nullptr) && nameLen != 0)
            return GE_Result_InvalidArg;
        if (nameLen == 0 || sizeBytes == 0)
            return GE_Result_InvalidArg;

        std::string_view sv(nameUtf8, static_cast<size_t>(nameLen));
        std::string name(sv);
        auto id = GameEngine::ECS::ComponentRegistry::RegisterBlobComponent(name, static_cast<std::size_t>(sizeBytes));
        if (id == 0)
            return GE_Result_Fail;
        *outTypeId = static_cast<GE_ECS_ComponentTypeId>(id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentDisabledTypeId(GE_ECS_ComponentTypeId componentTypeId,
                                                               GE_ECS_ComponentTypeId* outDisabledTypeId)
{
    try
    {
        if (!outDisabledTypeId)
            return GE_Result_InvalidArg;
        *outDisabledTypeId = 0;
        if (componentTypeId == 0)
            return GE_Result_InvalidArg;
        const auto typeId = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
        const auto* info = GameEngine::ECS::ComponentRegistry::GetComponentInfo(typeId);
        if (info && GameEngine::ECS::HasAnyFlag(info->Flags, GameEngine::ECS::ComponentFlags::NotToggleable))
            return GE_Result_Ok;
        const auto tag = GameEngine::ECS::ComponentRegistry::RegisterComponentDisabledTag(typeId);
        if (tag == 0)
            return GE_Result_NotFound;
        *outDisabledTypeId = static_cast<GE_ECS_ComponentTypeId>(tag);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_SetEntityEnabled(GE_Handle world, GE_ECS_Entity entity, int32_t enabled)
{
    try
    {
        if (world == 0 || entity == 0)
            return GE_Result_InvalidArg;
        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;
        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;
        if (w->GetDeferStructuralChanges())
        {
            GameEngine::ECS::Command cmd;
            cmd.type = GameEngine::ECS::Command::SET_ENTITY_ENABLED;
            cmd.entity = h;
            cmd.entityEnabled = enabled != 0;
            if (!w->TryPushCommand(std::move(cmd)))
                return GE_Result_BufferFull;
            return GE_Result_Ok;
        }
        w->SetEntityEnabledImmediate(h, enabled != 0);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_RegisterBlobComponentWithSchema(
    const char* nameUtf8, uint32_t nameLen, uint32_t sizeBytes,
    const GE_ECS_FieldDesc* fields, uint32_t fieldCount,
    GE_ECS_ComponentTypeId* outTypeId)
{
    try
    {
        using namespace GameEngine::ECS;

        if (!outTypeId)
            return GE_Result_InvalidArg;
        *outTypeId = 0;
        if (nameUtf8 == nullptr || nameLen == 0 || sizeBytes == 0)
            return GE_Result_InvalidArg;
        if (fields == nullptr && fieldCount != 0)
            return GE_Result_InvalidArg;

        const std::string name(nameUtf8, static_cast<size_t>(nameLen));

        // Validate the whole descriptor array up front — a bad schema is rejected whole (no
        // partial tables) and loudly; a silent drop would resurface much later as "component
        // saves nothing". The offset/size contract: every field's span must lie inside the
        // component and be a whole element count of its type.
        std::vector<FieldInfo> newFields;
        newFields.reserve(fieldCount);
        for (uint32_t i = 0; i < fieldCount; ++i)
        {
            const GE_ECS_FieldDesc& d = fields[i];
            const bool typeKnown =
                d.fieldType != 0 && d.fieldType <= static_cast<uint16_t>(FieldTypeId::Bytes);
            const uint32_t elem = typeKnown ? FieldElementSize(static_cast<FieldTypeId>(d.fieldType)) : 0;
            const bool spanValid = d.size != 0 &&
                                   static_cast<uint64_t>(d.offset) + d.size <= sizeBytes &&
                                   (elem == 0 || (d.size % elem) == 0);
            if (d.nameUtf8 == nullptr || d.nameLen == 0 || !typeKnown || !spanValid)
            {
                Logger::Log::Error("[ECSABI] blob component '{}' field schema REJECTED: field {} ('{}') "
                                "type {} offset {} size {} invalid for component size {}",
                                name, i,
                                d.nameUtf8 ? std::string(d.nameUtf8, d.nameLen) : std::string(),
                                d.fieldType, d.offset, d.size, sizeBytes);
                return GE_Result_InvalidArg;
            }
            FieldInfo f{};
            f.Name = std::string_view(d.nameUtf8, d.nameLen); // deep-copied by RegisterOwned below
            f.Offset = d.offset;
            f.Size = d.size;
            f.Type = static_cast<FieldTypeId>(d.fieldType);
            newFields.push_back(f);
        }

        // Resolve or create the blob registration. A size change on an existing registration is
        // the hot-reload relayout case: plain RegisterBlobComponent must keep rejecting it (it
        // cannot migrate), so resolve by name here and let the migration below carry the size.
        ComponentTypeId id = ComponentRegistry::GetComponentTypeId(name);
        std::size_t oldSize = 0;
        bool existed = false;
        if (id != 0)
        {
            auto* handler = ComponentRegistry::GetHandler(id);
            if (handler == nullptr || dynamic_cast<GameEngine::ECS::BlobComponentHandler*>(handler) == nullptr)
            {
                Logger::Log::Error("[ECSABI] blob component schema REJECTED: '{}' is already registered "
                                "as a native component — native reflection owns its field table",
                                name);
                return GE_Result_Fail;
            }
            oldSize = handler->GetComponentSize();
            existed = true;
        }
        else
        {
            id = ComponentRegistry::RegisterBlobComponent(name, static_cast<std::size_t>(sizeBytes));
            if (id == 0)
                return GE_Result_Fail;
        }

        // Snapshot the previous field table WITH owned name copies before RegisterOwned replaces
        // the string pool those names point into. This is the "old layout" side of a reload.
        std::vector<std::string> oldNames;
        std::vector<FieldInfo> oldFields;
        {
            const std::span<const FieldInfo> oldSpan = ComponentFieldRegistry::Get(id);
            oldNames.reserve(oldSpan.size()); // exact: views below must never move
            oldFields.assign(oldSpan.begin(), oldSpan.end());
            for (FieldInfo& f : oldFields)
            {
                oldNames.emplace_back(f.Name);
                f.Name = oldNames.back();
            }
        }

        ComponentFieldRegistry::RegisterOwned(id, newFields, name);

        // Default bytes: a C# component's default(T) is zero-initialized memory. Registering them
        // makes the component loadable from .scene (ComponentFactory::Create) and lists it in the
        // editor's Add Component menu; migration also seeds new fields from these bytes.
        std::vector<std::uint8_t> defaults(sizeBytes, 0);
        ComponentFactory::RegisterDefaultBytes(id, defaults.data(), defaults.size(), /*addable=*/true);

        // Hot-reload relayout: re-pack placed instances so same-named fields survive and the
        // recorded size matches the new stride. Same across-worlds entry as the editor's
        // native-reload migration — every live world holding the component migrates, with
        // the host's primary world always covered explicitly (this TU's statically-linked
        // ECS has its own live-world registry; see MigrateComponentLayoutAcrossWorlds).
        // A schema attaching to a previously schema-less blob at the SAME size is not a
        // relayout — the bytes already match the described layout; re-packing against an
        // empty old table would zero live instances.
        const bool sameSizeSchemaAttach = oldFields.empty() && oldSize == sizeBytes;
        if (existed && !sameSizeSchemaAttach &&
            (oldSize != sizeBytes || LayoutsDiffer(oldFields, oldSize, newFields, sizeBytes)))
        {
            auto& eng = GameEngine::EngineCore::GetInstance();
            if (GameEngine::ECS::World* world = eng.EnsurePrimaryWorld())
            {
                GameEngine::ECS::World::MigrateComponentLayoutAcrossWorlds(
                    *world, ComponentLayoutChange{id, std::move(oldFields), oldSize,
                                                  std::move(newFields),
                                                  static_cast<std::size_t>(sizeBytes),
                                                  std::move(defaults)});
                Logger::Log::Info("[ECSABI] migrated blob component '{}' layout: {} -> {} bytes",
                               name, oldSize, sizeBytes);
            }
        }

        *outTypeId = static_cast<GE_ECS_ComponentTypeId>(id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentTypeIdByName(const char* nameUtf8, uint32_t nameLen, GE_ECS_ComponentTypeId* outTypeId)
{
    try
    {
        if (!outTypeId)
            return GE_Result_InvalidArg;
        *outTypeId = 0;
        if ((nameUtf8 == nullptr) && nameLen != 0)
            return GE_Result_InvalidArg;
        if (nameLen == 0)
            return GE_Result_InvalidArg;
        std::string_view sv(nameUtf8, static_cast<size_t>(nameLen));
        std::string name(sv);
        auto id = GameEngine::ECS::ComponentRegistry::GetComponentTypeId(name);
        if (id == 0)
            return GE_Result_NotFound;
        *outTypeId = static_cast<GE_ECS_ComponentTypeId>(id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_SetComponentBytes(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, const uint8_t* data, uint32_t dataLen)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;
        if ((data == nullptr) && dataLen != 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        if (w->GetDeferStructuralChanges())
        {
            GameEngine::ECS::Command cmd;
            cmd.type = GameEngine::ECS::Command::SET_COMPONENT;
            cmd.entity = h;
            cmd.componentType = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
            if (data && dataLen > 0)
                cmd.componentData.Assign(data, static_cast<size_t>(dataLen));
            if (!w->TryPushCommand(std::move(cmd)))
                return GE_Result_BufferFull;
            return GE_Result_Ok;
        }

        if (!w->SetComponentBytesImmediate(h, static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId), data, static_cast<std::size_t>(dataLen)))
            return GE_Result_Fail;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_GetComponentBytes(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId, uint8_t* outData, uint32_t outCap, uint32_t* outLen)
{
    try
    {
        if (!outLen)
            return GE_Result_InvalidArg;
        *outLen = 0;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;
        if (outCap != 0 && outData == nullptr)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        std::size_t size = 0;
        if (!w->ReadComponentBytes(h, static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId),
                                   outData, static_cast<std::size_t>(outCap), size))
            return GE_Result_NotFound;

        *outLen = static_cast<uint32_t>(size);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_RemoveComponent(GE_Handle world, GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        if (w->GetDeferStructuralChanges())
        {
            GameEngine::ECS::Command cmd;
            cmd.type = GameEngine::ECS::Command::REMOVE_COMPONENT;
            cmd.entity = h;
            cmd.componentType = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
            if (!w->TryPushCommand(std::move(cmd)))
                return GE_Result_BufferFull;
            return GE_Result_Ok;
        }

        if (!w->RemoveComponentByTypeIdImmediate(h, static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId)))
            return GE_Result_NotFound;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.3: Cached chunk queries
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryCreate(GE_Handle world,
                                                       const GE_ECS_ComponentTypeId* requiredTypeIds,
                                                       uint32_t requiredCount,
                                                       const GE_ECS_ComponentTypeId* excludedTypeIds,
                                                       uint32_t excludedCount,
                                                       GE_Handle* outQuery)
{
    try
    {
        if (!outQuery)
            return GE_Result_InvalidArg;
        *outQuery = 0;
        if (world == 0)
            return GE_Result_InvalidArg;
        if ((requiredTypeIds == nullptr) && requiredCount != 0)
            return GE_Result_InvalidArg;
        if ((excludedTypeIds == nullptr) && excludedCount != 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        std::vector<GameEngine::ECS::ComponentTypeId> required;
        required.reserve(requiredCount);
        for (uint32_t i = 0; i < requiredCount; ++i)
            required.push_back(static_cast<GameEngine::ECS::ComponentTypeId>(requiredTypeIds[i]));

        std::vector<GameEngine::ECS::ComponentTypeId> excluded;
        excluded.reserve(excludedCount);
        for (uint32_t i = 0; i < excludedCount; ++i)
            excluded.push_back(static_cast<GameEngine::ECS::ComponentTypeId>(excludedTypeIds[i]));

        auto* cq = new GameEngine::ECS::CachedQuery(w, std::move(required), std::move(excluded));
        *outQuery = static_cast<GE_Handle>(reinterpret_cast<uintptr_t>(cq));
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryDestroy(GE_Handle query)
{
    try
    {
        if (query == 0)
            return GE_Result_InvalidArg;
        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        delete cq;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryReset(GE_Handle query,
                                                      uint32_t* outArchetypeCount)
{
    try
    {
        if (!outArchetypeCount)
            return GE_Result_InvalidArg;
        *outArchetypeCount = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        cq->Refresh();
        *outArchetypeCount = static_cast<uint32_t>(cq->GetArchetypeCount());
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetArchetypeInfo(GE_Handle query,
                                                                  uint32_t archetypeIndex,
                                                                  uint32_t* outEntityCount,
                                                                  uint32_t* outChunkCount)
{
    try
    {
        if (!outEntityCount || !outChunkCount)
            return GE_Result_InvalidArg;
        *outEntityCount = 0;
        *outChunkCount = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto* archetype = cq->GetArchetype(static_cast<std::size_t>(archetypeIndex));
        if (!archetype)
            return GE_Result_InvalidArg;

        *outEntityCount = static_cast<uint32_t>(archetype->GetEntityCount());
        *outChunkCount = static_cast<uint32_t>(archetype->GetChunkCount());
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkData(GE_Handle query,
                                                              uint32_t archetypeIndex,
                                                              uint32_t chunkIndex,
                                                              GE_ECS_ComponentTypeId componentTypeId,
                                                              void** outData,
                                                              uint32_t* outCount,
                                                              uint32_t* outStride)
{
    try
    {
        if (!outData || !outCount || !outStride)
            return GE_Result_InvalidArg;
        *outData = nullptr;
        *outCount = 0;
        *outStride = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto* archetype = cq->GetArchetype(static_cast<std::size_t>(archetypeIndex));
        if (!archetype)
            return GE_Result_InvalidArg;

        auto typeId = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
        auto [ptr, count] = archetype->GetChunkDataRaw(typeId, static_cast<std::size_t>(chunkIndex));
        if (!ptr && count == 0 && chunkIndex > 0)
            return GE_Result_InvalidArg;

        *outData = ptr;
        *outCount = static_cast<uint32_t>(count);

        // Get stride from handler
        auto* handler = GameEngine::ECS::ComponentRegistry::GetHandler(typeId);
        *outStride = handler ? static_cast<uint32_t>(handler->GetComponentSize()) : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// v1.7: chunk enumeration with the change-signaling chunkVersion out param.
// Mutable span — records a write grant (stamps the column). Managed reads
// must use the ReadOnly entry below instead.
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkDataV2(GE_Handle query,
                                                               uint32_t archetypeIndex,
                                                               uint32_t chunkIndex,
                                                               GE_ECS_ComponentTypeId componentTypeId,
                                                               void** outData,
                                                               uint32_t* outCount,
                                                               uint32_t* outStride,
                                                               uint64_t* outChunkVersion)
{
    try
    {
        if (!outData || !outCount || !outStride)
            return GE_Result_InvalidArg;
        *outData = nullptr;
        *outCount = 0;
        *outStride = 0;
        if (outChunkVersion)
            *outChunkVersion = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto* archetype = cq->GetArchetype(static_cast<std::size_t>(archetypeIndex));
        if (!archetype)
            return GE_Result_InvalidArg;

        auto typeId = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
        auto [ptr, count] = archetype->GetChunkDataRaw(typeId, static_cast<std::size_t>(chunkIndex));
        if (!ptr && count == 0 && chunkIndex > 0)
            return GE_Result_InvalidArg;

        *outData = ptr;
        *outCount = static_cast<uint32_t>(count);
        if (outChunkVersion)
            *outChunkVersion = archetype->GetColumnVersion(typeId, static_cast<std::size_t>(chunkIndex));

        auto* handler = GameEngine::ECS::ComponentRegistry::GetHandler(typeId);
        *outStride = handler ? static_cast<uint32_t>(handler->GetComponentSize()) : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// v1.7: read-only chunk span (design Q7/M11) — the const GetChunkDataRaw
// overload, exposed so managed READS never take a write grant.
GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkDataReadOnly(GE_Handle query,
                                                                     uint32_t archetypeIndex,
                                                                     uint32_t chunkIndex,
                                                                     GE_ECS_ComponentTypeId componentTypeId,
                                                                     const void** outData,
                                                                     uint32_t* outCount,
                                                                     uint32_t* outStride,
                                                                     uint64_t* outChunkVersion)
{
    try
    {
        if (!outData || !outCount || !outStride)
            return GE_Result_InvalidArg;
        *outData = nullptr;
        *outCount = 0;
        *outStride = 0;
        if (outChunkVersion)
            *outChunkVersion = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        const auto* archetype = cq->GetArchetype(static_cast<std::size_t>(archetypeIndex));
        if (!archetype)
            return GE_Result_InvalidArg;

        auto typeId = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
        auto [ptr, count] = archetype->GetChunkDataRaw(typeId, static_cast<std::size_t>(chunkIndex));
        if (!ptr && count == 0 && chunkIndex > 0)
            return GE_Result_InvalidArg;

        *outData = ptr;
        *outCount = static_cast<uint32_t>(count);
        if (outChunkVersion)
            *outChunkVersion = archetype->GetColumnVersion(typeId, static_cast<std::size_t>(chunkIndex));

        auto* handler = GameEngine::ECS::ComponentRegistry::GetHandler(typeId);
        *outStride = handler ? static_cast<uint32_t>(handler->GetComponentSize()) : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetChunkEntityIds(GE_Handle query,
                                                                   uint32_t archetypeIndex,
                                                                   uint32_t chunkIndex,
                                                                   uint32_t** outEntityIds,
                                                                   uint32_t* outCount)
{
    try
    {
        if (!outEntityIds || !outCount)
            return GE_Result_InvalidArg;
        *outEntityIds = nullptr;
        *outCount = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto [ids, count] = cq->GetChunkEntityIds(
            static_cast<std::size_t>(archetypeIndex),
            static_cast<std::size_t>(chunkIndex));
        if (!ids)
            return GE_Result_InvalidArg;

        *outEntityIds = ids;
        *outCount = static_cast<uint32_t>(count);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.3: HasComponent
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_HasComponent(GE_Handle world,
                                                   GE_ECS_Entity entity,
                                                   GE_ECS_ComponentTypeId componentTypeId,
                                                   int32_t* outHas)
{
    try
    {
        if (!outHas)
            return GE_Result_InvalidArg;
        *outHas = 0;
        if (world == 0)
            return GE_Result_InvalidArg;
        if (entity == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);
        if (!w->IsValid(h))
            return GE_Result_InvalidArg;

        auto* archetype = w->GetEntityArchetype(h);
        if (!archetype)
        {
            *outHas = 0;
            return GE_Result_Ok;
        }

        *outHas = archetype->GetSignature().Contains(
                      static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId))
                      ? 1
                      : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.3: Structural change fencing
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_SetDeferStructuralChanges(GE_Handle world, int32_t defer)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        w->SetDeferStructuralChanges(defer != 0);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_FlushDeferredCommands(GE_Handle world)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        w->SetDeferStructuralChanges(false);
        w->ProcessCommands();
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.4: Per-archetype component presence check
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryArchetypeHasComponent(GE_Handle query,
                                                                       uint32_t archetypeIndex,
                                                                       GE_ECS_ComponentTypeId componentTypeId,
                                                                       int32_t* outHas)
{
    try
    {
        if (!outHas)
            return GE_Result_InvalidArg;
        *outHas = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        *outHas = cq->ArchetypeHasComponent(
                      static_cast<std::size_t>(archetypeIndex),
                      static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId))
                      ? 1
                      : 0;
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.5: Entity command buffer
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_CreateEntityRaw(GE_Handle world, GE_ECS_Entity* outEntity)
{
    try
    {
        if (!outEntity)
            return GE_Result_InvalidArg;
        *outEntity = 0;
        if (world == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        // CreateEntity only allocates a metadata slot — no archetype, no components.
        // Safe to call during iteration because it does not move any existing data.
        GameEngine::ECS::EntityHandle h = w->CreateEntity();
        if (!h.IsValid())
            return GE_Result_Fail;

        *outEntity = static_cast<GE_ECS_Entity>(h.id);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_DeferCommand(GE_Handle world, uint32_t commandType,
                                                   GE_ECS_Entity entity, GE_ECS_ComponentTypeId componentTypeId,
                                                   const void* data, uint32_t dataLen)
{
    try
    {
        if (world == 0)
            return GE_Result_InvalidArg;

        auto* w = WorldFromHandle(world);
        if (!w)
            return GE_Result_Fail;

        GameEngine::ECS::Command cmd;
        const GameEngine::ECS::EntityHandle h = EntityFromId(entity);

        switch (commandType)
        {
        case 1: // DESTROY
            cmd.type = GameEngine::ECS::Command::DESTROY_ENTITY;
            cmd.entity = h;
            break;
        case 2: // ADD_COMPONENT
            cmd.type = GameEngine::ECS::Command::ADD_COMPONENT;
            cmd.entity = h;
            cmd.componentType = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
            if (data && dataLen > 0)
                cmd.componentData.Assign(data, static_cast<size_t>(dataLen));
            break;
        case 3: // REMOVE_COMPONENT
            cmd.type = GameEngine::ECS::Command::REMOVE_COMPONENT;
            cmd.entity = h;
            cmd.componentType = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
            break;
        case 4: // SET_COMPONENT
            cmd.type = GameEngine::ECS::Command::SET_COMPONENT;
            cmd.entity = h;
            cmd.componentType = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
            if (data && dataLen > 0)
                cmd.componentData.Assign(data, static_cast<size_t>(dataLen));
            break;
        default:
            return GE_Result_InvalidArg;
        }

        if (!w->TryPushCommand(std::move(cmd)))
            return GE_Result_BufferFull;

        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

// ---------------------------------------------------------------------------
// v1.6: Slice-based component data access
// ---------------------------------------------------------------------------

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetComponentSlice(
    GE_Handle query, uint32_t archetypeIndex, GE_ECS_ComponentTypeId componentTypeId,
    uint32_t entityOffset, void** outData, uint32_t* outCount, uint32_t* outStride)
{
    try
    {
        if (!outData || !outCount || !outStride)
            return GE_Result_InvalidArg;
        *outData = nullptr;
        *outCount = 0;
        *outStride = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto* archetype = cq->GetArchetype(static_cast<std::size_t>(archetypeIndex));
        if (!archetype)
            return GE_Result_InvalidArg;

        auto typeId = static_cast<GameEngine::ECS::ComponentTypeId>(componentTypeId);
        auto* handler = GameEngine::ECS::ComponentRegistry::GetHandler(typeId);
        if (!handler)
            return GE_Result_NotFound;

        const std::size_t stride = handler->GetComponentSize();
        const std::size_t totalEntities = archetype->GetEntityCount();

        if (entityOffset >= totalEntities)
            return GE_Result_Ok; // outCount stays 0

        // Walk chunks to find the one containing entityOffset. The walk uses
        // the CONST overload so chunks walked past are never write-granted
        // (P1 review F1: the mutable probe stamped columns in chunks 0..N on
        // every managed slice read); only the found chunk takes the mutable
        // (stamping) span that the caller may write through.
        const GameEngine::ECS::Archetype* constArchetype = archetype;
        const std::size_t chunkCount = archetype->GetChunkCount();
        std::size_t cumulative = 0;
        for (std::size_t c = 0; c < chunkCount; ++c)
        {
            auto [probePtr, chunkEntityCount] = constArchetype->GetChunkDataRaw(typeId, c);
            (void)probePtr;
            if (entityOffset < cumulative + chunkEntityCount)
            {
                auto [chunkPtr, grantedCount] = archetype->GetChunkDataRaw(typeId, c);
                if (!chunkPtr || grantedCount != chunkEntityCount)
                    return GE_Result_Fail;
                const std::size_t localOffset = entityOffset - cumulative;
                const std::size_t available = chunkEntityCount - localOffset;
                *outData = static_cast<uint8_t*>(chunkPtr) + localOffset * stride;
                *outCount = static_cast<uint32_t>(available);
                *outStride = static_cast<uint32_t>(stride);
                return GE_Result_Ok;
            }
            cumulative += chunkEntityCount;
        }

        return GE_Result_Ok; // offset past all data, outCount stays 0
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECSABI_CachedQueryGetEntityIdSlice(
    GE_Handle query, uint32_t archetypeIndex, uint32_t entityOffset,
    uint32_t maxCount, uint32_t** outEntityIds, uint32_t* outCount)
{
    try
    {
        if (!outEntityIds || !outCount)
            return GE_Result_InvalidArg;
        *outEntityIds = nullptr;
        *outCount = 0;
        if (query == 0)
            return GE_Result_InvalidArg;

        auto* cq = reinterpret_cast<GameEngine::ECS::CachedQuery*>(static_cast<uintptr_t>(query));
        if (!cq)
            return GE_Result_Fail;

        auto [ids, count] = cq->GetEntityIdSlice(
            static_cast<std::size_t>(archetypeIndex),
            static_cast<std::size_t>(entityOffset),
            static_cast<std::size_t>(maxCount));
        if (!ids)
            return GE_Result_Ok; // outCount stays 0

        *outEntityIds = ids;
        *outCount = static_cast<uint32_t>(count);
        return GE_Result_Ok;
    }
    GE_ECSABI_CATCH_ALL_FAIL
}

GE_API GE_Result GE_CDECL GE_ECS_GetInterface(uint32_t abiVersion, const void** outTable, uint32_t* outSizeBytes)
{
    try
    {
        if (!outTable || !outSizeBytes)
            return GE_Result_InvalidArg;

        const uint32_t major = GE_ECS_ABI_VERSION_MAJOR(abiVersion);
        const uint32_t curMajor = GE_ECS_ABI_VERSION_MAJOR(GE_ECS_ABI_VERSION_CURRENT);
        if (major != curMajor)
        {
            *outTable = nullptr;
            *outSizeBytes = 0;
            return GE_Result_NotFound;
        }

        static GE_ECS_Interface_v2 s_Iface;
        s_Iface.sizeBytes = sizeof(GE_ECS_Interface_v2);
        s_Iface.abiVersion = GE_ECS_ABI_VERSION_CURRENT;
        s_Iface.GetPrimaryWorld = reinterpret_cast<GE_ECSABI_GetPrimaryWorld_Fn>(&GE_ECSABI_GetPrimaryWorld);
        s_Iface.GetEntityCount = reinterpret_cast<GE_ECSABI_GetEntityCount_Fn>(&GE_ECSABI_GetEntityCount);
        s_Iface.CreateEmptyEntity = reinterpret_cast<GE_ECSABI_CreateEmptyEntity_Fn>(&GE_ECSABI_CreateEmptyEntity);
        s_Iface.CreatePrimitive = reinterpret_cast<GE_ECSABI_CreatePrimitive_Fn>(&GE_ECSABI_CreatePrimitive);
        s_Iface.IsEntityValid = reinterpret_cast<GE_ECSABI_IsEntityValid_Fn>(&GE_ECSABI_IsEntityValid);
        s_Iface.DestroyEntity = reinterpret_cast<GE_ECSABI_DestroyEntity_Fn>(&GE_ECSABI_DestroyEntity);
        s_Iface.SetName = reinterpret_cast<GE_ECSABI_SetName_Fn>(&GE_ECSABI_SetName);
        s_Iface.GetName = reinterpret_cast<GE_ECSABI_GetName_Fn>(&GE_ECSABI_GetName);
        s_Iface.GetTransform = reinterpret_cast<GE_ECSABI_GetTransform_Fn>(&GE_ECSABI_GetTransform);
        s_Iface.SetTransform = reinterpret_cast<GE_ECSABI_SetTransform_Fn>(&GE_ECSABI_SetTransform);
        s_Iface.RegisterBlobComponent = reinterpret_cast<GE_ECSABI_RegisterBlobComponent_Fn>(&GE_ECSABI_RegisterBlobComponent);
        s_Iface.GetComponentTypeIdByName = reinterpret_cast<GE_ECSABI_GetComponentTypeIdByName_Fn>(&GE_ECSABI_GetComponentTypeIdByName);
        s_Iface.SetComponentBytes = reinterpret_cast<GE_ECSABI_SetComponentBytes_Fn>(&GE_ECSABI_SetComponentBytes);
        s_Iface.GetComponentBytes = reinterpret_cast<GE_ECSABI_GetComponentBytes_Fn>(&GE_ECSABI_GetComponentBytes);
        s_Iface.RemoveComponent = reinterpret_cast<GE_ECSABI_RemoveComponent_Fn>(&GE_ECSABI_RemoveComponent);

        // v1.3: Cached chunk queries and structural change fencing
        s_Iface.CachedQueryCreate = reinterpret_cast<GE_ECSABI_CachedQueryCreate_Fn>(&GE_ECSABI_CachedQueryCreate);
        s_Iface.CachedQueryDestroy = reinterpret_cast<GE_ECSABI_CachedQueryDestroy_Fn>(&GE_ECSABI_CachedQueryDestroy);
        s_Iface.CachedQueryReset = reinterpret_cast<GE_ECSABI_CachedQueryReset_Fn>(&GE_ECSABI_CachedQueryReset);
        s_Iface.CachedQueryGetArchetypeInfo = reinterpret_cast<GE_ECSABI_CachedQueryGetArchetypeInfo_Fn>(&GE_ECSABI_CachedQueryGetArchetypeInfo);
        s_Iface.CachedQueryGetChunkData = reinterpret_cast<GE_ECSABI_CachedQueryGetChunkData_Fn>(&GE_ECSABI_CachedQueryGetChunkData);
        s_Iface.CachedQueryGetChunkEntityIds = reinterpret_cast<GE_ECSABI_CachedQueryGetChunkEntityIds_Fn>(&GE_ECSABI_CachedQueryGetChunkEntityIds);
        s_Iface.HasComponent = reinterpret_cast<GE_ECSABI_HasComponent_Fn>(&GE_ECSABI_HasComponent);
        s_Iface.SetDeferStructuralChanges = reinterpret_cast<GE_ECSABI_SetDeferStructuralChanges_Fn>(&GE_ECSABI_SetDeferStructuralChanges);
        s_Iface.FlushDeferredCommands = reinterpret_cast<GE_ECSABI_FlushDeferredCommands_Fn>(&GE_ECSABI_FlushDeferredCommands);

        // v1.4: Per-archetype component presence check
        s_Iface.CachedQueryArchetypeHasComponent = reinterpret_cast<GE_ECSABI_CachedQueryArchetypeHasComponent_Fn>(&GE_ECSABI_CachedQueryArchetypeHasComponent);

        // v1.5: Entity command buffer
        s_Iface.CreateEntityRaw = reinterpret_cast<GE_ECSABI_CreateEntityRaw_Fn>(&GE_ECSABI_CreateEntityRaw);
        s_Iface.DeferCommand = reinterpret_cast<GE_ECSABI_DeferCommand_Fn>(&GE_ECSABI_DeferCommand);

        // v1.6: Slice-based iteration
        s_Iface.CachedQueryGetComponentSlice = reinterpret_cast<GE_ECSABI_CachedQueryGetComponentSlice_Fn>(&GE_ECSABI_CachedQueryGetComponentSlice);
        s_Iface.CachedQueryGetEntityIdSlice = reinterpret_cast<GE_ECSABI_CachedQueryGetEntityIdSlice_Fn>(&GE_ECSABI_CachedQueryGetEntityIdSlice);

        // v1.7: Change-signaling chunk enumeration + read-only span
        s_Iface.CachedQueryGetChunkDataV2 = reinterpret_cast<GE_ECSABI_CachedQueryGetChunkDataV2_Fn>(&GE_ECSABI_CachedQueryGetChunkDataV2);
        s_Iface.CachedQueryGetChunkDataReadOnly = reinterpret_cast<GE_ECSABI_CachedQueryGetChunkDataReadOnly_Fn>(&GE_ECSABI_CachedQueryGetChunkDataReadOnly);

        // v2.1: blob component registration with a reflected field table
        s_Iface.RegisterBlobComponentWithSchema = reinterpret_cast<GE_ECSABI_RegisterBlobComponentWithSchema_Fn>(&GE_ECSABI_RegisterBlobComponentWithSchema);
        s_Iface.GetComponentDisabledTypeId = reinterpret_cast<GE_ECSABI_GetComponentDisabledTypeId_Fn>(&GE_ECSABI_GetComponentDisabledTypeId);
        s_Iface.SetEntityEnabled = reinterpret_cast<GE_ECSABI_SetEntityEnabled_Fn>(&GE_ECSABI_SetEntityEnabled);

        *outTable = &s_Iface;
        *outSizeBytes = sizeof(GE_ECS_Interface_v2);
        return GE_Result_Ok;
    }
    catch (...)
    {
        Logger::Log::Error("[ECSABI] GE_ECS_GetInterface swallowed a native exception");
        if (outTable)
            *outTable = nullptr;
        if (outSizeBytes)
            *outSizeBytes = 0;
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_SetHostEcsState(void* registryComponents,
                                               void* registryNames,
                                               void* registryHandlers)
{
    // Snapshot DLL-local registry count before redirect for diagnostics.
    // ComponentTypeId allocator no longer exists post-Phase-1b — type IDs are
    // consteval hashes, no per-DLL state to redirect.
    const auto orphanedRegistryEntries = GameEngine::ECS::ComponentRegistry::GetComponentCount();

    GameEngine::ECS::ComponentRegistry::SetExternalRegistryState(
        registryComponents, registryNames, registryHandlers);

    Logger::Log::Info("[ECSABI] GE_SetHostEcsState: ComponentRegistry redirected to host "
                      "(components={:x}, names={:x}, handlers={:x}); "
                      "orphaned {} DLL-local registry entries",
                      reinterpret_cast<uintptr_t>(registryComponents),
                      reinterpret_cast<uintptr_t>(registryNames),
                      reinterpret_cast<uintptr_t>(registryHandlers),
                      orphanedRegistryEntries);
    return GE_Result_Ok;
}

} // extern "C"
