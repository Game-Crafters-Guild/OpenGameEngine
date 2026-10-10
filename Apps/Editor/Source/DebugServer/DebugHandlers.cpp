#include "DebugServer/DebugHandlers.h"

#include "Automation/UiReplayCommandIds.h"
#include "AssetDatabase/IAssetDbCache.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/LensFlareImporter.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "AssetCore/GUID.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneIOContext.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "Core/DebugMetrics.h"
#include "Core/Engine.h"
#include "DebugServer/AssistantSessionHandlers.h"
#include "DebugServer/BenchSceneHandlers.h"
#include "DebugServer/CameraProjectionReport.h"
#include "Editor/CaptureOutputDirectory.h"
#include "DebugServer/ComponentValueReader.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/ElementPointerTarget.h"
#include "DebugServer/GpuToolingHandlers.h"
#include "DebugServer/GroundQueryDebugHandlers.h"
#include "DebugServer/JobSystemDebugHandlers.h"
#include "DebugServer/MarkupDebugHandlers.h"
#include "DebugServer/PerformDropRequest.h"
#include "DebugServer/SkyDebugHandlers.h"
#include "DebugServer/ThumbnailDebugHandlers.h"
#include "DebugServer/InjectedInput.h"
#include "DebugServer/OpenAssetPath.h"
#include "DebugServer/ParallaxDebugHandlers.h"
#include "DebugServer/PhysicsDebugHandlers.h"
#include "DebugServer/PlayModeRequest.h"
#include "DebugServer/PointerCoordsRequest.h"
#include "DebugServer/SceneDegradedReport.h"
#include "DebugServer/SceneHierarchyReport.h"
#include "DebugServer/SchemaComponentApply.h"
#include "DebugServer/RenderDocCapture.h"
#include "DebugServer/RenderGraphResourceFilter.h"
#include "DebugServer/ScreenshotCaptureWait.h"
#include "DebugServer/TerrainDebugHandlers.h"
#include "DebugServer/UITreeDump.h"
#include "DebugServer/ViewFocusRequest.h"
#include "DebugServer/WindowPrintCapture.h"
#include "UI/Interaction/DragDropManager.h"
#include "ECS/ComponentFactory.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ReflectionJson.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Assets/ShaderGlslOpen.h"
#include "EditorApplication.h"
#include "Search/UniversalSearchController.h"
#include "EditorChangeNotifications.h"
#include "GameViewController.h"
#include "MovieRecorderController.h"
#include "Video/VideoWriter.h"
#include "EditorContextMenu/InterceptableContextMenu.h"
#include "Platform/Display.h"
#include "EditorPanelIds.h"
#include "EditorPanelManager.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/DrawStreamLookupKey.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/LogLevel.h"
#include "Logger/RingBufferSink.h"
#include "Panels/AnimationWindowPanel.h"
#include "Panels/AssetViewPanel.h"
#include "Panels/AssetsPanel.h"
#include "Panels/BuildPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/GraphPanel.h"
#include "Graph/GraphKindChrome.h"
#include "Graph/GraphPanelFactory.h"
#include "Panels/SettingsPanel.h"
#include "Panels/SceneViewPanel.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "PlayMode/PlayModeManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Scene/SceneEditorController.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneView/SceneViewRenderCoordinator.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneViewController.h"
#include "SceneView/TerrainBrushTool.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainEntityProvisioning.h"
#include "TerrainECS/TerrainFactory.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainGrass/TerrainGrassClamps.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TableView.h"
#include "UI/Controls/TextField.h"
#include "UI/ResolvedStyle.h"
#include "UI/UiContext.h"
#include "UI/Utf8Helpers.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/DuplicateEntitiesCommand.h"
#include "UndoRedo/EntityComponentsEdit.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/SplineUndoHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Write one JSON value into a component's raw bytes for a single reflected field,
// dispatching on its FieldTypeId. Type-guarded so a mismatched JSON value is
// ignored rather than throwing. Float/Vec/Color accept a JSON array for the
// element-wise components (e.g. Light.Color = [r,g,b]).
// Returns false whenever the field was not written and the caller must say so: an
// AssetGuid/enum string that did not resolve (unknown asset path / unknown enumerator),
// an entity id naming no live entity, or a field type this writer has no representation
// for (Mat4/Bytes/Unknown). Reporting success for a write that changed nothing is what
// this contract exists to prevent. `world` supplies entity-handle liveness.
static bool WriteFieldFromJson(ECS::World& world, std::vector<uint8_t>& bytes,
                               const ECS::FieldInfo& f, const json& v)
{
    // The integer/enum stores below memcpy a value's low bytes into the field; that assumes a
    // little-endian host (every shipping target). A big-endian port would byte-swap.
    static_assert(std::endian::native == std::endian::little, "WriteFieldFromJson assumes little-endian");
    using ECS::FieldTypeId;
    const std::size_t off = f.Offset;
    const auto fits = [&](std::size_t n)
    { return off + n <= bytes.size(); };
    const auto writeFloats = [&](std::uint32_t count)
    {
        if (!v.is_array())
            return;
        for (std::uint32_t i = 0; i < count && i < v.size(); ++i)
        {
            if (!v[i].is_number() || !fits(i * 4 + 4))
                continue;
            float x = v[i].get<float>();
            std::memcpy(bytes.data() + off + i * 4, &x, 4);
        }
    };
    const auto storeInt = [&]<class T>()
    {
        if (v.is_number() && fits(sizeof(T)))
        {
            auto x = static_cast<T>(v.get<long long>());
            std::memcpy(bytes.data() + off, &x, sizeof(T));
        }
    };
    // Enum-by-name: a reflected enum field collapses to its underlying integer Type but
    // carries an EnumNames table. Accept the enumerator NAME the scene format + inspector
    // use (e.g. RenderMode="Fullscreen"), resolving it to the integer value. A numeric
    // value falls through to the normal integer path below.
    //
    // Matched case-INSENSITIVELY, like the field name this value arrived under and like the
    // scene loader, which lowercases before comparing — "blend" loading from a .scene and
    // failing over IPC is the get/set disagreement this surface exists to remove.
    // ReflectedEnumNameDrift pins the precondition: no reflected enum has two enumerators
    // differing only by case.
    if (!f.EnumNames.empty() && v.is_string() && f.Size <= sizeof(std::int64_t) && fits(f.Size))
    {
        const std::string name = v.get<std::string>();
        for (const auto& e : f.EnumNames)
        {
            if (IEquals(e.Name, name))
            {
                std::memcpy(bytes.data() + off, &e.Value, f.Size); // low bytes (LE, asserted above)
                return true;
            }
        }
        // Accept a numeric STRING (e.g. "1") as the underlying integer too, matching the lenient
        // JSON-number path — callers/scripts sometimes quote enum values.
        long long num = 0;
        if (auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), num);
            ec == std::errc{} && ptr == name.data() + name.size())
        {
            std::memcpy(bytes.data() + off, &num, f.Size);
            return true;
        }
        return false; // not a known enumerator name or integer → report rather than silently no-op
    }
    switch (f.Type)
    {
    case FieldTypeId::Bool:
        if (fits(1))
            bytes[off] = (v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0.0)) ? 1 : 0;
        break;
    case FieldTypeId::Int8:
        storeInt.operator()<std::int8_t>();
        break;
    case FieldTypeId::UInt8:
        storeInt.operator()<std::uint8_t>();
        break;
    case FieldTypeId::Int16:
        storeInt.operator()<std::int16_t>();
        break;
    case FieldTypeId::UInt16:
        storeInt.operator()<std::uint16_t>();
        break;
    case FieldTypeId::Int32:
        storeInt.operator()<std::int32_t>();
        break;
    case FieldTypeId::UInt32:
        storeInt.operator()<std::uint32_t>();
        break;
    case FieldTypeId::Int64:
        storeInt.operator()<std::int64_t>();
        break;
    case FieldTypeId::UInt64:
        storeInt.operator()<std::uint64_t>();
        break;
    case FieldTypeId::Double:
        if (v.is_number() && fits(8))
        {
            double x = v.get<double>();
            std::memcpy(bytes.data() + off, &x, 8);
        }
        break;
    case FieldTypeId::Float:
        if (v.is_array())
            writeFloats(f.Size / 4); // float[N] (e.g. Color[3])
        else if (v.is_number() && fits(4))
        {
            float x = v.get<float>();
            std::memcpy(bytes.data() + off, &x, 4);
        }
        break;
    case FieldTypeId::Vec2:
        writeFloats(2);
        break;
    case FieldTypeId::Vec3:
        writeFloats(3);
        break;
    case FieldTypeId::Vec4:
    case FieldTypeId::Quat:
    case FieldTypeId::Color:
        writeFloats(4);
        break;
    case FieldTypeId::String:
        if (v.is_string() && f.Size > 0 && fits(f.Size))
        {
            const std::string s = v.get<std::string>();
            const std::size_t n = std::min<std::size_t>(s.size(), f.Size - 1);
            std::memcpy(bytes.data() + off, s.data(), n);
            bytes[off + n] = 0;
        }
        break;
    case FieldTypeId::AssetGuid:
    {
        // An AssetRef<T> (MaterialRef/ModelRef/TextureRef) is a 16-byte GUID. Accept a GUID
        // string or {"guid":"..."}; the component write-back re-resolves it during extraction
        // (e.g. the renderer's MaterialRegistry::Find), so set_component can assign a material.
        const json* g = v.is_string()                           ? &v
                        : (v.is_object() && v.contains("guid"))  ? &v.at("guid")
                                                                 : nullptr;
        if (!g || !g->is_string())
            return v.is_null(); // null = leave the ref as-is (ok); any other non-string is malformed -> report
        if (!fits(GUID::kSize))
            break;
        const std::string s = g->get<std::string>();
        if (s.empty())
        {
            // "" CLEARS the reference. It is what a read emits for an unset ref, so this is the
            // half that makes get_entity_components output writable back: a payload carrying an
            // unset ref has to land as unset, not silently keep whatever the target already held.
            // (JSON null, above, means "no value supplied" and still leaves the ref alone; the
            // read shape never emits it.)
            std::memset(bytes.data() + off, 0, GUID::kSize);
            return true;
        }
        // The GUID string ctor fills zeros (not throw) on a malformed string. If it isn't a
        // raw GUID, treat the string as an asset PATH and resolve it (so MCP can assign any
        // AssetRef<> by path, e.g. "UI/Hud.uxml", matching the scene format).
        GUID guid{s};
        if (guid.IsNull())
            guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(std::filesystem::path(s));
        if (!guid.IsNull())
        {
            std::memcpy(bytes.data() + off, guid.GetData().data(), GUID::kSize);
            return true;
        }
        // Non-empty string that's neither a raw GUID nor a resolvable asset path: leave the
        // existing ref untouched and report it (restores the diagnostic the deleted UIDocument
        // special-case had) instead of a silent no-op masquerading as success.
        return false;
    }
    case FieldTypeId::EntityHandle:
    {
        if (!fits(sizeof(std::uint32_t)))
            return false;
        // JSON null clears the link. That IS a write, unlike the zero below.
        if (v.is_null())
        {
            const ECS::EntityHandle cleared{};
            std::memcpy(bytes.data() + off, &cleared.id, sizeof cleared.id);
            return true;
        }
        if (!v.is_number())
            return false;
        const auto raw = v.get<std::int64_t>();
        if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max())
            return false;
        // EntityHandle::IsValid only tests the kInvalidEntity sentinel, so a ZEROED id
        // passes it while naming no entity — versions are minted from 1, so (0,0) never
        // exists. Liveness lives in the World; ask it, or a raw byte round-trip would
        // store a handle that every reader then silently ignores.
        const ECS::EntityHandle handle{static_cast<std::uint32_t>(raw)};
        if (!world.IsValid(handle))
            return false;
        std::memcpy(bytes.data() + off, &handle.id, sizeof handle.id);
        return true;
    }
    case FieldTypeId::Mat4:
    case FieldTypeId::Bytes:
    case FieldTypeId::Unknown:
    default:
        return false; // no JSON representation here — report rather than claim a write
    }
    return true; // written, or an acceptable type-mismatch no-op
}

// Set fields on a reflected component from a JSON object, keyed by field name
// (exact or case-insensitive — set_component historically used camelCase keys
// like "fovY" for the PascalCase field FovY). Adds the component (default-
// constructed via the factory) if missing. Returns false if the type has no
// field table.
static bool SetReflectedComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                        ECS::ComponentTypeId typeId, const json& values,
                                        std::vector<std::string>* outUnapplied = nullptr)
{
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (fields.empty())
        return false;

    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
    {
        if (!ECS::ComponentFactory::Create(*world, entity, typeId))
            return false;
        if (!world->CaptureComponentBytes(entity, typeId, bytes))
            return false;
    }

    if (values.is_object())
    {
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            bool matched = false;
            for (const ECS::FieldInfo& f : fields)
            {
                if (f.Name == it.key() || IEquals(f.Name, it.key()))
                {
                    matched = true;
                    // Honor field policy: don't let set_component write fields a
                    // component marked internal (Hidden) or non-editable (ReadOnly).
                    if (ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden | ECS::FieldFlags::ReadOnly))
                    {
                        if (outUnapplied)
                            outUnapplied->push_back(it.key()); // read-only/hidden: not settable here
                    }
                    else if (!WriteFieldFromJson(*world, bytes, f, it.value()) && outUnapplied)
                    {
                        // Bad asset path / enum name, an entity id naming no live entity,
                        // or a field type with no JSON form (Mat4/Bytes).
                        outUnapplied->push_back(it.key());
                    }
                    break;
                }
            }
            if (!matched && outUnapplied)
                outUnapplied->push_back(it.key()); // no field by that name on this component
        }
    }
    return world->ApplyComponentBytesImmediate(entity, typeId, bytes);
}

// Apply named field values from JSON onto a component attached to an entity.
// Adds the component if it does not already exist. Returns a RefuseRequest
// refusal on failure or a null json (is_null()) on success. Callers should check
// result.is_null() to distinguish success from error.
// Result of ApplyComponentValues. Error is null on success (the plain-json
// contract error returns already use; the implicit constructor keeps them
// terse). A branch that applies a data-only fast path sets SuppressNotify so
// set_component doesn't announce the write — the policy lives next to the
// branch that implements it. Defaulting to notify fails safe: a forgotten
// flag costs a redundant inspector rebuild, never silent UI staleness.
struct ComponentApplyResult
{
    json Error;
    bool SuppressNotify = false;

    ComponentApplyResult(json error = {}) : Error(std::move(error)) {}
};

// The one message a component write returns for keys that named no field. Shared by the
// reflected path, the hand-written branches and the create_entity terrain payload, so a
// caller cannot tell from the wording which internal route their component took.
static json UnappliedFieldsError(std::string_view operation, const std::string& componentName,
                                 const std::vector<std::string>& unapplied)
{
    std::string joined;
    for (std::size_t i = 0; i < unapplied.size(); ++i)
        joined += (i ? ", " : "") + unapplied[i];
    return Editor::RefuseRequest(std::string(operation) + " " + componentName +
                                     ": fields not applied (unknown name, read-only, "
                                     "unresolved asset/enum, dead entity id, or a field "
                                     "type this path cannot express): " +
                                     joined);
}

// The replacement for a retired pre-volume modifier name, or null when the name is
// not one. The five names are no longer components — only a scene FORMAT the
// migration schemas still parse — but those schemas stay registered, so a retired
// name reaching the generic schema fallback would FIND one: the lone-ApplyProperty
// path would park a half-parsed block on the entity and answer ok, and a later save
// would re-emit a legacy block that the next load turns back into a live modifier.
// Silent at the call, materialised a save later. Refused instead, naming the
// replacement (terrain-editing-ux-design 1.2).
//
// Only this surface refuses. The other schema callers — the preserved-field
// resolver and the three VCS revert sites — REPLAY already-authored data, which
// is exactly what the migration exists to consume. set_component is new authoring.
//
// Matched case-INSENSITIVELY, because every name resolver this sits in front of
// is: SceneSchemaRegistry::Find lowercases through ToKey, and
// ComponentFieldRegistry::FindByName documents IEquals matching for exactly this
// caller (user-typed MCP names). An exact compare here would refuse
// "TerrainFlattenModifier" and wave "terrainflattenmodifier" straight through to
// the schema it still resolves to.
static const char* RetiredModifierReplacement(const std::string& componentName)
{
    if (IEquals(componentName, "TerrainFlattenModifier"))
        return "TerrainModifierVolume + TerrainFlattenEffect";
    if (IEquals(componentName, "TerrainNoiseModifier"))
        return "TerrainModifierVolume + TerrainNoiseEffect";
    if (IEquals(componentName, "TerrainStampModifier"))
        return "TerrainModifierVolume + TerrainStampEffect";
    if (IEquals(componentName, "TerrainPaintLayerModifier"))
        return "TerrainModifierVolume + TerrainPaintLayerEffect";
    if (IEquals(componentName, "TerrainSplineModifier"))
        return "TerrainModifierVolume (Shape = SplineArea) + TerrainFlattenEffect";
    return nullptr;
}

// Range invariants the reflected writer cannot know about. SetReflectedComponentFields lands raw
// bytes with no bounds checking, so a component whose fields carry ranges runs its own clamp pass
// here — the SAME pass its scene schema runs after parsing a property, so a value set over IPC and
// the same value loaded from a .scene land identically. Whole-component and idempotent, so it needs
// no knowledge of which fields the write touched.
static void ApplyReflectedComponentInvariants(ECS::World* world, ECS::EntityHandle entity,
                                              ECS::ComponentTypeId typeId)
{
    if (typeId == ECS::GetComponentTypeId<Components::TerrainGrass>())
    {
        if (auto* grass = world->GetComponentForWrite<Components::TerrainGrass>(entity))
            TerrainGrass::ClampFields(*grass);
    }
}

// PhysicsBody.motionType from its enumerator name or integer index; nullopt for anything else,
// so a typo is refused instead of landing as Dynamic.
static std::optional<Physics::MotionType> ParseMotionType(const json& value)
{
    if (value.is_string())
    {
        const std::string& name = value.get_ref<const std::string&>();
        if (name == "Static")
            return Physics::MotionType::Static;
        if (name == "Kinematic")
            return Physics::MotionType::Kinematic;
        if (name == "Dynamic")
            return Physics::MotionType::Dynamic;
        return std::nullopt;
    }
    if (value.is_number_integer())
    {
        const int64_t index = value.get<int64_t>();
        if (index >= static_cast<int64_t>(Physics::MotionType::Static) &&
            index <= static_cast<int64_t>(Physics::MotionType::Dynamic))
            return static_cast<Physics::MotionType>(index);
    }
    return std::nullopt;
}

static ComponentApplyResult ApplyComponentFields(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    // Every hand-written branch below understands a fixed set of field names, so it reads
    // through the tracking reader and the tail reports whatever it never looked at. Without
    // that, a misspelled or wrong-case key is indistinguishable from an absent one and
    // set_component answers `ok: true` for a write that changed nothing.
    Editor::ComponentValueReader fields(values);
    bool suppressNotify = false;

    if (componentName == "Transform")
    {
        // Merge onto the existing transform: a partial write ({position} only)
        // must not reset rotation/scale to defaults — a scale reset silently
        // collapses scale-derived volumes (reflection-probe influence boxes).
        Mathematics::Vector3 pos{};
        Mathematics::Quaternion rot{};
        Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
        if (const auto* existing = world->GetComponent<Components::Transform>(entity))
        {
            pos = existing->GetPosition();
            rot = existing->GetRotation();
            scl = existing->GetScale();
        }
        if (fields.Has("position"))
        {
            auto& p = fields["position"];
            pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
        }
        if (fields.Has("rotation"))
        {
            auto& r = fields["rotation"];
            rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
        }
        if (fields.Has("scale"))
        {
            auto& s = fields["scale"];
            scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
        }
        auto xf = Components::Transform::FromTRS(pos, rot, scl);
        if (!world->GetComponent<Components::Transform>(entity))
            world->AddComponentImmediate(entity, xf);
        else
            *world->GetComponentForWrite<Components::Transform>(entity) = xf;
    }
    else if (componentName == "WorldSectorCoord")
    {
        // Debug/IPC tagging surface for camera-relative rendering (Earth-scale).
        // WorldSectorCoord is also user-addable in the Inspector; this IPC path
        // is the test/automation equivalent (place a mesh far out at a precise
        // sector + small local so it renders AND picks at the same world spot).
        // Merges onto any existing sector.
        Components::WorldSectorCoord sc{};
        if (const auto* existing = world->GetComponent<Components::WorldSectorCoord>(entity))
            sc = *existing;
        sc.x = fields.Value("x", sc.x);
        sc.y = fields.Value("y", sc.y);
        sc.z = fields.Value("z", sc.z);
        if (!world->GetComponent<Components::WorldSectorCoord>(entity))
            world->AddComponentImmediate(entity, sc);
        else
            *world->GetComponentForWrite<Components::WorldSectorCoord>(entity) = sc;
    }
    else if (componentName == "Parent")
    {
        // No reflected field table (EntityHandle field, [DoNotSerialize]) — apply directly.
        // Deliberately emits no EditorChangeNotifications: this models a gameplay/script
        // reparent (the ECS data-only set fast path), which the Hierarchy panel picks up
        // via its Changed<Parent> scan, not the editor command layer. parent=0 un-parents
        // (stored as the invalid-handle sentinel, matching ReparentCommand's Parent{}).
        const uint32_t parentId = fields.Value("parent", 0u);
        const ECS::EntityHandle parentHandle =
            parentId != 0 ? ECS::EntityHandle(parentId) : ECS::EntityHandle::Invalid();
        if (parentId != 0 && !world->IsValid(parentHandle))
            return Editor::RefuseRequest("Invalid parent entity");
        if (parentHandle == entity)
            return Editor::RefuseRequest("Cannot parent an entity to itself");
        world->AddComponentImmediate(entity, Components::Parent{parentHandle});
        suppressNotify = true;
    }
    else if (componentName == "SplineComponent")
    {
        auto* splineService = SplineECS::SplineService::TryGet();
        if (!splineService)
            return Editor::RefuseRequest("SplineService not available");

        bool closed = fields.Value("closed", false);
        float defaultRadius = fields.Value("DefaultRadius", 5.0f);
        uint32_t typeVal = fields.Value("type", 0u);
        auto splineType = static_cast<Spline::SplineType>(typeVal);

        // The entity's own spline is rewritten in place, keeping its handle, so set_component's
        // spline snapshot can put the old points back on undo; a first set creates one.
        auto* existing = world->GetComponent<Components::SplineComponent>(entity);
        const bool reuse = existing && splineService->IsValid(SplineECS::SplineHandle(existing->SplineDataIndex,
                                                                                      existing->SplineDataGeneration));
        const SplineECS::SplineHandle handle =
            reuse ? SplineECS::SplineHandle(existing->SplineDataIndex, existing->SplineDataGeneration)
                  : splineService->CreateSpline(splineType, closed);
        auto* data = splineService->GetSplineData(handle);
        if (!data)
            return Editor::RefuseRequest("Failed to create spline data");
        data->Type = splineType;
        data->Closed = closed;
        data->Points.clear();

        if (fields.Has("points") && fields["points"].is_array())
        {
            for (const auto& pt : fields["points"])
            {
                float x = pt.value("x", 0.0f);
                float y = pt.value("y", 0.0f);
                float z = pt.value("z", 0.0f);
                float r = pt.value("radius", defaultRadius);
                data->AddPoint(Mathematics::Vector3(x, y, z), r);
            }
        }
        data->MarkDirty();
        Spline::RebuildSplineCache(*data);

        Components::SplineComponent sc{};
        sc.SplineDataIndex = handle.Index();
        sc.SplineDataGeneration = handle.Generation();
        sc.DefaultRadius = defaultRadius;

        if (!existing)
            world->AddComponentImmediate(entity, sc);
        else
            *world->GetComponentForWrite<Components::SplineComponent>(entity) = sc;
    }
    else if (componentName == "ParticleCollisionEventsBuffer")
    {
        if (!world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
            world->AddComponentImmediate(entity, Components::ParticleCollisionEventsBuffer{});
        if (auto* c = world->GetComponentForWrite<Components::ParticleCollisionEventsBuffer>(entity))
            c->Clear();
    }
    else if (componentName == "PhysicsBody")
    {
        // Physics components live under PhysicsECS/Include/PhysicsECS/Components,
        // outside the component scanner's reflected glob, so they need explicit
        // handlers here (the reflected fallback below can't find them).
        // motionType is validated before anything is written, so a refused value
        // leaves the entity untouched.
        std::optional<Physics::MotionType> motionType;
        if (fields.Has("motionType"))
        {
            motionType = ParseMotionType(fields["motionType"]);
            if (!motionType)
                return Editor::RefuseRequest("PhysicsBody.motionType must be \"Static\", \"Kinematic\", "
                                             "\"Dynamic\" or 0/1/2; got " + fields["motionType"].dump());
        }
        if (!world->HasComponent<Components::PhysicsBody>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsBody{});
        auto* c = world->GetComponentForWrite<Components::PhysicsBody>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsBody");
        if (fields.Has("mass"))
            c->mass = fields["mass"].get<float>();
        if (fields.Has("linearDamping"))
            c->linearDamping = fields["linearDamping"].get<float>();
        if (fields.Has("angularDamping"))
            c->angularDamping = fields["angularDamping"].get<float>();
        if (fields.Has("gravityScale"))
            c->gravityScale = fields["gravityScale"].get<float>();
        if (fields.Has("centerOfMassOffset") && fields["centerOfMassOffset"].is_object())
        {
            const auto& o = fields["centerOfMassOffset"];
            c->centerOfMassOffsetX = o.value("x", c->centerOfMassOffsetX);
            c->centerOfMassOffsetY = o.value("y", c->centerOfMassOffsetY);
            c->centerOfMassOffsetZ = o.value("z", c->centerOfMassOffsetZ);
        }
        if (fields.Has("centerOfMassOffsetX"))
            c->centerOfMassOffsetX = fields["centerOfMassOffsetX"].get<float>();
        if (fields.Has("centerOfMassOffsetY"))
            c->centerOfMassOffsetY = fields["centerOfMassOffsetY"].get<float>();
        if (fields.Has("centerOfMassOffsetZ"))
            c->centerOfMassOffsetZ = fields["centerOfMassOffsetZ"].get<float>();
        if (motionType)
            c->motionType = *motionType;
        c->initialized = false;
    }
    else if (componentName == "PhysicsCollider")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        auto* c = world->GetComponentForWrite<Components::PhysicsCollider>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add PhysicsCollider");
        if (fields.Has("isTrigger"))
            c->isTrigger = fields["isTrigger"].get<bool>();
    }
    else if (componentName == "BoxColliderShape")
    {
        // A collider shape implies a PhysicsCollider (required component).
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::BoxColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::BoxColliderShape{});
        auto* c = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add BoxColliderShape");
        if (fields.Has("halfExtentsX"))
            c->halfExtentsX = fields["halfExtentsX"].get<float>();
        if (fields.Has("halfExtentsY"))
            c->halfExtentsY = fields["halfExtentsY"].get<float>();
        if (fields.Has("halfExtentsZ"))
            c->halfExtentsZ = fields["halfExtentsZ"].get<float>();
    }
    else if (componentName == "SphereColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::SphereColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::SphereColliderShape{});
        auto* c = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add SphereColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
    }
    else if (componentName == "CapsuleColliderShape")
    {
        if (!world->HasComponent<Components::PhysicsCollider>(entity))
            world->AddComponentImmediate(entity, Components::PhysicsCollider{});
        if (!world->HasComponent<Components::CapsuleColliderShape>(entity))
            world->AddComponentImmediate(entity, Components::CapsuleColliderShape{});
        auto* c = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
        if (!c)
            return Editor::RefuseRequest("Could not add CapsuleColliderShape");
        if (fields.Has("radius"))
            c->radius = fields["radius"].get<float>();
        if (fields.Has("halfHeight"))
            c->halfHeight = fields["halfHeight"].get<float>();
        if (fields.Has("axis"))
            c->axis = static_cast<uint8>(fields["axis"].get<int>());
    }
    else
    {
        // UIDocument, TerrainGrass and every other reflected component (incl. AssetRef<> fields
        // by GUID or path and enum fields by name) flow through the generic reflection path
        // below — no per-component special-case needed since WriteFieldFromJson learned
        // path-resolve + enum-by-name. A hand-written branch here would shadow the reflected
        // field table and silently cover fewer fields than the component has.
        ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(componentName);
        std::vector<std::string> unapplied;
        if (typeId != 0 && SetReflectedComponentFields(world, entity, typeId, values, &unapplied))
        {
            // Before reporting anything: the bytes are already in the world, so a partially
            // applied write must not leave out-of-range values behind on the error path.
            ApplyReflectedComponentInvariants(world, entity, typeId);
            // The component applied, but report any keys that didn't so MCP/LLM callers
            // don't read a silent no-op as success.
            if (!unapplied.empty())
                return UnappliedFieldsError("set_component", componentName, unapplied);
            return json{};
        }
        // Schema-only components (Terrain, ...) have no reflected field table; drive them
        // through the same hand-written schema the .scene loader uses. Reflected components
        // already returned above — PostProcessVolume, SkyEnvironment and VolumetricFogEffect
        // are all reflected, so their schema entries are unreachable from here.
        if (const char* replacement = RetiredModifierReplacement(componentName))
            return Editor::RefuseRequest(componentName + " is retired; set " + replacement +
                                             " instead");
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(componentName))
        {
            const std::string refusal = Editor::ApplyComponentViaSchema(*world, entity, componentName, *schema, values);
            return refusal.empty() ? json{} : Editor::RefuseRequest(refusal);
        }
        return Editor::RefuseRequest("Unknown component: " + componentName);
    }

    // A hand-written branch ran. Anything it never asked about wrote nothing, so report it
    // in the same words the reflected path uses rather than answering `ok` for a no-op.
    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("set_component", componentName, unconsumed);

    ComponentApplyResult applied;
    applied.SuppressNotify = suppressNotify;
    return applied;
}

// Resolve an IPC component name to its ECS type id: reflected components via
// the field registry, schema-only ones (Terrain, PostProcessVolume, ...) via
// the ECS component-name registry (suffix match tolerates namespaced names).
static ECS::ComponentTypeId ResolveComponentTypeIdByName(const std::string& componentName)
{
    if (const ECS::ComponentTypeId reflected = ECS::ComponentFieldRegistry::FindByName(componentName);
        reflected != 0)
        return reflected;
    const std::string suffix = "::" + componentName;
    for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
    {
        const bool match = (regName == componentName) ||
                           (regName.size() >= suffix.size() &&
                            regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
        if (match)
            return ECS::ComponentRegistry::GetComponentTypeId(regName);
    }
    return 0;
}

// A component's on/off state is its ECS::ComponentDisabled tag, not a field, but a write
// still names it "enabled" (or "Enabled"). The key is taken out of what the field writers see
// and applied as the tag once the rest of the write landed — for a type that switches through
// the tag (ComponentRegistry::SwitchesThroughDisabledTag). Any other type's key goes to the
// field writers: a post-process effect's or a terrain effect's own Enabled field, or, for a type
// declared NotToggleable, a field it does not have.
static ComponentApplyResult ApplyComponentValues(ECS::World* world, ECS::EntityHandle entity,
                                                 const std::string& componentName, const json& values)
{
    const ECS::ComponentTypeId typeId = ResolveComponentTypeIdByName(componentName);
    if (!values.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return ApplyComponentFields(world, entity, componentName, values);

    json fieldValues = values;
    std::optional<bool> enabled;
    for (const char* key : {"enabled", "Enabled"})
    {
        if (!fieldValues.contains(key))
            continue;
        if (!fieldValues[key].is_boolean())
            return Editor::RefuseRequest(componentName + "." + key + " must be true or false");
        enabled = fieldValues[key].get<bool>();
        fieldValues.erase(key);
    }

    ComponentApplyResult applied = ApplyComponentFields(world, entity, componentName, fieldValues);
    if (applied.Error.is_null() && enabled)
        world->SetComponentEnabledImmediate(entity, typeId, *enabled);
    return applied;
}

static json CreateDefaultTerrainForEntity(ECS::World* world, ECS::EntityHandle entity, const json& values)
{
    if (!world || !entity.IsValid())
        return Editor::RefuseRequest("Invalid terrain entity");

    // Same key-consumption contract set_component applies: a misspelled key here would
    // silently hand back a DEFAULT terrain (a planar one for a caller who asked for a
    // planet), which is exactly the kind of no-op that reads as success.
    Editor::ComponentValueReader fields(values);

    Components::Terrain terrainComp{};
    terrainComp.SizeX = fields.Value("SizeX", 256.0f);
    terrainComp.SizeZ = fields.Value("SizeZ", 256.0f);
    terrainComp.HeightScale = fields.Value("HeightScale", 64.0f);
    terrainComp.SamplesPerMeter = fields.Value("SamplesPerMeter", terrainComp.SamplesPerMeter);
    terrainComp.StreamingRadius = fields.Value("StreamingRadius", terrainComp.StreamingRadius);
    terrainComp.Domain = fields.Value("Domain", 0) == 1
                             ? Components::TerrainDomain::Spherical
                             : Components::TerrainDomain::Planar;
    terrainComp.PlanetRadius = fields.Value("PlanetRadius", terrainComp.PlanetRadius);
    terrainComp.TargetPixelError = fields.Value("TargetPixelError", terrainComp.TargetPixelError);
    terrainComp.SeaLevel = fields.Value("SeaLevel", terrainComp.SeaLevel);
    terrainComp.CastShadows = fields.Value("CastShadows", terrainComp.CastShadows);
    terrainComp.ReceiveShadows = fields.Value("ReceiveShadows", terrainComp.ReceiveShadows);
    const bool terrainEnabled = fields.Value("Enabled", true);

    // Base relief is a companion component now (attached only to a planet). Accept the
    // legacy PlanetRelief* keys on the create payload so existing IPC callers keep working.
    Components::TerrainPlanetRelief reliefComp{};
    reliefComp.Amplitude = fields.Value("PlanetReliefAmplitude", reliefComp.Amplitude);
    reliefComp.Frequency = fields.Value("PlanetReliefFrequency", reliefComp.Frequency);
    reliefComp.Octaves = fields.Value("PlanetReliefOctaves", reliefComp.Octaves);

    if (const std::vector<std::string> unconsumed = fields.UnconsumedKeys(); !unconsumed.empty())
        return UnappliedFieldsError("create_entity", "Terrain", unconsumed);

    // Shared with the create menu (TerrainECS::ProvisionTerrainEntity says what it does: the
    // terrain's components and data, the scene's default surface-rules volume, and a refusal
    // for a size past the extent limit, which the caller gets back).
    if (std::string refusal = Editor::ProvisionTerrainEntity(*world, entity, terrainComp, reliefComp); !refusal.empty())
        return Editor::RefuseRequest(refusal);
    if (!terrainEnabled)
        ECS::Entity(world, entity).SetEnabled<Components::Terrain>(false);

    return json{};
}

// create_entity's write: a new entity with its name, its transform from the top-level
// position/rotation/scale, its parent and the requested components. Returns a refusal or a
// null json; `outEntity` is the new entity either way. Every component lands immediately, so
// the undo step committed after it captures them.
static json BuildRequestedEntity(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                 ECS::EntityHandle& outEntity)
{
    auto entity = world->Create();
    outEntity = entity.GetHandle();

    // Set name if provided.
    std::string name = params.value("name", "");
    if (!name.empty())
    {
        Components::Name nameComp{};
        std::strncpy(nameComp.value, name.c_str(), sizeof(nameComp.value) - 1);
        nameComp.value[sizeof(nameComp.value) - 1] = '\0';
        world->AddComponentImmediate(entity.GetHandle(), nameComp);
    }

    // Always add a Transform. Top-level position/rotation/scale default to identity.
    Mathematics::Vector3 pos{};
    Mathematics::Quaternion rot{};
    Mathematics::Vector3 scl{1.0f, 1.0f, 1.0f};
    if (params.contains("position") && params["position"].is_object())
    {
        auto& p = params["position"];
        pos = Mathematics::Vector3(p.value("x", 0.0f), p.value("y", 0.0f), p.value("z", 0.0f));
    }
    if (params.contains("rotation") && params["rotation"].is_object())
    {
        auto& r = params["rotation"];
        rot = Mathematics::Quaternion(r.value("w", 1.0f), r.value("x", 0.0f), r.value("y", 0.0f), r.value("z", 0.0f));
    }
    if (params.contains("scale") && params["scale"].is_object())
    {
        auto& s = params["scale"];
        scl = Mathematics::Vector3(s.value("x", 1.0f), s.value("y", 1.0f), s.value("z", 1.0f));
    }
    world->AddComponentImmediate(entity.GetHandle(), Components::Transform::FromTRS(pos, rot, scl));

    if (parentHandle.IsValid())
        world->AddComponentImmediate(entity.GetHandle(), Components::Parent{parentHandle});

    // Set components if provided.
    if (params.contains("components") && params["components"].is_object())
    {
        auto& comps = params["components"];
        if (comps.contains("Terrain"))
        {
            const auto& terrainValues = comps["Terrain"].is_object() ? comps["Terrain"] : json::object();
            auto err = CreateDefaultTerrainForEntity(world, entity.GetHandle(), terrainValues);
            if (!err.is_null())
                return err;
        }

        for (auto it = comps.begin(); it != comps.end(); ++it)
        {
            // Transform is already set from top-level position/rotation/scale.
            if (it.key() == "Transform" || it.key() == "Terrain")
                continue;
            const auto& values = it.value().is_object() ? it.value() : json::object();
            // SuppressNotify is irrelevant here: create_entity announces the
            // whole batch with one WorldStructureChanged.
            auto applied = ApplyComponentValues(world, entity.GetHandle(), it.key(), values);
            if (!applied.Error.is_null())
                return applied.Error;
        }
    }
    return json{};
}

// A snapshot edit of the spline `entity` already has, for a set_component SplineComponent that
// rewrites it in place; inactive when the entity has no live spline.
static Editor::UndoRedoService::InteractiveEdit BeginExistingSplineEdit(Editor::UndoRedoService& undo, ECS::World* world,
                                                                        ECS::EntityHandle entity,
                                                                        Editor::EditorChangeNotifications* notifications,
                                                                        const std::string& label)
{
    auto* service = SplineECS::SplineService::TryGet();
    const auto* spline = world->GetComponent<Components::SplineComponent>(entity);
    if (!service || !spline)
        return {};
    const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
    if (!service->IsValid(handle))
        return {};
    return undo.BeginInteractiveEdit(
        label, Editor::SplineUndo::MakeSplineEditableSnapshotTarget(service, handle, world, entity, notifications, label));
}

// set_component's write as one undo step (with `undo`): the entity's components before and after
// (CommitEntityComponentsEdit) and, for a SplineComponent, the points of the spline it rewrites
// in place, which live in the spline service rather than in component bytes. A write refused
// part-way records the keys it applied before refusing; one refused before writing records nothing.
static ComponentApplyResult CommitComponentWrite(Editor::UndoRedoService* undo,
                                                 Editor::EditorChangeNotifications* notifications, ECS::World* world,
                                                 ECS::EntityHandle entity, const std::string& componentName,
                                                 const json& values)
{
    const std::string label = "Set " + componentName;
    ComponentApplyResult applied;
    const auto write = [&]() {
        (void)Editor::CommitEntityComponentsEdit(*world, entity, undo, notifications, label, [&]() {
            applied = ApplyComponentValues(world, entity, componentName, values);
            return applied.Error.is_null();
        });
    };
    if (!undo)
    {
        write();
        return applied;
    }
    Editor::CommitGenericEdit(*world, *undo, label, [&]() {
        Editor::UndoRedoService::InteractiveEdit splineEdit;
        if (componentName == "SplineComponent")
            splineEdit = BeginExistingSplineEdit(*undo, world, entity, notifications, label);
        write();
        splineEdit.Commit();
    });
    return applied;
}

// BuildRequestedEntity, or nothing: a refused build removes the entity it began, so the refusal
// leaves the world as it was before the request.
static json BuildRequestedEntityOrNone(ECS::World* world, const json& params, ECS::EntityHandle parentHandle,
                                       ECS::EntityHandle& outEntity)
{
    json refusal = BuildRequestedEntity(world, params, parentHandle, outEntity);
    if (!refusal.is_null() && world->IsValid(outEntity))
        world->DestroyEntityImmediate(outEntity);
    return refusal;
}

static std::string EntityNameFromWorld(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid())
        return {};

    auto* name = world->GetComponent<Components::Name>(entity);
    if (name)
        return std::string(name->View());
    return {};
}

// In-place gamma encode (linear → sRGB) for pixels already quantized to RGBA8.
// Only for sources that are 8-bit to begin with (no precision left to recover).
// Float sources must encode in float inside ConvertToRGBA8 BEFORE the quantize:
// encoding after quantizing spreads one 1/255 linear quantum across ~4 output
// LSB near black, manufacturing banding the screen never shows.
static void ApplySRGBEncodeRGBA8(std::vector<uint8_t>& rgba)
{
    auto encode = [](uint8_t v) -> uint8_t
    {
        float linear = static_cast<float>(v) / 255.0f;
        float srgb = (linear <= 0.0031308f)
                         ? 12.92f * linear
                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        srgb = std::max(0.0f, std::min(1.0f, srgb));
        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    };
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = encode(rgba[i + 0]);
        rgba[i + 1] = encode(rgba[i + 1]);
        rgba[i + 2] = encode(rgba[i + 2]);
        // alpha is left untouched
    }
}

// Interleaved gradient noise, ported verbatim from encode_srgb.frag.
// Coordinates are pixel centers (x+0.5, y+0.5) to mirror gl_FragCoord.
static float InterleavedGradientNoiseAt(uint32_t px, uint32_t py, float offset)
{
    auto fract = [](float v) -> float { return v - std::floor(v); };
    const float x = static_cast<float>(px) + 0.5f + offset;
    const float y = static_cast<float>(py) + 0.5f + offset * 0.7f;
    return fract(52.9829189f * fract(0.06711056f * x + 0.00583715f * y));
}

// Per-pixel triangular-PDF dither, ported verbatim from encode_srgb.frag
// (InterleavedGradientNoise + inverse-CDF remap). The screen's sRGB encode pass
// dithers its 8-bit quantize with exactly this pattern; captures quantizing the
// same linear source must match or they show staircase banding the display
// never had. Returns a value in (-1, 1).
static float TriangularDitherAt(uint32_t px, uint32_t py)
{
    const float u = InterleavedGradientNoiseAt(px, py, 0.0f);
    const float o = 2.0f * u - 1.0f;
    const float s = (o > 0.0f) ? 1.0f : ((o < 0.0f) ? -1.0f : 0.0f);
    return s * (1.0f - std::sqrt(std::max(0.0f, 1.0f - std::abs(o))));
}

// Gradient-aware deband on the encoded float plane — the CPU mirror of
// encode_srgb.frag's pre-dither DebandEncoded, kept so captures continue to
// match the screen (the same parity contract as TriangularDitherAt above):
// identical noise fields, tap geometry, iteration schedule, and all-channel
// threshold gate, with clamp-to-edge bilinear taps mirroring the pass's
// sampler. One deliberate divergence: the shader encodes each bilinear tap of
// the LINEAR source, this taps the pre-ENCODED plane — the two differ only by
// transfer-curve curvature across a tap's bilinear footprint, sub-LSB on the
// shallow gradients the gate opens for, and the gate is shut at real edges
// either way. `thresholdLsb` is the pass-published effective gate — the SAME
// volume/env resolution the screen's backbuffer encode used (see
// GetBackbufferOutputDebandThresholdLsb), so a volume toggle or a GE_DEBAND
// relaunch changes screen and captures together. Radius stays process-wide.
//
// Reached only on frames whose composite is still LINEAR — an HDR-display
// capture, where the terminal pass remains the single encode and debands
// full-frame. On SDR frames each world view debanded inside its own image, so the
// composite arrives finalized and the conversion below requantizes it without
// running either filter.
static std::vector<float> DebandEncodedPlane(
    const std::vector<float>& enc, uint32_t width, uint32_t height, float thresholdLsb)
{
    const float threshold = thresholdLsb / 255.0f;
    const float baseRadius = Rendering::Passes::GetOutputDebandSettings().RadiusPx;
    constexpr int kIterations = 3; // encode_srgb.frag kDebandIterations
    constexpr float kGolden = 0.61803399f;
    constexpr float kTwoPi = 6.2831853f;

    auto fract = [](float v) -> float { return v - std::floor(v); };
    auto sampleBilinear = [&](float sx, float sy, float rgb[3])
    {
        // Pixel-center coordinates in, clamp-to-edge bilinear out.
        const float fx = std::clamp(sx - 0.5f, 0.0f, static_cast<float>(width - 1));
        const float fy = std::clamp(sy - 0.5f, 0.0f, static_cast<float>(height - 1));
        const uint32_t x0 = static_cast<uint32_t>(fx);
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t x1 = std::min(x0 + 1, width - 1);
        const uint32_t y1 = std::min(y0 + 1, height - 1);
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const float* p00 = &enc[(static_cast<size_t>(y0) * width + x0) * 3];
        const float* p10 = &enc[(static_cast<size_t>(y0) * width + x1) * 3];
        const float* p01 = &enc[(static_cast<size_t>(y1) * width + x0) * 3];
        const float* p11 = &enc[(static_cast<size_t>(y1) * width + x1) * 3];
        for (int c = 0; c < 3; ++c)
        {
            const float top = p00[c] + (p10[c] - p00[c]) * tx;
            const float bot = p01[c] + (p11[c] - p01[c]) * tx;
            rgb[c] = top + (bot - top) * ty;
        }
    };

    std::vector<float> out(enc.size());
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const float u1 = InterleavedGradientNoiseAt(px, py, 5.0f);
            const float u2 = InterleavedGradientNoiseAt(px, py, 11.0f);
            const float cx = static_cast<float>(px) + 0.5f;
            const float cy = static_cast<float>(py) + 0.5f;
            float res[3];
            const float* center = &enc[(static_cast<size_t>(py) * width + px) * 3];
            res[0] = center[0];
            res[1] = center[1];
            res[2] = center[2];
            for (int i = 1; i <= kIterations; ++i)
            {
                const float r1 = fract(u1 + static_cast<float>(i) * kGolden);
                const float r2 = fract(u2 + static_cast<float>(i) * kGolden);
                const float dist = baseRadius * static_cast<float>(i) * (0.25f + 0.75f * r1);
                const float angle = kTwoPi * r2;
                const float ox = dist * std::cos(angle);
                const float oy = dist * std::sin(angle);
                float t0[3], t1[3], t2[3], t3[3];
                sampleBilinear(cx + ox, cy + oy, t0);
                sampleBilinear(cx - oy, cy + ox, t1);
                sampleBilinear(cx - ox, cy - oy, t2);
                sampleBilinear(cx + oy, cy - ox, t3);
                // Gate against the ORIGINAL center (mirrors DebandEncoded):
                // bounds total displacement to one threshold by construction.
                float avg[3];
                bool pass = true;
                for (int c = 0; c < 3; ++c)
                {
                    avg[c] = 0.25f * (t0[c] + t1[c] + t2[c] + t3[c]);
                    pass = pass && std::abs(avg[c] - center[c]) < threshold;
                }
                if (pass)
                {
                    res[0] = avg[0];
                    res[1] = avg[1];
                    res[2] = avg[2];
                }
            }
            float* dst = &out[(static_cast<size_t>(py) * width + px) * 3];
            dst[0] = res[0];
            dst[1] = res[1];
            dst[2] = res[2];
        }
    }
    return out;
}

// Encode-filter state a capture should mirror: the effective deband gate the
// screen's backbuffer encode declared. Snapshotted on the render thread right
// after the target window's encode declaration (the render callback) so a sibling
// window's later declaration can't clobber the value before the deferred
// conversion runs; null falls back to the process-wide publish.
struct CaptureEncodeFilterState
{
    float DebandThresholdLsb = 0.0f;
};

// Encoded-domain finish for float color planes (3 floats per pixel, linear,
// already range-scaled): transfer curve, optional deband, TPDF dither,
// quantize to RGBA8. Splitting the encode from the quantize (vs the inline
// toU8 in ConvertToRGBA8) lets the deband tap a fully-encoded plane the way
// the screen's shader taps its source; with the deband inactive the op order
// (curve, +dither, clamp-quantize) is identical to the inline path.
static void EncodeDitherQuantizePlane(
    std::vector<float>& plane, uint32_t width, uint32_t height,
    bool srgbEncode, bool dither, bool deband, const CaptureEncodeFilterState* filterState,
    std::vector<uint8_t>& rgba)
{
    // Captures always quantize to 8 bits whatever depth the swapchain runs at,
    // so the mirror's dither is sized to ITS OWN quantizer — not to the screen's.
    constexpr float kCaptureDitherLsb = 1.0f / 255.0f;
    // BOTH filters are conditioned on `srgbEncode`, which asks "does this
    // conversion own the transfer curve?" — and therefore also "has anything
    // quantized these values for a destination yet?". A linear source (an
    // HDR-display frame's composite, a float data plane) has not been, so the
    // mirror encodes it and runs the screen's filters. An already-encoded source
    // arrives FINALIZED: every world view applied the OETF, debanded and dithered
    // at the presented step inside its own image, and the chrome around it is CSS
    // bytes that quantize exactly. Filtering that a second time would displace
    // world pixels the screen kept and grain chrome the screen left byte-stable,
    // so the conversion is a pure requantize — which makes screen-vs-capture
    // parity exact on chrome by construction. The residual it accepts: an 8-bit
    // capture of a world dithered for a 10-bit screen keeps only a quarter of
    // that noise, so a shallow gradient can contour in the PNG where the screen
    // shows none. Fixing that belongs upstream (finalize at the capture's step),
    // not in a filter here that cannot tell world from chrome.
    if (srgbEncode)
    {
        for (float& v : plane)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    // The gate is the pass-published effective threshold (volume AND env
    // resolved), 0 whenever the screen's filter is off — captures and screen
    // stay in lockstep through every combination of lever and override.
    const float effectiveDebandLsb =
        filterState ? filterState->DebandThresholdLsb
                    : Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();
    if (deband && srgbEncode && effectiveDebandLsb > 0.0f)
        plane = DebandEncodedPlane(plane, width, height, effectiveDebandLsb);

    const bool ditherThisPlane = dither && srgbEncode;
    for (uint32_t py = 0; py < height; ++py)
    {
        for (uint32_t px = 0; px < width; ++px)
        {
            const size_t i = static_cast<size_t>(py) * width + px;
            const float d =
                ditherThisPlane ? TriangularDitherAt(px, py) * kCaptureDitherLsb : 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                const float v = plane[i * 3 + c] + d;
                rgba[i * 4 + c] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
            }
        }
    }
}

// Convert raw readback bytes into RGBA8 for PNG encoding.
// Handles depth (linearize 0-1 → grayscale), float (tonemap), and integer formats.
// srgbEncode applies the linear→sRGB transfer curve (same constants as
// ApplySRGBEncodeRGBA8) — in float before the quantize for float/packed sources,
// in 8-bit for sources that are already quantized.
// dither adds a 1-LSB (1/255) TPDF offset in output space right before any
// float→8-bit quantize, matching the screen's encode pass. It never applies to
// sources that are already 8-bit — there is no sub-LSB information to recover,
// noise would only corrupt them.
// deband mirrors the screen's pre-dither deband on sRGB-encoded float sources
// (see DebandEncodedPlane); like dither it recovers/reshapes sub-LSB structure
// and never applies to already-quantized sources.
static std::vector<uint8_t> ConvertToRGBA8(
    const void* raw, uint32_t width, uint32_t height,
    Rendering::TextureFormat fmt, float rangeMin, float rangeMax, bool srgbEncode,
    bool dither, bool deband, const CaptureEncodeFilterState* filterState = nullptr)
{
    using TF = Rendering::TextureFormat;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(pixelCount * 4, 255);

    constexpr float kDitherLsb = 1.0f / 255.0f;

    // d is the per-pixel TPDF offset (already scaled to the 1/255 output LSB;
    // 0 when dithering is off), applied after the transfer curve — the same
    // order as encode_srgb.frag (encode, dither in sRGB space, quantize).
    auto toU8 = [srgbEncode](float v, float d) -> uint8_t
    {
        if (srgbEncode)
            v = (v <= 0.0031308f) ? 12.92f * v
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        v += d;
        return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)));
    };

    auto ditherAt = [dither, width](size_t i) -> float
    {
        if (!dither)
            return 0.0f;
        const uint32_t px = static_cast<uint32_t>(i % width);
        const uint32_t py = static_cast<uint32_t>(i / width);
        return TriangularDitherAt(px, py) * kDitherLsb;
    };

    float scale = (rangeMax > rangeMin) ? 1.0f / (rangeMax - rangeMin) : 1.0f;

    if (fmt == TF::RGBA8_UNORM || fmt == TF::RGBA8_SRGB)
    {
        std::memcpy(rgba.data(), raw, pixelCount * 4);
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::BGRA8_UNORM || fmt == TF::BGRA8_SRGB)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2]; // B→R
            rgba[i * 4 + 1] = src[i * 4 + 1]; // G→G
            rgba[i * 4 + 2] = src[i * 4 + 0]; // R→B
            rgba[i * 4 + 3] = src[i * 4 + 3]; // A→A
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::D32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (src[i] - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D24_UNORM_S8_UINT || fmt == TF::X8_D24_UNORM_PACK32)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i] & 0x00FFFFFFu) / 16777215.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::D16_UNORM)
    {
        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float depth = static_cast<float>(src[i]) / 65535.0f;
            float v = (depth - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R16G16B16A16_FLOAT)
    {
        // half-float: use bit manipulation to convert to float
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (halfToFloat(src[i * 4 + 0]) - rangeMin) * scale;
            plane[i * 3 + 1] = (halfToFloat(src[i * 4 + 1]) - rangeMin) * scale;
            plane[i * 3 + 2] = (halfToFloat(src[i * 4 + 2]) - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R16_FLOAT)
    {
        auto halfToFloat = [](uint16_t h) -> float
        {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0)
                return sign ? -0.0f : 0.0f;
            if (exp == 31)
                return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        auto* src = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float v = (halfToFloat(src[i]) - rangeMin) * scale;
            uint8_t g = toU8(v, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t g = toU8((src[i] - rangeMin) * scale, ditherAt(i));
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
        }
        return rgba;
    }

    if (fmt == TF::R32G32B32A32_FLOAT)
    {
        auto* src = static_cast<const float*>(raw);
        std::vector<float> plane(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            plane[i * 3 + 0] = (src[i * 4 + 0] - rangeMin) * scale;
            plane[i * 3 + 1] = (src[i * 4 + 1] - rangeMin) * scale;
            plane[i * 3 + 2] = (src[i * 4 + 2] - rangeMin) * scale;
        }
        EncodeDitherQuantizePlane(plane, width, height, srgbEncode, dither, deband, filterState, rgba);
        return rgba;
    }

    if (fmt == TF::R11G11B10_FLOAT)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            // Unpack R11G11B10 — 11-bit R, 11-bit G, 10-bit B (all unsigned float)
            uint32_t bits = src[i];
            auto unpack11 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 6) & 0x1F;
                uint32_t mantissa = v & 0x3F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 64.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            auto unpack10 = [](uint32_t v) -> float
            {
                uint32_t exp = (v >> 5) & 0x1F;
                uint32_t mantissa = v & 0x1F;
                if (exp == 0)
                    return 0.0f;
                if (exp == 31)
                    return 65536.0f;
                return std::ldexp(static_cast<float>(mantissa) / 32.0f + 1.0f, static_cast<int>(exp) - 15);
            };
            float r = (unpack11(bits & 0x7FF) - rangeMin) * scale;
            float g = (unpack11((bits >> 11) & 0x7FF) - rangeMin) * scale;
            float b = (unpack10((bits >> 22) & 0x3FF) - rangeMin) * scale;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    if (fmt == TF::R8_UNORM)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = src[i];
            rgba[i * 4 + 1] = src[i];
            rgba[i * 4 + 2] = src[i];
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
        return rgba;
    }

    if (fmt == TF::RGB10A2_UNORM)
    {
        auto* src = static_cast<const uint32_t*>(raw);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            uint32_t bits = src[i];
            float r = static_cast<float>(bits & 0x3FF) / 1023.0f;
            float g = static_cast<float>((bits >> 10) & 0x3FF) / 1023.0f;
            float b = static_cast<float>((bits >> 20) & 0x3FF) / 1023.0f;
            const float d = ditherAt(i);
            rgba[i * 4 + 0] = toU8(r, d);
            rgba[i * 4 + 1] = toU8(g, d);
            rgba[i * 4 + 2] = toU8(b, d);
        }
        return rgba;
    }

    // Fallback: copy raw bytes as RGBA8 (best effort).
    size_t bpp = Rendering::BytesPerPixel(fmt);
    if (bpp > 0)
    {
        auto* src = static_cast<const uint8_t*>(raw);
        size_t copyBytes = std::min(bpp, size_t(4));
        for (size_t i = 0; i < pixelCount; ++i)
        {
            std::memcpy(&rgba[i * 4], &src[i * bpp], copyBytes);
        }
        if (srgbEncode)
            ApplySRGBEncodeRGBA8(rgba);
    }
    return rgba;
}

// Crop an RGBA8 buffer to a sub-rectangle. Clamps the rect to the source extents.
// Returns an empty buffer if the rect is fully out of bounds.
static std::vector<uint8_t> CropRGBA8(
    const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
    int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t& outW, uint32_t& outH)
{
    outW = 0;
    outH = 0;
    if (srcW == 0 || srcH == 0 || w <= 0 || h <= 0)
        return {};

    // Clamp against the source bounds.
    int32_t x0 = std::max(0, x);
    int32_t y0 = std::max(0, y);
    int32_t x1 = std::min<int32_t>(static_cast<int32_t>(srcW), x + w);
    int32_t y1 = std::min<int32_t>(static_cast<int32_t>(srcH), y + h);
    if (x1 <= x0 || y1 <= y0)
        return {};

    const uint32_t cw = static_cast<uint32_t>(x1 - x0);
    const uint32_t ch = static_cast<uint32_t>(y1 - y0);
    std::vector<uint8_t> out(static_cast<size_t>(cw) * ch * 4);
    for (uint32_t row = 0; row < ch; ++row)
    {
        const uint8_t* srcRow = src.data() + (static_cast<size_t>(y0 + row) * srcW + x0) * 4;
        uint8_t* dstRow = out.data() + static_cast<size_t>(row) * cw * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
    }
    outW = cw;
    outH = ch;
    return out;
}

// Encode RGBA8 pixels as PNG in memory using stb_image_write.
static std::vector<uint8_t> EncodePNG(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pngData;
    auto writeFunc = [](void* context, void* data, int size)
    {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        auto* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    stbi_write_png_to_func(writeFunc, &pngData,
                           static_cast<int>(width), static_cast<int>(height),
                           4 /*RGBA*/, rgba, static_cast<int>(width * 4));
    return pngData;
}

// Base64 encoder for binary data (used by take_screenshot to return pixel data).
static std::string Base64Encode(const uint8_t* data, size_t length)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length)
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length)
            n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(kTable[(n >> 18) & 0x3F]);
        result.push_back(kTable[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < length) ? kTable[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < length) ? kTable[n & 0x3F] : '=');
    }
    return result;
}

// ---------------------------------------------------------------------------
// capture_resource support
// ---------------------------------------------------------------------------

// Base64 of the whole buffer travels over the debug-server socket; a multi-MB
// GPUScene buffer would stall the channel for a diagnostic.
static constexpr uint64_t kMaxBufferCaptureBytes = 1ull << 20;
// Decoded arrays are for eyeballing values, not bulk transfer.
static constexpr size_t kMaxDecodedElements = 4096;
// Raw texel captures ship bytes verbatim over the same socket.
static constexpr uint64_t kMaxRawCaptureBytes = 4ull << 20;
// A frame declares hundreds of resources; a miss reports a bounded sample.
static constexpr size_t kMaxReportedCandidates = 64;

// Reported in the capture response so a caller can tell a 1-channel depth map
// from a 4-channel colour target without parsing the format name.
static uint32_t FormatChannelCount(Rendering::TextureFormat fmt)
{
    using TF = Rendering::TextureFormat;
    switch (fmt)
    {
    case TF::R8_UNORM:
    case TF::R8_UINT:
    case TF::R8_SINT:
    case TF::R16_FLOAT:
    case TF::R16_UINT:
    case TF::R16_SINT:
    case TF::R32_FLOAT:
    case TF::R32_UINT:
    case TF::R32_SINT:
    case TF::D32_FLOAT:
    case TF::D16_UNORM:
    case TF::S8_UINT:
    case TF::X8_D24_UNORM_PACK32:
        return 1;
    case TF::R8G8_UNORM:
    case TF::R8G8_UINT:
    case TF::R8G8_SINT:
    case TF::R16G16_FLOAT:
    case TF::R16G16_UINT:
    case TF::R16G16_SINT:
    case TF::R32G32_FLOAT:
    case TF::R32G32_UINT:
    case TF::R32G32_SINT:
    case TF::D24_UNORM_S8_UINT:
    case TF::D32_SFLOAT_S8_UINT:
        return 2;
    case TF::R32G32B32_UINT:
    case TF::R32G32B32_SINT:
    case TF::R11G11B10_FLOAT:
        return 3;
    default:
        return 4;
    }
}

// A resource declared in the frame under inspection.
struct CaptureResourceMatch
{
    uint32_t Id = 0xFFFFFFFFu;
    bool IsTexture = true;
    std::string Name;
};

// Resolve a capture_resource `name` against the resources declared in `frame`.
//
// An exact name wins outright. Otherwise a case-insensitive substring match is
// accepted only when it is UNIQUE — a debug tool that silently picks one of
// several "SceneView.Color"-ish targets would attribute the wrong pixels, which
// is worse than failing. Both failure modes fill `outCandidates` so the caller
// can report what the frame actually declared instead of a bare "not found".
static bool ResolveCaptureResource(const Rendering::RenderGraph::RGFrame& frame,
                                   const std::string& requested,
                                   CaptureResourceMatch& outMatch,
                                   std::vector<std::string>& outCandidates)
{
    namespace RG = Rendering::RenderGraph;
    const auto& graph = frame.Graph();
    const size_t count = graph.ResourceCount();

    auto lower = [](std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(requested);

    std::vector<CaptureResourceMatch> partial;
    for (uint32_t id = 0; id < count; ++id)
    {
        const auto& desc = graph.ResourceDesc(id);
        if (!desc.Name)
            continue;
        if (desc.Kind == RG::RGResourceKind::AccelerationStructure)
            continue; // no bytes the graph owns to capture
        const bool isTexture = desc.Kind == RG::RGResourceKind::Texture;
        const std::string name = desc.Name;
        if (name == requested)
        {
            outMatch = CaptureResourceMatch{id, isTexture, name};
            return true;
        }
        if (lower(name).find(needle) != std::string::npos)
            partial.push_back(CaptureResourceMatch{id, isTexture, name});
    }

    if (partial.size() == 1)
    {
        outMatch = partial.front();
        return true;
    }

    // Ambiguous → the near misses are the useful report; empty → everything is.
    const auto& report = partial;
    if (!report.empty())
    {
        for (const auto& m : report)
            outCandidates.push_back(m.Name);
    }
    else
    {
        for (uint32_t id = 0; id < count; ++id)
            if (graph.ResourceDesc(id).Name)
                outCandidates.push_back(graph.ResourceDesc(id).Name);
    }
    return false;
}

// ---------------------------------------------------------------------------
// take_screenshot support
// ---------------------------------------------------------------------------

struct ScreenshotCropDesc
{
    bool valid = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    std::string label;
};

// Monotonic per-session sequence: every capture writes a fresh file so a stale
// PNG from an earlier (possibly failed) capture can never masquerade as the
// new one when a caller re-reads a remembered filePath. It restarts at 1 in
// every process, which is why the names live under a process-scoped directory
// (CaptureOutputDirectory) rather than in one shared per-machine folder.
static std::atomic<uint32_t> s_CaptureSequence{0};

// Material texture bindings still waiting on a decode, world-wide. Reported on
// every capture and by get_editor_state: a frame taken while this is non-zero
// shows bindless defaults (white albedo) for those materials, which on screen is
// indistinguishable from a material that never binds. A first material bind tags
// the texture's cook usage, so on a cold derived cache the window is as wide as
// the block-compression encode — seconds to minutes. Zero when the renderer is
// not up, which is the honest answer for a frame with no material binding at all.
static size_t PendingMaterialTextureBinds()
{
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        return rs->Textures().PendingMaterialTextureBindCount();
    return 0u;
}

// Encode and publish one owned capture. Each request gets its own file because
// completed readbacks can encode concurrently on different workers.
static json EncodeCapturePng(const std::vector<uint8_t>& rgba8, uint32_t width, uint32_t height,
                             const std::string& fileStem)
{
    auto png = EncodePNG(rgba8.data(), width, height);
    if (png.empty())
        return Editor::RefuseRequest("PNG encoding failed");

    const std::filesystem::path filePath = Editor::CaptureOutputDirectory() /
        (fileStem + "_" + std::to_string(++s_CaptureSequence) + ".png");
    std::ofstream output(filePath, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    output.close();
    if (!output)
        return Editor::RefuseRequest("Capture write failed: " + filePath.string());
    return json{{"filePath", filePath.string()}, {"pngBase64", Base64Encode(png.data(), png.size())}};
}

static json FinishResourceCaptureResult(Rendering::ViewReadbackResult readback,
                                        float rangeMin, float rangeMax, json metadata)
{
    // Named resources have no color-space stamp. Visualize their declared range
    // without a transfer curve; raw captures retain values outside that range.
    auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                readback.format, rangeMin, rangeMax,
                                /*srgbEncode*/ false, /*dither*/ false, /*deband*/ false);
    std::string safeName = metadata.at("resourceName").get<std::string>();
    for (char& character : safeName)
        if (character == '/' || character == '\\' || character == ':' || character == '?' ||
            character == '*' || character == ' ')
            character = '_';
    json result = EncodeCapturePng(rgba8, readback.width, readback.height, "resource_" + safeName);
    if (!Editor::IsRefusal(result))
        result.update(metadata);
    return result;
}

// Crop, encode, persist, and fill the take_screenshot response (either the
// success payload or a refusal). Touches no editor state, so the rendergraph
// path runs it on a job: the encode costs seconds for a full window in DebugFast.
// `method` records which path produced the pixels: "rendergraph" (GPU readback
// of the frame composite) or "printwindow" (OS-compositor picture of the window,
// served only to a request that passed allowWindowCapture).
// `pendingMaterialTextureBinds` records whether the frame's materials had
// finished binding — the same class of self-description as `method`, and the
// one that tells a transient frame from a broken one.
static void FinishScreenshotResult(json& outResult,
                                   std::vector<uint8_t>&& rgba8,
                                   uint32_t srcW, uint32_t srcH,
                                   const ScreenshotCropDesc& crop,
                                   const std::string& target,
                                   const char* method,
                                   size_t pendingMaterialTextureBinds)
{
    uint32_t outW = srcW;
    uint32_t outH = srcH;
    std::vector<uint8_t> finalPixels;
    if (crop.valid)
    {
        finalPixels = CropRGBA8(rgba8, srcW, srcH, crop.x, crop.y, crop.w, crop.h, outW, outH);
        if (finalPixels.empty())
        {
            outResult = Editor::RefuseRequest("Crop produced empty image (rect outside window for target='" + target + "')");
            return;
        }
    }
    else
    {
        finalPixels = std::move(rgba8);
    }

    outResult = EncodeCapturePng(finalPixels, outW, outH, "screenshot_" + target);
    if (Editor::IsRefusal(outResult))
        return;
    outResult.update(json{
        {"width", outW},
        {"height", outH},
        {"sourceWidth", srcW},
        {"sourceHeight", srcH},
        {"target", target},
        {"format", "png"},
        {"method", method},
        {"pendingMaterialTextureBinds", pendingMaterialTextureBinds}
    });
}

// Find a panel of type T in the editor's panel storage.
template <typename T>
static T* FindPanel(const std::vector<std::unique_ptr<UIElement>>& panels)
{
    for (auto& el : panels)
    {
        if (auto* p = dynamic_cast<T*>(el.get()))
            return p;
    }
    return nullptr;
}

static GraphPanel* ResolveDebugGraphPanel(std::string_view kindId = {})
{
    if (!kindId.empty())
    {
        if (GraphPanel* live = GraphPanel::FindLiveByKind(kindId))
            return live;
    }
    if (GraphPanel* last = GraphPanel::FindLiveLastOpened())
        return last;
    GraphPanel* first = nullptr;
    GraphPanel::ForEachLive([&first](GraphPanel& panel) {
        if (!first)
            first = &panel;
    });
    return first;
}

static AnimationWindowPanel* FindAnimationPanelOfKind(const std::vector<std::unique_ptr<UIElement>>& panels,
                                                      AnimationWindowPanel::PanelKind kind)
{
    for (auto& el : panels)
    {
        if (auto* p = dynamic_cast<AnimationWindowPanel*>(el.get()))
        {
            if (p->GetPanelKind() == kind)
                return p;
        }
    }
    return nullptr;
}

// Component checker table: maps component name to a predicate.
using ComponentChecker = std::function<bool(ECS::World*, ECS::EntityHandle)>;

static const std::unordered_map<std::string, ComponentChecker>& GetComponentCheckers()
{
    static const std::unordered_map<std::string, ComponentChecker> kCheckers = {
        {"Transform", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Transform>(e); }},
        {"Name", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Name>(e); }},
        {"Parent", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Parent>(e); }},
        {"MeshRenderer", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::MeshRenderer>(e); }},
        {"Camera", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Camera>(e); }},
        {"Light", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Light>(e); }},
        {"Terrain", [](ECS::World* w, ECS::EntityHandle e)
         { return w->HasComponent<Components::Terrain>(e); }},
    };
    return kCheckers;
}

// Serialize a dynamically-discovered component to JSON via its raw bytes.
//
// A component with a reflected field table reads back BY FIELD NAME, in the canonical
// PascalCase the reflected writer accepts, so get_entity_components output feeds straight back
// into set_component (terrain-authoring-design §9.1). Enum fields carry their enumerator name
// and AssetRef<> fields their GUID as compact hex — both forms set_component parses.
// Everything else (physics, any type the scanner skipped) keeps the opaque hex blob: without a
// field table there is nothing to name it with, and guessing types would be worse than saying so.
static json SerializeDynamicComponent(ECS::World* world, ECS::EntityHandle entity,
                                      ECS::ComponentTypeId typeId)
{
    std::vector<uint8_t> bytes;
    if (!world->CaptureComponentBytes(entity, typeId, bytes))
        return json::object();
    if (bytes.empty())
        return json::object();

    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    if (!fields.empty())
    {
        std::string text;
        ECS::AppendComponentJson(text, fields, reinterpret_cast<const std::byte*>(bytes.data()));
        // The dump builds JSON text; parse once per component rather than mirror its decoding
        // ladder here. A discarded parse falls through to the hex blob below, so a decode gap
        // degrades to the old opaque answer instead of dropping the component silently.
        if (json parsed = json::parse(text, nullptr, false); !parsed.is_discarded())
            return parsed;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hexStr;
    hexStr.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        hexStr.push_back(kHex[b >> 4]);
        hexStr.push_back(kHex[b & 0xF]);
    }
    return json{{"size", bytes.size()}, {"data", hexStr}};
}

// A component's on/off state is its ECS::ComponentDisabled tag, reported as "enabled" in its
// entry, for a type that switches through the tag (ComponentRegistry::SwitchesThroughDisabledTag).
// A type that keeps its own Enabled field carries it in its own dump; a NotToggleable type has none.
static void AddComponentEnabledState(json& entry, ECS::World* world, ECS::EntityHandle entity,
                                     ECS::ComponentTypeId typeId)
{
    if (!entry.is_object() || !ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
        return;
    entry["enabled"] = world->IsComponentEnabled(entity, typeId);
}

// Serialize all components of an entity to JSON.
// Hand-written serializers cover the components whose IPC shape is not their field layout
// (Transform's TRS, physics runtime handles); everything else in the entity's archetype goes
// through SerializeDynamicComponent, which names reflected fields and falls back to a hex blob
// only for types with no field table.
//
// Every hand-written block below SHADOWS the reflected shape, and each is a fixed list someone
// has to remember to extend — the same class that left TerrainGrass covering 41 of its 54 fields
// on both sides. They are not all justified by the Transform/physics reason; several are just
// older than the reflected path. Prefer deleting one on contact to adding another, and add one
// only when the component's IPC shape genuinely is not its field layout.
static json SerializeEntityComponents(ECS::World* world, ECS::EntityHandle entity)
{
    json components = json::object();

    // Track which type IDs have been serialized by the hand-written path.
    std::unordered_set<ECS::ComponentTypeId> serialized;

    if (world->HasComponent<Components::Transform>(entity))
    {
        auto* c = world->GetComponent<Components::Transform>(entity);
        auto pos = c->GetPosition();
        auto rot = c->GetRotation();
        auto scl = c->GetScale();
        const auto& rq = rot.GetGLM();
        components["Transform"] = {
            {"position", {{"x", pos.x}, {"y", pos.y}, {"z", pos.z}}},
            {"rotation", {{"x", rq.x}, {"y", rq.y}, {"z", rq.z}, {"w", rq.w}}},
            {"scale", {{"x", scl.x}, {"y", scl.y}, {"z", scl.z}}}};
        serialized.insert(ECS::GetComponentTypeId<Components::Transform>());
    }

    if (world->HasComponent<Components::Name>(entity))
    {
        auto* c = world->GetComponent<Components::Name>(entity);
        components["Name"] = {{"value", std::string(c->View())}};
        serialized.insert(ECS::GetComponentTypeId<Components::Name>());
    }

    if (world->HasComponent<Components::Parent>(entity))
    {
        auto* c = world->GetComponent<Components::Parent>(entity);
        components["Parent"] = {{"parentId", c->parent.id}};
        serialized.insert(ECS::GetComponentTypeId<Components::Parent>());
    }

    if (world->HasComponent<Components::MeshRenderer>(entity))
    {
        auto* c = world->GetComponent<Components::MeshRenderer>(entity);
        components["MeshRenderer"] = {
            {"meshId", c->meshId},
            {"renderLayerMask", c->renderLayerMask},
            {"castShadows", c->castShadows},
            {"receiveShadows", c->receiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::MeshRenderer>());
    }

    if (world->HasComponent<Components::Camera>(entity))
    {
        auto* c = world->GetComponent<Components::Camera>(entity);
        components["Camera"] = {
            {"perspective", c->Perspective},
            {"fovY", c->FovY},
            {"orthographicSize", c->OrthographicSize},
            {"nearZ", c->NearZ},
            {"farZ", c->FarZ},
            {"cullingMask", c->CullingMask},
            {"postProcessProfileId", c->PostProcessProfileId},
            {"postProcessMask", c->PostProcessMask}};
        serialized.insert(ECS::GetComponentTypeId<Components::Camera>());
    }

    if (world->HasComponent<Components::Light>(entity))
    {
        auto* c = world->GetComponent<Components::Light>(entity);
        components["Light"] = {
            {"type", static_cast<uint32_t>(c->Type)},
            {"color", {c->Color[0], c->Color[1], c->Color[2]}},
            {"intensity", c->Intensity},
            {"range", c->Range},
            {"innerAngle", c->InnerAngle},
            {"outerAngle", c->OuterAngle},
            {"areaShape", static_cast<uint32_t>(c->AreaShape)},
            {"areaWidth", c->AreaWidth},
            {"areaHeight", c->AreaHeight},
            {"areaRadius", c->AreaRadius},
            {"falloff", static_cast<uint32_t>(c->Falloff)},
            {"decay", c->Decay},
            {"castsLight", c->CastsLight},
            {"castsShadows", c->CastsShadows},
            {"cascadeCount", c->CascadeCount},
            {"shadowAngularDiameter", c->ShadowAngularDiameter},
            {"fogContribution", c->FogContribution},
            {"fogDensityBoost", c->FogDensityBoost},
            {"fogAnisotropy", c->FogAnisotropy},
            {"fogOriginFade", c->FogOriginFade}};
        serialized.insert(ECS::GetComponentTypeId<Components::Light>());
    }

    if (world->HasComponent<Components::ParticleCollisionEventsBuffer>(entity))
    {
        auto* c = world->GetComponent<Components::ParticleCollisionEventsBuffer>(entity);
        json events = json::array();
        for (uint32_t i = 0; i < c->Count && i < Components::ParticleCollisionEventsBuffer::kMaxEvents; ++i)
        {
            const auto& e = c->Events[i];
            events.push_back({{"Emitter", e.Emitter.id},
                              {"ParticleId", e.ParticleId},
                              {"KilledParticle", e.KilledParticle},
                              {"Position", {e.Position[0], e.Position[1], e.Position[2]}},
                              {"Normal", {e.Normal[0], e.Normal[1], e.Normal[2]}},
                              {"Velocity", {e.Velocity[0], e.Velocity[1], e.Velocity[2]}},
                              {"Speed", e.Speed}});
        }
        components["ParticleCollisionEventsBuffer"] = {
            {"Count", c->Count},
            {"Events", std::move(events)}};
        serialized.insert(ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>());
    }

    if (world->HasComponent<Components::Terrain>(entity))
    {
        auto* c = world->GetComponent<Components::Terrain>(entity);
        components["Terrain"] = {
            {"BaseSource", static_cast<uint32>(c->BaseSource)},
            {"TerrainAsset",
             c->TerrainAssetGuid.IsNull() ? std::string{} : c->TerrainAssetGuid.ToGuid().ToString()},
            {"SizeX", c->SizeX},
            {"SizeZ", c->SizeZ},
            {"HeightScale", c->HeightScale},
            {"SamplesPerMeter", c->SamplesPerMeter},
            {"StreamingRadius", c->StreamingRadius},
            {"Domain", static_cast<uint32>(c->Domain)},
            {"PlanetRadius", c->PlanetRadius},
            {"SphereSculpt",
             c->SphereSculptGuid.IsNull() ? std::string{} : c->SphereSculptGuid.ToGuid().ToString()},
            {"TargetPixelError", c->TargetPixelError},
            {"SeaLevel", c->SeaLevel},
            {"MaterialTiling", c->MaterialTiling},
            {"MaterialLibrary", c->MaterialLibraryGuid.IsNull()
                                   ? std::string{}
                                   : c->MaterialLibraryGuid.ToGuid().ToString()},
            // The library slot each channel role binds. Per-role for the same reason the
            // per-layer keys below are per-layer: the schema takes layerRole0..3, so an
            // array key would have nothing to land on when this is fed back to set_component.
            {"LayerRole0", static_cast<uint32>(c->LayerRoleSlot[0])},
            {"LayerRole1", static_cast<uint32>(c->LayerRoleSlot[1])},
            {"LayerRole2", static_cast<uint32>(c->LayerRoleSlot[2])},
            {"LayerRole3", static_cast<uint32>(c->LayerRoleSlot[3])},
            {"LayerAlbedo0", c->LayerAlbedoTexture[0].IsNull() ? std::string{} : c->LayerAlbedoTexture[0].ToGuid().ToString()},
            {"LayerAlbedo1", c->LayerAlbedoTexture[1].IsNull() ? std::string{} : c->LayerAlbedoTexture[1].ToGuid().ToString()},
            {"LayerAlbedo2", c->LayerAlbedoTexture[2].IsNull() ? std::string{} : c->LayerAlbedoTexture[2].ToGuid().ToString()},
            {"LayerAlbedo3", c->LayerAlbedoTexture[3].IsNull() ? std::string{} : c->LayerAlbedoTexture[3].ToGuid().ToString()},
            // Per-layer, not packed: every other key here lowercases to the Terrain scene
            // schema's own property name, so get_entity_components output can be fed
            // straight back to set_component. A `LayerTiling` array and a `LayerHexTiling`
            // bitmask have no schema property to land on (it takes layerTiling0..3 and
            // layerHex0..3), which broke that round-trip for exactly these two fields.
            {"LayerTiling0", c->LayerTiling[0]},
            {"LayerTiling1", c->LayerTiling[1]},
            {"LayerTiling2", c->LayerTiling[2]},
            {"LayerTiling3", c->LayerTiling[3]},
            {"LayerHex0", (c->LayerHexTiling & 1u) != 0u},
            {"LayerHex1", (c->LayerHexTiling & 2u) != 0u},
            {"LayerHex2", (c->LayerHexTiling & 4u) != 0u},
            {"LayerHex3", (c->LayerHexTiling & 8u) != 0u},
            {"RenderLayerMask", c->RenderLayerMask},
            {"CastShadows", c->CastShadows},
            {"ReceiveShadows", c->ReceiveShadows}};
        serialized.insert(ECS::GetComponentTypeId<Components::Terrain>());
    }

    if (world->HasComponent<Components::TerrainPlanetRelief>(entity))
    {
        auto* c = world->GetComponent<Components::TerrainPlanetRelief>(entity);
        components["TerrainPlanetRelief"] = {
            {"Amplitude", c->Amplitude},
            {"Frequency", c->Frequency},
            {"Octaves", c->Octaves}};
        serialized.insert(ECS::GetComponentTypeId<Components::TerrainPlanetRelief>());
    }

    if (world->HasComponent<Components::SplineComponent>(entity))
    {
        auto* c = world->GetComponent<Components::SplineComponent>(entity);
        components["SplineComponent"] = {
            {"SplineDataIndex", c->SplineDataIndex},
            {"SplineDataGeneration", c->SplineDataGeneration},
            {"DefaultRadius", c->DefaultRadius}};
        serialized.insert(ECS::GetComponentTypeId<Components::SplineComponent>());
    }

    // Physics components live outside the component scanner's reflected glob
    // (see the set_component handler), so the dynamic fallback below cannot name
    // their fields. The runtime handles are the interesting part here: they say
    // which physics body actually drives this entity, which is what tells a
    // duplicated or revived entity apart from one aliasing another's body.
    if (world->HasComponent<Components::PhysicsBody>(entity))
    {
        auto* c = world->GetComponent<Components::PhysicsBody>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["PhysicsBody"] = {
            {"motionType", static_cast<uint32_t>(c->motionType)},
            {"mass", c->mass},
            {"initialized", c->initialized},
            {"bodyHandle", c->body.Value()},
            {"shapeHandle", c->shape.Value()},
            {"childShapeCount", c->childShapeCount},
            // The physics world's own verdict on those handles: a copied handle
            // can name a live body it does not own, so "valid" is not "mine".
            {"bodyValid", pw ? pw->IsBodyValid(c->body) : false},
            {"shapeValid", pw ? pw->IsShapeValid(c->shape) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::PhysicsBody>());
    }

    if (world->HasComponent<Components::CharacterController>(entity))
    {
        auto* c = world->GetComponent<Components::CharacterController>(entity);
        auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
        components["CharacterController"] = {
            {"motor", static_cast<uint32_t>(c->motor)},
            {"radius", c->radius},
            {"height", c->height},
            {"initialized", c->initialized},
            {"grounded", c->grounded},
            {"characterHandle", c->character.Value()},
            {"characterValid", pw ? pw->IsCharacterValid(c->character) : false},
            {"physicsWorldPresent", pw != nullptr}};
        serialized.insert(ECS::GetComponentTypeId<Components::CharacterController>());
    }

    // Dynamic fallback: serialize remaining components via the ComponentRegistry,
    // then report every component's own on/off state beside its fields.
    auto* archetype = world->GetEntityArchetype(entity);
    if (archetype)
    {
        for (auto typeId : archetype->GetSignature().GetComponents())
        {
            std::string name;
            if (const std::string_view reflectedName = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
                !reflectedName.empty())
            {
                name.assign(reflectedName);
            }
            else if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            {
                name = info->Name;
            }
            else
            {
                name = "Component_" + std::to_string(typeId);
            }
            // A component's switched-off tag is reported as its "enabled" below, not as an
            // entry of its own.
            if (name.starts_with(ECS::kComponentDisabledNamePrefix))
                continue;

            // Use short name (strip namespace prefix) for JSON keys.
            auto lastColon = name.rfind("::");
            if (lastColon != std::string::npos)
                name = name.substr(lastColon + 2);
            if (!serialized.count(typeId))
                components[name] = SerializeDynamicComponent(world, entity, typeId);
            AddComponentEnabledState(components[name], world, entity, typeId);
        }
    }

    return components;
}

static std::vector<std::string> ParseDebugBuildSceneList(const std::string& raw)
{
    std::vector<std::string> out;
    for (size_t start = 0; start < raw.size();)
    {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos)
            end = raw.size();

        std::string line = raw.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        if (first < line.size())
            line = line.substr(first);
        if (!line.empty())
            out.push_back(std::move(line));

        start = end + (end < raw.size() ? 1u : 0u);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Render-graph introspection helpers (shared by the get_render_graph_* tools).
// All read the MAIN window's live RGFrame at request time, which holds the
// previous frame's compiled graph (the next BeginFrame, which clears it, runs
// later in the frame loop) — a stable snapshot.
// ---------------------------------------------------------------------------
namespace
{
namespace RG = Rendering::RenderGraph;

RG::RGFrame* MainRgFrame(EditorApplication& app)
{
    if (app.m_Windows.empty() || !app.m_Windows[0])
        return nullptr;
    return app.m_Windows[0]->RenderGraphStream.Frame.get();
}

// Lifetime classes reported by get_render_graph_resources. The backbuffer is
// an external import too, so it must be tested before the external case.
const char* RgResourceLifetimeName(const RG::RGFrame& frame, RG::RGResourceId r)
{
    if (frame.IsBackbuffer(RG::RGTexture{r}))
        return "Swapchain";
    if (frame.Graph().IsImported(r))
        return "Persistent";
    if (frame.Graph().IsExternal(r))
        return "Imported";
    return "Transient";
}

const char* RgQueueName(RG::RGQueue q)
{
    switch (q)
    {
    case RG::RGQueue::Graphics: return "Graphics";
    case RG::RGQueue::Compute:  return "Compute";
    case RG::RGQueue::Transfer: return "Copy";
    }
    return "?";
}

const char* RgLayoutName(RG::RGImageLayout l)
{
    switch (l)
    {
    case RG::RGImageLayout::Undefined:       return "Undefined";
    case RG::RGImageLayout::General:         return "General";
    case RG::RGImageLayout::ColorAttachment: return "ColorAttachment";
    case RG::RGImageLayout::DepthAttachment: return "DepthAttachment";
    case RG::RGImageLayout::DepthReadOnly:   return "DepthReadOnly";
    case RG::RGImageLayout::ShaderReadOnly:  return "ShaderReadOnly";
    case RG::RGImageLayout::TransferSrc:     return "TransferSrc";
    case RG::RGImageLayout::TransferDst:     return "TransferDst";
    case RG::RGImageLayout::Present:         return "Present";
    }
    return "?";
}

const char* RgAccessName(RG::RGAccess a)
{
    switch (a)
    {
    case RG::RGAccess::Sampled:         return "Sampled";
    case RG::RGAccess::SampledCompute:  return "SampledCompute";
    case RG::RGAccess::SampledVertex:   return "SampledVertex";
    case RG::RGAccess::UniformRead:     return "Uniform";
    case RG::RGAccess::StorageRead:     return "StorageRead";
    case RG::RGAccess::IndirectRead:    return "Indirect";
    case RG::RGAccess::IndexRead:       return "Index";
    case RG::RGAccess::VertexRead:      return "Vertex";
    case RG::RGAccess::CopySrc:         return "CopySrc";
    case RG::RGAccess::DepthRead:       return "DepthRead";
    case RG::RGAccess::ColorLoad:       return "ColorLoad";
    case RG::RGAccess::AccelerationStructureRead: return "AccelerationStructureRead";
    case RG::RGAccess::ColorAttachment: return "ColorAttachment";
    case RG::RGAccess::DepthWrite:      return "DepthWrite";
    case RG::RGAccess::StorageWrite:    return "StorageWrite";
    case RG::RGAccess::CopyDst:         return "CopyDst";
    case RG::RGAccess::AccelerationStructureBuild: return "AccelerationStructureBuild";
    }
    return "?";
}

uint32_t RgBarrierCountForPass(const RG::RGGraph& g, RG::RGPassId p)
{
    uint32_t n = 0;
    for (const auto& batch : g.BarrierBatches())
        if (batch.Pass == p)
            n += batch.Count;
    return n;
}

bool IsVec3Array(const json& v)
{
    return v.is_array() && v.size() >= 3 &&
           v[0].is_number() && v[1].is_number() && v[2].is_number();
}

// Camera IPC param validation. A wrong-shaped param used to be silently
// ignored while the handler still returned ok:true — e.g. a position sent as
// {"x": ...} instead of [x, y, z] left the camera unmoved with no error.
// Wrong shapes on recognized params are hard errors; unrecognized keys are
// collected as warnings so newer clients keep working against older editors.
// Returns a RefuseRequest refusal on shape mismatch, null json otherwise.
json ValidateCameraParams(const json& params,
                          std::initializer_list<const char*> vec3Keys,
                          std::initializer_list<const char*> numberKeys,
                          std::initializer_list<const char*> unsignedKeys,
                          json& outWarnings)
{
    outWarnings = json::array();
    if (!params.is_object())
        return Editor::RefuseRequest("params must be a JSON object");

    for (const char* key : vec3Keys)
    {
        if (params.contains(key) && !IsVec3Array(params[key]))
            return Editor::RefuseRequest(std::string("'") + key + "' must be a [x, y, z] number array");
    }
    for (const char* key : numberKeys)
    {
        if (params.contains(key) && !params[key].is_number())
            return Editor::RefuseRequest(std::string("'") + key + "' must be a number");
    }
    for (const char* key : unsignedKeys)
    {
        if (params.contains(key) && !params[key].is_number_unsigned())
            return Editor::RefuseRequest(std::string("'") + key + "' must be a non-negative integer");
    }

    auto isKnown = [&](const std::string& key)
    {
        for (const char* k : vec3Keys)
            if (key == k)
                return true;
        for (const char* k : numberKeys)
            if (key == k)
                return true;
        for (const char* k : unsignedKeys)
            if (key == k)
                return true;
        return false;
    };
    for (auto it = params.begin(); it != params.end(); ++it)
    {
        if (!isKnown(it.key()))
            outWarnings.push_back("ignored unknown param '" + it.key() + "'");
    }

    return json{};
}
} // namespace

// ---------------------------------------------------------------------------
// View activation, shared by focus_view and set_play_mode's activateGameView so
// the two can never drift into different notions of "that view is up".
// ---------------------------------------------------------------------------
namespace
{

struct ViewActivation
{
    // False only when the main window or its UI manager is unavailable; the
    // caller reports that rather than claiming a view it never raised.
    bool ok = false;
    // Read back from the dock model and the UI manager after the fact, never
    // assumed from the request.
    bool active = false;
    bool viewportFocused = false;
    std::string focusedElementId;
};

// Bring `panelId` to the front of its dock tab group and apply the UI focus that
// view wants. The two viewports want opposite things from UI focus — the Game
// View's viewport must hold it for input to reach the running game, the Scene
// View's must not or UIManager consumes its camera keys before the editor input
// actions see them — so the policy travels with the request, not with the panel.
ViewActivation ActivateDockedView(EditorApplication& app,
                                  DockingManager& docking,
                                  const std::string& panelId,
                                  bool wantsViewportUiFocus)
{
    ViewActivation result;
    if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->ui)
        return result;

    app.OpenPanel(panelId);

    auto* mainWindow = app.m_Windows[0].get();
    UIManager* ui = mainWindow->ui.get();

    // Raise the OS window: a backgrounded editor renders black or throttled, and
    // both callers exist to decide what gets rendered and captured next.
    if (mainWindow->window)
        mainWindow->window->Focus();

    if (wantsViewportUiFocus)
    {
        if (UIElement* rootEl = ui->GetRootElement())
        {
            if (rootEl->FindById(EditorPanelIds::GameViewViewport))
            {
                ui->SetFocusById(EditorPanelIds::GameViewViewport);
                result.viewportFocused = true;
            }
        }
    }
    else
    {
        ui->SetFocusById("");
    }

    result.ok = true;
    result.active = docking.IsPanelActiveTab(panelId);
    result.focusedElementId = ui->GetFocusedElementId();
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool ApplyMainSceneViewPose(EditorApplication& app, const SceneViewCameraPose& pose)
{
    if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
        return false;
    app.m_Windows[0]->scene->SetCameraPose(pose);

    // Push the new angles into the SceneViewPanel — see set_camera.
    if (auto* root = app.m_Windows[0]->ui->GetRootElement())
        if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);
    return true;
}

void RegisterDebugHandlers(EditorDebugServer& server,
                           EditorApplication& app,
                           Logger::RingBufferSink* ringBufferSink)
{
    // 1. get_editor_state
    server.RegisterHandler("get_editor_state", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        json result;

        // Play mode
        auto* playMode = app.m_PlayMode.get();
        if (playMode)
            result["playMode"] = Editor::PlayModeStateToString(playMode->GetState());
        else
            result["playMode"] = "editing";

        if (app.m_SceneEditor)
        {
            if (const auto activeScene = app.m_SceneEditor->GetActiveScenePath())
                result["scenePath"] = activeScene->string();
            else
                result["scenePath"] = "";
            result["sceneDirty"] = app.m_SceneEditor->IsSceneDirty();
            // Blocked-on-modal visibility: scene modals (save changes / restore
            // backup / save as / revert scene) silently park pending opens until
            // answered, so agents need to see them without a screenshot. Answer via
            // respond_modal or avoid them with open_scene restorePolicy. The
            // loadFailed notice parks nothing but blocks execute_command until
            // respond_modal ok dismisses it.
            const std::string modalKind = app.m_SceneEditor->GetActiveModalKind();
            result["modal"] = json{{"open", !modalKind.empty()},
                                   {"kind", modalKind.empty() ? json() : json(modalKind)}};

            // A failed open is otherwise invisible here: open_scene only reports
            // that it QUEUED the request, and an entityCount taken afterwards
            // looks perfectly plausible while the world holds a partial load. A
            // non-null sceneLoadError means the world is not the scene anyone
            // asked for; worldCleared distinguishes "your previous scene is gone"
            // from "the open document survived".
            if (const auto& failure = app.m_SceneEditor->GetLastSceneLoadFailure())
            {
                result["sceneLoadError"] = json{
                    {"path", failure->DocumentPath.string()},
                    {"message", failure->Message},
                    {"file", failure->ErrorFile.string()},
                    {"line", failure->ErrorLine},
                    {"worldCleared", failure->WorldCleared},
                    {"entitiesInWorld", failure->EntitiesInWorld}};
            }
            else
            {
                result["sceneLoadError"] = json();
            }

            // Kept SEPARATE from sceneLoadError on purpose. sceneLoadError means "the world is not
            // the scene you asked for, do not trust scenePath or entityCount". A degraded scene IS
            // the scene you asked for and both of those ARE trustworthy — it is merely missing the
            // assignments this build could not read. Folding the two together would make the
            // documented sceneLoadError contract false for every degraded open.
            //
            // Both records, because they answer different questions and reporting only one made
            // this surface contradict the window title. BuildSceneDegradedReport owns that
            // reconciliation and the shape it produces.
            result["sceneDegraded"] =
                BuildSceneDegradedReport(app.m_SceneEditor->GetLastSceneLoadDegraded(),
                                         app.m_SceneEditor->GetOutstandingSceneLoadDegraded());
        }
        else
        {
            result["scenePath"] = "";
            result["sceneLoadError"] = json();
            result["sceneDegraded"] = json();
        }

        // Content readiness the pixels cannot report. Non-zero means some
        // material is still sampling a bindless default instead of its authored
        // texture, so anything measured from the viewport right now is a
        // transient state — poll to zero before drawing a visual conclusion.
        result["pendingMaterialTextureBinds"] = PendingMaterialTextureBinds();
        // The same question for UI chrome, which the material counter cannot
        // answer: non-zero means that many background images (icons, panel art)
        // have a load or upload in flight and are drawing nothing where they
        // will draw. Zero is a settled UI frame.
        UIManager* chromeUi =
            (!app.m_Windows.empty() && app.m_Windows[0]) ? app.m_Windows[0]->ui.get() : nullptr;
        result["pendingUiBackgroundImages"] =
            chromeUi ? chromeUi->PendingBackgroundImageCount() : 0u;

        // Selected entity — read from the InspectorPanel which displays the
        // currently selected entity across all selection sources.
        auto* inspector = FindPanel<InspectorPanel>(app.m_PanelStorage);
        ECS::EntityHandle selectedEntity = inspector ? inspector->GetInspectedEntity() : ECS::EntityHandle{};
        if (selectedEntity.IsValid())
        {
            auto* world = EngineCore::GetInstance().GetPrimaryWorld();
            result["selectedEntity"] = {
                {"id", selectedEntity.id},
                {"name", EntityNameFromWorld(world, selectedEntity)}
            };
        }
        else
        {
            result["selectedEntity"] = nullptr;
        }

        // Window count
        result["windowCount"] = static_cast<int>(app.m_Windows.size());
        if (!app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->window)
        {
            int wx = 0;
            int wy = 0;
            int ww = 0;
            int wh = 0;
            app.m_Windows[0]->window->GetPosition(wx, wy);
            app.m_Windows[0]->window->GetWindowSize(ww, wh);
            result["mainWindow"] = {
                {"x", wx},
                {"y", wy},
                {"width", ww},
                {"height", wh}
            };
            if (app.m_Windows[0]->ui)
            {
                result["focusedElementId"] = app.m_Windows[0]->ui->GetFocusedElementId();
                result["hoveredElement"] = app.m_Windows[0]->ui->GetHoveredElementDebugName();
                result["mouseCaptured"] = app.m_Windows[0]->ui->IsMouseCaptured();
                result["captureElementId"] = app.m_Windows[0]->ui->GetCaptureId();
            }
        }

        // Entity count
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        result["entityCount"] = world ? static_cast<uint64_t>(world->GetEntityCount()) : 0;

        // Session provenance: who this editor is, so an agent can answer "may I touch
        // it?" from one query instead of the process table. Every field is stamped at
        // launch; an unknown field is null, never a guess. `branchAtLaunch` is the
        // branch this binary was BUILT from — the tree may have been switched since,
        // which is exactly why `worktreePath` is the identity that cannot drift.
        const auto& session = app.m_SessionDescriptor;
        auto optionalString = [](const std::optional<std::string>& value) -> json
        { return value.has_value() ? json(*value) : json(nullptr); };
        result["session"] = {
            {"label", optionalString(session.SessionLabel)},
            {"worktreePath", session.WorktreePath.has_value() ? json(session.WorktreePath->string()) : json(nullptr)},
            {"worktreeName", optionalString(session.WorktreeName)},
            {"branchAtLaunch", optionalString(session.BranchAtLaunch)},
            {"buildConfig", optionalString(session.BuildConfig)}
        };

        return result; });

    // 1b. move_window
    // Moves/resizes an editor window through the engine's own platform layer.
    // Useful for monitor/HDR testing on macOS where external Apple Events may
    // be denied by TCC.
    server.RegisterHandler("move_window", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->window)
            return Editor::RefuseRequest("window unavailable");

        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        wndCtx->window->GetPosition(x, y);
        wndCtx->window->GetWindowSize(width, height);

        bool resizeRequested = ctx.params.contains("width") || ctx.params.contains("height");

        // Optional "monitor" index: place the window on that display's work area.
        // Defaults size to fill the target monitor unless the caller overrides width/height.
        if (ctx.params.contains("monitor"))
        {
            const int monitorIndex = ctx.params.value("monitor", 0);
            const auto monitors = Platform::EnumerateMonitors();
            const auto it = std::find_if(monitors.begin(), monitors.end(),
                                         [monitorIndex](const Platform::MonitorInfo& m)
                                         { return m.index == monitorIndex; });
            if (it == monitors.end())
                return Editor::RefuseRequest("monitor index out of range", json{{"monitorCount", monitors.size()}});
            x = it->workX;
            y = it->workY;
            if (!resizeRequested)
            {
                width = it->workWidth;
                height = it->workHeight;
                resizeRequested = true;
            }
        }

        x = ctx.params.value("x", x);
        y = ctx.params.value("y", y);
        width = ctx.params.value("width", width);
        height = ctx.params.value("height", height);

        wndCtx->window->SetPosition(x, y);
        if (resizeRequested && width > 0 && height > 0)
            wndCtx->window->SetWindowSize(width, height);
        wndCtx->window->Focus();

        int outX = 0;
        int outY = 0;
        int outW = 0;
        int outH = 0;
        wndCtx->window->GetPosition(outX, outY);
        wndCtx->window->GetWindowSize(outW, outH);
        return json{{"ok", true},
                    {"windowIndex", windowIndex},
                    {"x", outX},
                    {"y", outY},
                    {"width", outW},
                    {"height", outH}}; });

    // 2. get_log
    server.RegisterHandler("get_log", [ringBufferSink](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ringBufferSink)
            return Editor::RefuseRequest("Log ring buffer sink not available");

        size_t count = ctx.params.value("count", 100);
        size_t offset = ctx.params.value("offset", 0);
        std::string minLevelStr = ctx.params.value("minLevel", "info");
        std::string substring = ctx.params.value("filter", "");
        Logger::LogLevel minLevel = Logger::StringToLogLevel(Logger::String(minLevelStr));

        // Returns the most recent `count` matches (the tail of the log),
        // oldest-first within that window. `offset` pages further back.
        auto messages = ringBufferSink->GetMessages(minLevel, count, Logger::String(substring), offset);

        json result = json::array();
        for (auto& msg : messages)
        {
            result.push_back({
                {"level", Logger::LogLevelToString(msg.Level)},
                {"message", msg.Message},
            });
        }
        return result; });

    // 3. get_scene_hierarchy
    server.RegisterHandler("get_scene_hierarchy", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return json{{"entities", json::array()}};
        return Editor::DescribeSceneHierarchy(*world); });

    // open_scene
    //   path:          required scene path.
    //   additive:      optional (default false).
    //   restorePolicy: optional "prompt" (default) | "discard" | "restore" —
    //                  headless answer for the modals an open can raise. The
    //                  dirty-scene prompt resolves as Don't Save for both
    //                  non-prompt values; the crash-recovery prompt discards or
    //                  restores the autosave staging. With "prompt", a raised
    //                  modal silently blocks the open — poll get_editor_state
    //                  (modal field) and answer via respond_modal.
    // A failed open raises the loadFailed notice whatever the policy, because
    // the user at the editor needs it as much as the caller; the caller reads
    // the same failure from get_editor_state.sceneLoadError and dismisses the
    // notice with respond_modal ok.
    server.RegisterHandler("open_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        if (pathStr.empty())
            return Editor::RefuseRequest("Missing path");

        const bool additive = ctx.params.value("additive", false);
        const std::string policyStr = ctx.params.value("restorePolicy", "prompt");
        auto policy = Editor::SceneEditorController::OpenRestorePolicy::Prompt;
        if (policyStr == "discard")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Discard;
        else if (policyStr == "restore")
            policy = Editor::SceneEditorController::OpenRestorePolicy::Restore;
        else if (policyStr != "prompt")
            return Editor::RefuseRequest("restorePolicy must be prompt, discard, or restore");

        app.m_SceneEditor->RequestOpenScene(std::filesystem::path(pathStr), additive, policy);
        return json{{"queued", true}, {"path", pathStr}, {"additive", additive},
                    {"restorePolicy", policyStr}}; });

    // respond_modal
    // Answers the currently-visible scene modal (the ones that block scene
    // opens/saves) by choice name, equivalent to clicking its button:
    //   saveChanges   -> save | dontSave | cancel
    //   restoreBackup -> restore | discard
    //   saveAs        -> cancel
    //   revertScene   -> confirm | cancel
    //   loadFailed    -> ok
    // get_editor_state reports which modal is up under "modal".
    server.RegisterHandler("respond_modal", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");
        const std::string choice = ctx.params.value("choice", "");
        if (choice.empty())
            return Editor::RefuseRequest("Provide choice (save|dontSave|cancel|restore|discard|confirm|ok)");

        const std::string kind = app.m_SceneEditor->GetActiveModalKind();
        std::string error;
        if (!app.m_SceneEditor->RespondToActiveModal(choice, &error))
            return Editor::RefuseRequest(error);
        return json{{"ok", true}, {"modal", kind}, {"choice", choice}}; });

    // save_scene — synchronous save of the active scene document.
    //   path present -> Save As to that path (project-relative resolves under the
    //                   asset root, .scene enforced), adopting it as the document path.
    //   path absent  -> Save to the current scene path (fails if the document is untitled).
    // Routes through SceneDocumentManager::Save / SaveAs, so hierarchy-UI capture +
    // MarkClean happen exactly as the Ctrl+S / File menu path does.
    server.RegisterHandler("save_scene", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_SceneEditor)
            return Editor::RefuseRequest("Scene editor is not available");

        const std::string pathStr = ctx.params.value("path", "");
        // A degraded scene will not overwrite its own source without this. Automation is the easiest
        // way to destroy such a file silently — no modal stands in the way — so refusing is the
        // default and consent has to be typed.
        const bool force = ctx.params.value("force", false);
        const Editor::DegradedSavePolicy degradedPolicy =
            force ? Editor::DegradedSavePolicy::SaveAnyway : Editor::DegradedSavePolicy::Refuse;
        std::string error;

        if (pathStr.empty())
        {
            // Deliberately NOT the UI-replay command: it calls SaveActiveScene with the default
            // policy and carries no way to pass one, so force= could not reach the guard through it.
            if (!app.m_SceneEditor->SaveActiveScene(&error, degradedPolicy))
                return Editor::RefuseRequest(error.empty() ? "save failed" : error);
            const auto saved = app.m_SceneEditor->GetActiveScenePath();
            return json{{"ok", true}, {"path", saved ? saved->string() : std::string{}}};
        }

        std::filesystem::path scenePath(pathStr);
        if (scenePath.is_relative())
        {
            const auto& assetRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
            if (!assetRoot.empty())
                scenePath = assetRoot / scenePath;
        }
        std::string ext = scenePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".scene")
            scenePath.replace_extension(".scene");
        scenePath = scenePath.lexically_normal();

        if (!app.m_SceneEditor->SaveActiveSceneAs(scenePath, &error, degradedPolicy))
            return Editor::RefuseRequest(error.empty() ? "save-as failed" : error);
        return json{{"ok", true}, {"path", scenePath.string()}}; });

    // set_play_mode — drive the editor's Play / Pause / Stop actions.
    //   action: "enter" | "exit" | "pause" | "resume".
    //   activateGameView: optional bool, default false.
    // Routed through the same UI-replay commands the toolbar buttons use, so the
    // play-fullscreen teardown and toolbar state stay in sync with the transition.
    // Each action is legal from exactly one state and refused from any other, and
    // the response always carries the resulting "playMode" so a caller never has
    // to trust its own intent.
    //
    // activateGameView raises the Game View through the same path focus_view
    // takes, before the transition: the Game View's composite is what publishes
    // the gameplay UI host, so a HUD needs that tab up to have anywhere to mount.
    // No main window means the view can never come up, so that refuses the whole
    // transition instead of entering play with the caller believing it did; the
    // reached-the-dock-model case reports "gameViewActive" read back instead.
    //
    // Nothing is saved on the caller's behalf. Entering snapshots the world and
    // exiting restores that snapshot, so runtime mutations are discarded — see the
    // tool description for the full contract.
    server.RegisterHandler("set_play_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* playMode = app.m_PlayMode.get();
        if (!playMode)
            return Editor::RefuseRequest("Play mode is not available");

        const Editor::PlayModeRequest request(ctx.params, playMode->GetState());
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error(),
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        std::optional<ViewActivation> activation;
        if (request.ActivateGameView())
        {
            if (!app.m_Docking)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

            activation = ActivateDockedView(app, *app.m_Docking, EditorPanelIds::GameView,
                                            /*wantsViewportUiFocus=*/true);
            if (!activation->ok)
                return Editor::RefuseRequest("cannot activate the Game View; the main editor window is not available",
                                             json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});
        }

        std::string error;
        if (!app.InvokeUiReplayCommand(request.CommandId(), &error))
            return Editor::RefuseRequest(error.empty() ? std::string("play mode transition failed") : error,
                                         json{{"playMode", Editor::PlayModeStateToString(playMode->GetState())}});

        const Editor::PlayModeState after = playMode->GetState();
        json result{{"playMode", Editor::PlayModeStateToString(after)}};

        // Present only when activation was requested: a caller that did not ask
        // to move its layout has no view state to reconcile.
        if (activation)
        {
            result["gameViewActive"] = activation->active;
            result["gameViewViewportFocused"] = activation->viewportFocused;
        }

        // EnterPlayMode refuses without a world, or while a deferred scene build is
        // still resolving entities, and says so only to the log. Report it rather
        // than answering ok with the editor still sitting in Edit.
        if (request.IsEnter() && after == Editor::PlayModeState::Edit)
            return Editor::RefuseRequest("play mode did not start; no world is open, the scene is still loading, "
                                         "or the pre-play snapshot failed — see the editor log",
                                         std::move(result));

        return result; });

    // focus_view — bring the Scene View or Game View tab to the front of its dock
    // group in the main window and give that view the input focus it expects.
    //   view: "scene" | "game".
    // "active" and "focusedElementId" are read back from the dock model and the UI
    // manager after the fact, never assumed from the request.
    server.RegisterHandler("focus_view", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const Editor::ViewFocusRequest request(ctx.params);
        if (!request.Error().empty())
            return Editor::RefuseRequest(request.Error());

        if (!app.m_Docking)
            return Editor::RefuseRequest("Main editor window is not available");

        const std::string panelId = request.PanelId();
        const ViewActivation activation =
            ActivateDockedView(app, *app.m_Docking, panelId, request.WantsViewportUiFocus());
        if (!activation.ok)
            return Editor::RefuseRequest("Main editor window is not available");

        return json{{"panelId", panelId},
                    {"active", activation.active},
                    {"viewportFocused", activation.viewportFocused},
                    {"focusedElementId", activation.focusedElementId}}; });

    // 4. get_entity_components
    server.RegisterHandler("get_entity_components", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        json result;
        result["entityId"] = entityId;
        result["components"] = SerializeEntityComponents(world, entity);
        return result; });

    // 4b. terrain-specific diagnostics + sculpt stroke driver
    RegisterTerrainDebugHandlers(server, app);

    // 4c. renderer scale-bench spawner (Tools/Benchmarks/Rendering/perf-bench)
    RegisterBenchSceneHandlers(server);

    // 4d. physics world queries (the collision oracle for automated checks)
    RegisterPhysicsDebugHandlers(server);

    // 4e. composed-ground queries (the ground oracle for placement checks)
    RegisterGroundQueryDebugHandlers(server);

    // 4e2. the sky's per-frame inputs (the readback for comparing two runs input by input)
    RegisterSkyDebugHandlers(server);

    // 4e3. the Parallax steps debug view (height samples per pixel of each relief march)
    RegisterParallaxDebugHandlers(server);

    // 4e3b. world mark-ups: the agent's side of the visual conversation
    RegisterMarkupDebugHandlers(server, app);

    // 4e4. folder thumbnail bakes (the Assets panel's Generate Thumbnails)
    RegisterThumbnailDebugHandlers(server, app.m_ThumbnailProvider);

    // 4f. GPU vendor, which graphics tools are attached to the live device, and
    //     Nsight Graphics capture triggering
    RegisterGpuToolingHandlers(server, app);

    // 4g. an assistant session's binding and its permission for actions outside the
    //     editor, answered by the gate that claims them
    RegisterAssistantSessionHandlers(server);

    // 4h. the engine job system: what every pool thread runs, lane and channel backlogs
    RegisterJobSystemDebugHandlers(server);

    // 5. query_ecs
    server.RegisterHandler("query_ecs", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        std::string componentName = ctx.params.value("hasComponent", "");
        int limit = ctx.params.value("limit", 50);

        if (componentName.empty())
            return Editor::RefuseRequest("Missing hasComponent parameter");

        // Try the fast static checker table first, then fall back to reflected
        // field metadata and the older ComponentRegistry.
        auto& checkers = GetComponentCheckers();
        auto it = checkers.find(componentName);

        // Dynamic fallback: collect ALL type IDs whose registered name matches.
        // There can be multiple (e.g. C++ auto-registered + scripting blob component).
        std::vector<ECS::ComponentTypeId> matchingTypeIds;
        if (it == checkers.end())
        {
            if (const ECS::ComponentTypeId reflectedTypeId =
                    ECS::ComponentFieldRegistry::FindByName(componentName);
                reflectedTypeId != 0)
            {
                matchingTypeIds.push_back(reflectedTypeId);
            }

            const std::string suffix = "::" + componentName;
            for (const auto& regName : ECS::ComponentRegistry::GetAllComponentNames())
            {
                bool match = (regName == componentName) ||
                             (regName.size() >= suffix.size() &&
                              regName.compare(regName.size() - suffix.size(), suffix.size(), suffix) == 0);
                if (match)
                {
                    auto tid = ECS::ComponentRegistry::GetComponentTypeId(regName);
                    if (tid != 0)
                        matchingTypeIds.push_back(tid);
                }
            }
            if (matchingTypeIds.empty())
                return Editor::RefuseRequest("Unknown component: " + componentName);
        }

        auto entities = world->GetAliveEntitiesSnapshot();
        json results = json::array();
        int count = 0;

        for (auto entity : entities)
        {
            if (count >= limit)
                break;

            bool has = false;
            if (it != checkers.end())
            {
                has = it->second(world, entity);
            }
            else
            {
                auto* archetype = world->GetEntityArchetype(entity);
                if (archetype)
                {
                    const auto& sig = archetype->GetSignature();
                    for (auto tid : matchingTypeIds)
                    {
                        if (sig.Contains(tid))
                        {
                            has = true;
                            break;
                        }
                    }
                }
            }

            if (has)
            {
                json entry;
                entry["id"] = entity.id;
                entry["name"] = EntityNameFromWorld(world, entity);
                results.push_back(std::move(entry));
                ++count;
            }
        }

        return json{{"entities", std::move(results)}, {"total", count}}; });

    // 6. get_ui_tree
    server.RegisterHandler("get_ui_tree", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        int maxDepth = ctx.params.value("maxDepth", 5);
        std::string rootId = ctx.params.value("rootId", "");
        bool includeLayout = ctx.params.value("includeLayout", true);
        bool includeHidden = ctx.params.value("includeHidden", false);
        // 0 = return every text value whole, for a caller checking a long string.
        int maxTextLength = ctx.params.value("maxTextLength", Editor::kDefaultUITreeMaxTextLength);

        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        UIElement* root = wndCtx->ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        if (!rootId.empty())
        {
            UIElement* found = root->FindById(rootId);
            if (!found)
                return Editor::RefuseRequest("Element not found: " + rootId);
            root = found;
        }

        Editor::UITreeDumpOptions options{maxDepth, includeLayout, includeHidden, maxTextLength};
        Editor::UITreeDumpCompleteness completeness;
        json tree = Editor::BuildUITreeDump(root, options, completeness);
        // A self-hidden rootId yields no node at all. Returning that bare null would answer a
        // targeted lookup with something a caller reads as "no such element", when the element
        // was found and then filtered out by this request's own default. A featureless element
        // also yields nothing, which is not an error — hence the explicit predicate rather
        // than treating every null as hidden.
        if (tree.is_null() && Editor::IsHiddenForDump(*root))
            return Editor::RefuseRequest((rootId.empty() ? std::string("The root element") : "Element '" + rootId + "'") +
                                             " is hidden (display:none or not visible), so it was filtered out of the dump. "
                                             "Pass includeHidden:true to inspect it.");
        if (tree.is_null())
            tree = json::object();
        Editor::AnnotateDumpCompleteness(tree, completeness, options, Editor::UITreeDumpLookupAdvice::RootIdAvailable);
        return tree; });

    // get_panel_tree — resolve a docked panel by name and return its UI subtree.
    // Unlike get_ui_tree with rootId, this finds the actual panel content regardless
    // of where it sits in the dockspace layout (bypasses dock-config declarations).
    server.RegisterHandler("get_panel_tree", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string panelId = ctx.params.value("panelId", "");
        if (panelId.empty())
            return Editor::RefuseRequest("Provide a 'panelId' parameter (e.g. 'Hierarchy', 'Inspector')");

        int maxDepth = ctx.params.value("maxDepth", 5);
        bool includeLayout = ctx.params.value("includeLayout", true);
        bool includeHidden = ctx.params.value("includeHidden", false);
        // 0 = return every text value whole, for a caller checking a long string.
        int maxTextLength = ctx.params.value("maxTextLength", Editor::kDefaultUITreeMaxTextLength);

        if (!app.m_Docking)
            return Editor::RefuseRequest("Docking system not initialized");

        UIElement* panel = app.m_Docking->GetPanel(panelId);
        if (!panel)
        {
            // List available panel IDs to help discovery.
            json available = json::array();
            for (const auto& [id, _] : app.m_Docking->GetPanels())
                available.push_back(id);
            return Editor::RefuseRequest("Panel not found: " + panelId, json{{"availablePanels", available}});
        }

        Editor::UITreeDumpOptions options{maxDepth, includeLayout, includeHidden, maxTextLength};
        Editor::UITreeDumpCompleteness completeness;
        json tree = Editor::BuildUITreeDump(panel, options, completeness);
        if (tree.is_null() && Editor::IsHiddenForDump(*panel))
            return Editor::RefuseRequest("Panel '" + panelId +
                                             "' is hidden (display:none or not visible), so it was filtered out of the dump. "
                                             "Pass includeHidden:true to inspect it.");
        if (tree.is_null())
            tree = json::object();
        // DepthOnly: this handler takes no rootId, so its note must not send callers to one.
        Editor::AnnotateDumpCompleteness(tree, completeness, options, Editor::UITreeDumpLookupAdvice::DepthOnly);
        return tree; });

    // 7. click_element
    // Simulates a click by pressing on this frame and releasing on the next.
    // UIManager dispatches each transition on arrival, so a same-frame
    // press+release would also land; scheduling the release explicitly keeps the
    // injected gesture deterministic — handlers observe a held button for
    // exactly one frame, which is what a drag-arming control needs to see.
    server.RegisterHandler("click_element", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        float x = 0.0f;
        float y = 0.0f;

        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y coordinates");
        }

        int button = 0;
        if (ctx.params.contains("button"))
        {
            if (ctx.params["button"].is_number_integer())
            {
                button = ctx.params["button"].get<int>();
            }
            else if (ctx.params["button"].is_string())
            {
                const std::string buttonName = ctx.params["button"].get<std::string>();
                if (buttonName == "left")
                    button = 0;
                else if (buttonName == "right")
                    button = 1;
                else if (buttonName == "middle")
                    button = 2;
                else
                    return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
            else
            {
                return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
        }
        if (button < 0)
            return Editor::RefuseRequest("button must be non-negative");

        // Frame 1: move + press.
        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        // Defer the release to the next frame so the UI processes the press first.
        // The poll function runs inside FlushPendingRequests, which is called once
        // per frame *before* ui->Update(). We must skip the first poll (same frame
        // as the press) so that ui->Update() sees the press edge. On the second
        // poll (next frame), we issue the release before ui->Update() processes it.
        float clickX = x;
        float clickY = y;
        int clickButton = button;
        auto framesToSkip = std::make_shared<int>(1);
        server.EnqueueDeferredResponse(ctx.id, [wndCtx, clickX, clickY, clickButton, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false; // Not ready yet — wait for next frame.
            }
            if (!wndCtx || !wndCtx->ui)
            {
                outResult = Editor::RefuseRequest("UI manager went away before the click could be released");
                return true;
            }
            // This poll runs after the press edge's ui->Update(), so hit-testing has resolved
            // who the press was dispatched to. "clicked" only ever meant "pointer input was
            // injected at these coordinates"; report the element that actually received it,
            // the way move_pointer reports its hover, so a click that landed somewhere other
            // than the requested target is visible instead of reading as a success.
            //
            // Reported as information, not as a pass/fail verdict on the click: the resolved
            // hover is not stable enough to convict on. Two identical clicks on one dropdown
            // were measured returning different hits, the second an empty string, so a
            // boolean would have asserted a confident false negative on a click that worked.
            const std::string hit = wndCtx->ui->GetHoveredElementDebugName();
            Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), clickX, clickY);
            Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), clickButton, false);
            outResult = json{{"clicked", true}, {"x", clickX}, {"y", clickY}, {"button", clickButton}, {"hit", hit}};
            return true;
        });

        return EditorDebugServer::DeferredMarker(); });

    // move_pointer
    // Moves the UI pointer without pressing a button so MCP clients can
    // inspect hover behavior and capture stable visual evidence. The move
    // enters the window's input routing and nothing else: hover, tooltips and
    // scene-view hover all read the routed pointer, and the operating-system
    // cursor stays wherever the person using the machine has it.
    server.RegisterHandler("move_pointer", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y coordinates");
        }

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);

        // Wait until Update() has resolved the injected position, then report
        // the actual leaf hit target. This makes hover regressions observable
        // through MCP instead of relying only on screenshot timing.
        auto framesToSkip = std::make_shared<int>(1);
        server.EnqueueDeferredResponse(ctx.id, [ui, x, y, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            outResult = json{{"moved", true},
                             {"x", x},
                             {"y", y},
                             {"hovered", ui->GetHoveredElementDebugName()}};
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

    // ---- Context-menu automation -------------------------------------------
    // Native popup menus (Win32 TrackPopupMenuEx / NSMenu) run a nested OS
    // message loop that freezes the main loop — and this debug server — until
    // the menu closes, so menu flows previously required OS-level input.
    // Instead of pumping IPC from inside the nested loop (handlers would run
    // against a dead RGFrame capture context and re-entrant UI state), menus
    // are driven programmatically: open_context_menu arms a one-shot
    // interceptor and performs the click; the InterceptableContextMenu
    // decorator (which CreateContextMenu wraps around every editor menu)
    // hands over the state-resolved item tree plus a copy of the command
    // callback instead of opening the popup; invoke_menu_item runs the chosen
    // command. Contract: the capture stays valid until the next
    // open_context_menu or invoke_menu_item; invoke promptly — callbacks
    // capture their owning panels, so a scene or layout change between
    // capture and invoke acts on stale context (same risk class as
    // click_element on a rebuilt UI). A command that itself opens another
    // menu reverts to the popup (interceptor is one-shot) and will stall IPC
    // as before.
    struct MenuAutomationState
    {
        bool armed = false;
        bool captured = false;
        uint64_t generation = 0;
        InterceptableContextMenu::Capture menu;
        // Pooled-row identification for the click that opened the capture
        // ({index, tableId}), or null when the click was not over a pooled row.
        json rowInfo;
    };
    auto menuAutomation = std::make_shared<MenuAutomationState>();
    InterceptableContextMenu::SetInterceptor(
        [menuAutomation](InterceptableContextMenu::Capture&& capture) -> bool
    {
        if (!menuAutomation->armed)
            return false; // interactive use — open the popup as usual
        menuAutomation->armed = false;
        menuAutomation->captured = true;
        ++menuAutomation->generation;
        menuAutomation->menu = std::move(capture);
        return true;
    });

    auto menuCaptureToJson = [menuAutomation]() -> json
    {
        json items = json::array();
        for (const InterceptableContextMenu::CapturedItem& item : menuAutomation->menu.Items)
        {
            json j{{"path", item.Path}};
            if (item.IsSeparator)
            {
                j["separator"] = true;
            }
            else
            {
                if (item.IsSubMenu)
                    j["submenu"] = true;
                else
                    j["commandId"] = item.CommandId;
                j["enabled"] = item.Enabled;
                if (item.Checked)
                    j["checked"] = true;
                if (!item.Icon.empty())
                    j["icon"] = item.Icon;
                if (!item.Color.empty())
                    j["color"] = item.Color;
            }
            items.push_back(std::move(j));
        }
        json result{{"ok", true},
                    {"items", std::move(items)},
                    {"x", menuAutomation->menu.X},
                    {"y", menuAutomation->menu.Y},
                    {"generation", menuAutomation->generation}};
        if (!menuAutomation->rowInfo.is_null())
            result["row"] = menuAutomation->rowInfo;
        return result;
    };

    // Resolves a pooled list from an automation id: the element may be the
    // ListView itself, a TableView (its body is the pooled list), or a
    // container holding exactly one pooled list.
    auto resolvePooledList = [](UIManager* ui, const std::string& tableId, std::string& outError) -> ListView*
    {
        UIElement* root = ui->GetRootElement();
        UIElement* el = root ? root->FindById(tableId) : nullptr;
        if (!el)
        {
            outError = "Element not found: " + tableId;
            return nullptr;
        }
        if (auto* list = dynamic_cast<ListView*>(el))
            return list;
        if (auto* table = dynamic_cast<TableView*>(el))
            return &table->Body();
        std::vector<ListView*> found;
        std::function<void(UIElement*)> walk = [&](UIElement* e)
        {
            if (auto* list = dynamic_cast<ListView*>(e))
            {
                found.push_back(list);
                return; // a list's internals cannot host another pooled-list target
            }
            for (const auto& child : e->GetChildren())
            {
                if (child)
                    walk(child.get());
            }
        };
        walk(el);
        if (found.size() == 1)
            return found.front();
        outError = found.empty()
            ? "No pooled list (ListView/TableView) under element '" + tableId + "'"
            : "Element '" + tableId + "' contains multiple pooled lists - pass the list's own id";
        return nullptr;
    };

    // Pooled-row identification for a click point. The injected mouse move
    // resolved hover through the real hit-test, so walk up from the hovered
    // leaf to the owning pooled list and ask it which logical row contains the
    // point. Null when the click was not over a bound pooled row.
    auto resolvePooledRowAtPoint = [](UIManager* ui, float x, float y) -> json
    {
        for (UIElement* el = ui->GetHoveredElement(); el; el = el->GetParent())
        {
            auto* list = dynamic_cast<ListView*>(el);
            if (!list)
                continue;
            const int index = list->GetIndexAtPoint(x, y);
            if (index < 0)
                return json();
            json row{{"index", index}};
            if (!list->GetId().empty())
                row["tableId"] = list->GetId();
            return row;
        }
        return json();
    };

    // open_context_menu
    // Right-clicks (default) the target and returns the context menu's item
    // tree instead of opening the OS popup. Targeting mirrors click_element,
    // plus a pooled-row mode for virtualized lists (TableView/ListView), whose
    // recycled row widgets have no per-logical-row element ids:
    //   elementId | x,y      : click location, or
    //   tableId + rowIndex   : pooled list automation id (the ListView, its
    //                          TableView, or a container with exactly one list)
    //                          + logical provider row index. The row is scrolled
    //                          into view and its live cell is clicked exactly
    //                          like a user right-click.
    //   button               : "right" (default) or "left" (toolbar dropdowns).
    //   windowIndex          : optional.
    // Response: {items:[{path, commandId|submenu|separator, enabled, checked?, icon?, color?}],
    //            x, y, generation, row?:{index, tableId}}. The row field is
    //            also reported when plain x/y targeting lands on a pooled row.
    //            Follow with invoke_menu_item.
    server.RegisterHandler("open_context_menu", [&app, &server, menuAutomation, menuCaptureToJson, resolvePooledList, resolvePooledRowAtPoint](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        const std::string buttonName = ctx.params.value("button", std::string("right"));
        int button = 1;
        if (buttonName == "right")
            button = 1;
        else if (buttonName == "left")
            button = 0;
        else
            return Editor::RefuseRequest("button must be right or left");

        const bool hasTableId = ctx.params.contains("tableId");
        const bool hasRowIndex = ctx.params.contains("rowIndex");
        if (hasTableId != hasRowIndex)
            return Editor::RefuseRequest("tableId and rowIndex must be provided together");

        constexpr int kMaxFramesWaitingForMenu = 25;

        if (hasTableId)
        {
            const std::string tableId = ctx.params["tableId"].get<std::string>();
            const int rowIndex = ctx.params["rowIndex"].get<int>();

            std::string resolveError;
            ListView* list = resolvePooledList(ui, tableId, resolveError);
            if (!list)
                return Editor::RefuseRequest(resolveError);
            const int count = list->GetItemCount();
            if (rowIndex < 0 || rowIndex >= count)
                return Editor::RefuseRequest("rowIndex " + std::to_string(rowIndex) + " out of range (list '"
                                                 + tableId + "' has " + std::to_string(count) + " items)");

            // The logical row may be outside the bound window; scroll it into
            // view and let the next virtualization pass bind a pooled cell.
            list->ScrollIndexIntoView(rowIndex);

            // Phase machine: bind-wait -> press -> release -> capture-wait. The
            // list is re-resolved by id every poll so a panel rebuild between
            // frames cannot leave a dangling pointer. Press/release keep the
            // click_element cadence (down edge gets a full ui->Update() before
            // the up edge).
            struct RowClickState
            {
                int phase = 0;         // polls since enqueue (first runs on the press-frame flush)
                int pressedPhase = -1; // poll at which the press was injected
                float clickX = 0.0f;
                float clickY = 0.0f;
            };
            auto st = std::make_shared<RowClickState>();
            constexpr int kMaxFramesWaitingForBind = 15;
            server.EnqueueDeferredResponse(ctx.id,
                [ui, wndCtx, menuAutomation, menuCaptureToJson, resolvePooledList, tableId, rowIndex, button, st](json& outResult) -> bool
            {
                ++st->phase;
                if (st->pressedPhase < 0)
                {
                    std::string resolveError;
                    ListView* list = resolvePooledList(ui, tableId, resolveError);
                    if (!list)
                    {
                        outResult = Editor::RefuseRequest(resolveError);
                        return true;
                    }
                    UIElement* cell = list->GetCellForIndex(rowIndex);
                    if (cell && cell->GetLayoutWidth() > 0.0f && cell->GetLayoutHeight() > 0.0f)
                    {
                        st->clickX = cell->GetLayoutX() + cell->GetLayoutWidth() * 0.5f;
                        st->clickY = cell->GetLayoutY() + cell->GetLayoutHeight() * 0.5f;
                        menuAutomation->armed = true;
                        menuAutomation->captured = false;
                        menuAutomation->menu = {};
                        menuAutomation->rowInfo = json{{"index", rowIndex}, {"tableId", tableId}};
                        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), st->clickX, st->clickY);
                        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);
                        st->pressedPhase = st->phase;
                        return false;
                    }
                    if (st->phase > kMaxFramesWaitingForBind)
                    {
                        outResult = Editor::RefuseRequest("Row " + std::to_string(rowIndex) + " of '" + tableId
                                                              + "' did not bind within " + std::to_string(kMaxFramesWaitingForBind)
                                                              + " frames — is the list visible?");
                        return true;
                    }
                    return false;
                }
                if (st->phase == st->pressedPhase + 1)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), st->clickX, st->clickY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                    return false;
                }
                if (menuAutomation->captured)
                {
                    outResult = menuCaptureToJson();
                    outResult["clickX"] = st->clickX;
                    outResult["clickY"] = st->clickY;
                    return true;
                }
                if (st->phase > st->pressedPhase + kMaxFramesWaitingForMenu)
                {
                    menuAutomation->armed = false;
                    outResult = Editor::RefuseRequest("No context menu opened within " + std::to_string(kMaxFramesWaitingForMenu)
                                                          + " frames — does row " + std::to_string(rowIndex) + " of '" + tableId
                                                          + "' have a context menu?");
                    return true;
                }
                return false;
            });
            return EditorDebugServer::DeferredMarker();
        }

        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId, x/y coordinates, or tableId+rowIndex");
        }

        menuAutomation->armed = true;
        menuAutomation->captured = false;
        menuAutomation->menu = {};
        menuAutomation->rowInfo = json();

        // Same press-now / release-next-frame cadence as click_element; the
        // menu opens inside ui->Update() when the click handler runs, and the
        // armed interceptor captures it in the same frame.
        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        auto phase = std::make_shared<int>(0);
        const float clickX = x;
        const float clickY = y;
        server.EnqueueDeferredResponse(ctx.id,
            [ui, wndCtx, menuAutomation, menuCaptureToJson, resolvePooledRowAtPoint, clickX, clickY, button, phase](json& outResult) -> bool
        {
            ++(*phase);
            // Deferred polls run in the SAME FlushPendingRequests as the
            // handler that queued them, so phase 1 is still the press frame —
            // releasing here would erase the down edge before ui->Update()
            // ever sees it (same skip click_element does). Release on the
            // next frame's poll instead.
            if (*phase == 1)
                return false;
            if (*phase == 2)
            {
                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), clickX, clickY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                return false;
            }
            if (menuAutomation->captured)
            {
                // Identify the pooled row under the click (if any) so scripts
                // can verify which logical row's menu was captured.
                menuAutomation->rowInfo = resolvePooledRowAtPoint(ui, clickX, clickY);
                outResult = menuCaptureToJson();
                return true;
            }
            if (*phase > kMaxFramesWaitingForMenu)
            {
                menuAutomation->armed = false;
                outResult = Editor::RefuseRequest("No context menu opened within " + std::to_string(kMaxFramesWaitingForMenu)
                                                      + " frames — is the target menu-clickable?");
                return true;
            }
            return false;
        });
        return EditorDebugServer::DeferredMarker(); });

    // get_context_menu
    // Returns the currently captured menu (from the last open_context_menu),
    // or {open:false} when none is pending.
    server.RegisterHandler("get_context_menu", [menuAutomation, menuCaptureToJson](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        if (!menuAutomation->captured)
            return json{{"open", false}};
        json result = menuCaptureToJson();
        result["open"] = true;
        return result; });

    // invoke_menu_item
    // Invokes a command on the captured menu through a copy of the menu's
    // command callback — equivalent to the user clicking that row in the
    // popup. The capture is consumed on success.
    //   commandId : integer from open_context_menu, or
    //   path      : exact item path ("Create/Entity") or unambiguous leaf title.
    server.RegisterHandler("invoke_menu_item", [&app, menuAutomation](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!menuAutomation->captured)
            return Editor::RefuseRequest("No captured context menu — call open_context_menu first");

        const InterceptableContextMenu::CapturedItem* chosen = nullptr;
        if (ctx.params.contains("commandId"))
        {
            const uint32_t commandId = ctx.params["commandId"].get<uint32_t>();
            for (const auto& item : menuAutomation->menu.Items)
            {
                if (!item.IsSeparator && !item.IsSubMenu && item.CommandId == commandId)
                {
                    chosen = &item;
                    break;
                }
            }
            if (!chosen)
                return Editor::RefuseRequest("No menu item with commandId " + std::to_string(commandId));
        }
        else if (ctx.params.contains("path"))
        {
            const std::string pathParam = ctx.params["path"].get<std::string>();
            int leafMatches = 0;
            const InterceptableContextMenu::CapturedItem* leafHit = nullptr;
            for (const auto& item : menuAutomation->menu.Items)
            {
                if (item.IsSeparator || item.IsSubMenu)
                    continue;
                if (item.Path == pathParam)
                {
                    chosen = &item;
                    break;
                }
                const size_t slash = item.Path.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? item.Path : item.Path.substr(slash + 1);
                if (leaf == pathParam)
                {
                    ++leafMatches;
                    leafHit = &item;
                }
            }
            if (!chosen && leafMatches == 1)
                chosen = leafHit;
            if (!chosen)
                return Editor::RefuseRequest(leafMatches > 1
                           ? "Menu title '" + pathParam + "' is ambiguous — pass the full path or commandId"
                           : "No menu item matches '" + pathParam + "'");
        }
        else
        {
            return Editor::RefuseRequest("Provide commandId or path");
        }

        if (!chosen->Enabled)
            return Editor::RefuseRequest("Menu item is disabled: " + chosen->Path);
        if (!menuAutomation->menu.Invoke)
            return Editor::RefuseRequest("Captured menu has no command handler");

        const uint32_t commandId = chosen->CommandId;
        const std::string itemPath = chosen->Path;
        // Consume the capture before invoking: the callback may rebuild UI or
        // open follow-up flows, and a stale capture must not stay addressable.
        auto invoke = std::move(menuAutomation->menu.Invoke);
        menuAutomation->captured = false;
        menuAutomation->menu = {};
        menuAutomation->rowInfo = json();

        // Bind the window's UI context so callbacks that post deferred UI work
        // (dispatcher/scheduler TLS) behave exactly as they do when invoked
        // from the popup inside UI event dispatch.
        UIManager* ui = !app.m_Windows.empty() && app.m_Windows[0] ? app.m_Windows[0]->ui.get() : nullptr;
        if (ui)
        {
            UI::UiContextScope uiScope(ui->GetDispatcher(), ui->GetScheduler());
            invoke(commandId);
        }
        else
        {
            invoke(commandId);
        }
        return json{{"ok", true}, {"invoked", itemPath}, {"commandId", commandId}}; });

    // set_text_field
    // Sets a TextField's value by element id. send_key cannot type punctuation
    // (paths, URLs), so text-entry flows (Package Manager add/relocate/re-pin,
    // future search/rename automation) are driven by setting the field value
    // directly — the field's click handlers read GetValue() exactly as they
    // would after real typing.
    server.RegisterHandler("set_text_field", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        if (!ctx.params.contains("elementId") || !ctx.params.contains("text"))
            return Editor::RefuseRequest("Provide elementId and text");
        const std::string elementId = ctx.params["elementId"].get<std::string>();
        const std::string text = ctx.params["text"].get<std::string>();

        UIElement* root = wndCtx->ui->GetRootElement();
        UIElement* target = root ? root->FindById(elementId) : nullptr;
        if (!target)
            return Editor::RefuseRequest("Element not found: " + elementId);
        auto* field = dynamic_cast<TextField*>(target);
        if (!field)
            return Editor::RefuseRequest("Element is not a TextField: " + elementId);

        const std::string previous = field->GetValue();
        field->SetValue(text);
        return json{{"ok", true}, {"elementId", elementId}, {"previous", previous}}; });

    // input_text
    // Types a string through the UI system's real text-input path: one
    // UIManager::OnChar per codepoint — the same entry point the platform
    // layer feeds from WM_CHAR / glfwSetCharCallback — so any focused editable
    // (TextField, float/int field editors, search fields) receives it exactly
    // as if typed, including the per-keystroke value-changed callbacks that
    // set_text_field bypasses. Punctuation and non-ASCII are fine (UTF-8).
    //   text:      required UTF-8 string. Control characters are rejected —
    //              use commit:true for Enter, send_key for Tab/Escape.
    //   elementId: optional element to focus first (same focus path as
    //              clicking it). Omitted: current focus.
    //   clear:     select-all before typing (default false), so the text
    //              replaces the current content. Needed for free-text fields,
    //              which keep their caret on a mouse-equivalent focus; value
    //              fields select-all on focus and replace either way.
    //   commit:    press Enter after the text (default false) — commits
    //              numeric field editors and fires Enter-bound callbacks.
    // Focus-in, select-all, chars, and the Enter press all queue as pending
    // UI inputs and dispatch in order inside the next UIManager::Update, so a
    // single call reproduces the full click-and-type interaction.
    server.RegisterHandler("input_text", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        if (!ctx.params.contains("text") || !ctx.params["text"].is_string())
            return Editor::RefuseRequest("Provide text (string)");
        const std::string text = ctx.params["text"].get<std::string>();

        // Decode UTF-8 up-front so malformed input fails before any event is queued.
        std::vector<uint32_t> codepoints;
        codepoints.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char c0 = static_cast<unsigned char>(text[i]);
            uint32_t cp = 0;
            size_t len = 0;
            if (c0 < 0x80) { cp = c0; len = 1; }
            else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1Fu; len = 2; }
            else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0Fu; len = 3; }
            else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07u; len = 4; }
            else
                return Editor::RefuseRequest("text is not valid UTF-8");
            if (i + len > text.size())
                return Editor::RefuseRequest("text is not valid UTF-8");
            for (size_t k = 1; k < len; ++k)
            {
                const unsigned char cc = static_cast<unsigned char>(text[i + k]);
                if ((cc & 0xC0) != 0x80)
                    return Editor::RefuseRequest("text is not valid UTF-8");
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            if (cp == '\n' || cp == '\r' || !Utf8::IsTextInputCodepoint(cp))
                return Editor::RefuseRequest("text contains a control character the UI would drop — use commit:true for Enter, send_key for others");
            codepoints.push_back(cp);
            i += len;
        }

        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            UIElement* root = ui->GetRootElement();
            UIElement* target = root ? root->FindById(elementId) : nullptr;
            if (!target)
                return Editor::RefuseRequest("Element not found: " + elementId);
            ui->FocusElement(target);
        }
        if (ui->GetFocusedElementId().empty())
            return Editor::RefuseRequest("No focused element — pass elementId or focus one first");
        const std::string focusId = ui->GetFocusedElementId();

        if (ctx.params.value("clear", false))
        {
            // Select-all through the same key path as Ctrl+A so the typed text
            // replaces the content; the release keeps mod-held state balanced.
            ui->OnKey('A', 1 /*press*/, 0x0002 /*ctrl*/);
            ui->OnKey('A', 0 /*release*/, 0);
        }

        for (const uint32_t cp : codepoints)
            ui->OnChar(cp);

        const bool commit = ctx.params.value("commit", false);
        constexpr int kKeyEnter = 257; // GLFW_KEY_ENTER
        if (commit)
            ui->OnKey(kKeyEnter, 1 /*press*/, 0);

        // Defer the response one frame so ui->Update() has dispatched the whole
        // queued sequence (focus-in, select-all, chars, Enter press) before the
        // field value is read back; the Enter release mirrors send_key's tap.
        auto framesToSkip = std::make_shared<int>(1);
        const size_t cpCount = codepoints.size();
        server.EnqueueDeferredResponse(ctx.id, [ui, focusId, commit, cpCount, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            if (commit)
                ui->OnKey(kKeyEnter, 0 /*release*/, 0);

            outResult = json{{"ok", true},
                             {"focusId", focusId},
                             {"codepoints", cpCount},
                             {"committed", commit}};
            // Best-effort readback of the focused field's value so callers can
            // verify what the control now holds without a second round-trip.
            UIElement* root = ui->GetRootElement();
            UIElement* el = root ? root->FindById(focusId) : nullptr;
            if (auto* sf = dynamic_cast<Field<std::string>*>(el))
                outResult["value"] = sf->GetValue();
            else if (auto* ff = dynamic_cast<Field<float>*>(el))
                outResult["value"] = ff->GetValue();
            else if (auto* iff = dynamic_cast<Field<int>*>(el))
                outResult["value"] = iff->GetValue();
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

    // send_key
    // Injects a keyboard event through WindowInputRouter, the path a real key
    // takes: the window's global shortcut interception (onKeyPre), then the
    // chrome UI, then a live play surface's HUD and gameplay, then the editor's
    // own actions. So an injected Ctrl+W closes what a typed Ctrl+W closes and
    // an injected W walks the character a typed W walks — which is the whole
    // point of driving the editor over this protocol. Text-entry behaviour that
    // needs no chain (Tab traversal, Enter commit, Escape cancel) is unaffected:
    // the focused element still answers first.
    //   key:    a name (Tab, Enter, Return, NumpadEnter, Escape, Space,
    //           Backspace, Delete, Home, End, Up, Down, Left, Right, A-Z, 0-9)
    //           or a raw GLFW keycode integer.
    //   mods:   optional shift|control|alt|super as a string, array of strings,
    //           or an integer GLFW mod mask.
    //   action: "tap" (default; press now + release next frame), "press",
    //           "release", or "repeat" (OS key-repeat edge, e.g. held Tab).
    //   count:  tap only — number of taps to issue (e.g. Tab x3). Default 1.
    server.RegisterHandler("send_key", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        if (!ctx.params.contains("key"))
            return Editor::RefuseRequest("Provide key (name or GLFW keycode integer)");

        auto toLower = [](std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; };

        int keyCode = -1;
        const auto& kparam = ctx.params["key"];
        if (kparam.is_number_integer())
        {
            keyCode = kparam.get<int>();
        }
        else if (kparam.is_string())
        {
            const std::string name = kparam.get<std::string>();
            static const std::unordered_map<std::string, int> kNamed = {
                {"tab", 258}, {"enter", 257}, {"return", 257}, {"numpadenter", 335},
                {"escape", 256}, {"esc", 256}, {"space", 32}, {"backspace", 259},
                {"delete", 261}, {"home", 268}, {"end", 269},
                {"up", 265}, {"down", 264}, {"left", 263}, {"right", 262}};
            auto it = kNamed.find(toLower(name));
            if (it != kNamed.end())
                keyCode = it->second;
            else if (name.size() == 1)
            {
                const char c = name[0];
                if (c >= 'a' && c <= 'z') keyCode = 'A' + (c - 'a');
                else if (c >= 'A' && c <= 'Z') keyCode = c;
                else if (c >= '0' && c <= '9') keyCode = c;
            }
            if (keyCode < 0)
                return Editor::RefuseRequest("Unknown key name: " + name);
        }
        else
        {
            return Editor::RefuseRequest("key must be a string name or integer keycode");
        }

        int mods = 0;
        auto applyMod = [&mods, &toLower](const std::string& m) {
            const std::string s = toLower(m);
            if (s == "shift") mods |= 0x0001;
            else if (s == "control" || s == "ctrl") mods |= 0x0002;
            else if (s == "alt") mods |= 0x0004;
            else if (s == "super" || s == "cmd" || s == "meta") mods |= 0x0008;
        };
        if (ctx.params.contains("mods"))
        {
            const auto& mp = ctx.params["mods"];
            if (mp.is_string())
                applyMod(mp.get<std::string>());
            else if (mp.is_array())
                for (const auto& e : mp) { if (e.is_string()) applyMod(e.get<std::string>()); }
            else if (mp.is_number_integer())
                mods = mp.get<int>();
        }

        const std::string action = ctx.params.value("action", std::string("tap"));
        constexpr int kRelease = 0, kPress = 1, kRepeat = 2;

        if (action == "press")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kPress, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "press"}, {"mods", mods}};
        }
        if (action == "release")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kRelease, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "release"}, {"mods", mods}};
        }
        if (action == "repeat")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kRepeat, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "repeat"}, {"mods", mods}};
        }
        if (action != "tap")
            return Editor::RefuseRequest("action must be tap, press, release, or repeat");

        int count = ctx.params.value("count", 1);
        if (count < 1) count = 1;
        if (count > 64) count = 64;

        // Press (count times) this frame; defer the release to the next frame so
        // ui->Update() processes the press edge(s) first (mirrors click_element).
        for (int i = 0; i < count; ++i)
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kPress, mods);

        auto framesToSkip = std::make_shared<int>(1);
        const int relKey = keyCode;
        const int relMods = mods;
        const int relCount = count;
        server.EnqueueDeferredResponse(ctx.id, [wndCtx, ui, relKey, relMods, relCount, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            Editor::InjectKey(wndCtx->inputConfig, ui, relKey, 0 /*release*/, relMods);
            outResult = json{{"ok", true}, {"key", relKey}, {"action", "tap"}, {"count", relCount}, {"mods", relMods}};
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

    // send_text — inject UTF-8 text into the currently focused UI control.
    // This mirrors the platform character callback and complements send_key,
    // which is intentionally limited to physical key press/release events.
    server.RegisterHandler("send_text", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        if (!ctx.params.contains("text") || !ctx.params["text"].is_string())
            return Editor::RefuseRequest("Provide UTF-8 string parameter 'text'");

        const std::string text = ctx.params["text"].get<std::string>();
        std::size_t at = 0;
        int sent = 0;
        while (at < text.size())
        {
            const unsigned char lead = static_cast<unsigned char>(text[at++]);
            std::uint32_t codepoint = 0;
            int continuationCount = 0;
            if (lead < 0x80) codepoint = lead;
            else if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1F; continuationCount = 1; }
            else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0F; continuationCount = 2; }
            else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07; continuationCount = 3; }
            else return Editor::RefuseRequest("Invalid UTF-8 lead byte");

            for (int i = 0; i < continuationCount; ++i)
            {
                if (at >= text.size())
                    return Editor::RefuseRequest("Truncated UTF-8 sequence");
                const unsigned char next = static_cast<unsigned char>(text[at++]);
                if ((next & 0xC0) != 0x80)
                    return Editor::RefuseRequest("Invalid UTF-8 continuation byte");
                codepoint = (codepoint << 6) | (next & 0x3F);
            }
            if (Editor::InjectChar(wndCtx->inputConfig, wndCtx->ui.get(), codepoint))
                ++sent;
        }
        return json{{"ok", true}, {"codepoints", sent},
                    {"focusedElementId", wndCtx->ui->GetFocusedElementId()}}; });

    // Open the global palette directly, naming the intent rather than relying on
    // a key binding staying bound to it.
    server.RegisterHandler("show_universal_search", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        if (app.m_UniversalSearch)
            app.m_UniversalSearch->Toggle();
        return json{{"ok", true},
                    {"open", app.m_UniversalSearch && app.m_UniversalSearch->IsOpen()}}; });

    // simulate_mouse_drag
    // Drives a press-move-release drag across frames so controls that arm on
    // press, begin dragging on later move, and commit on release see the same
    // event sequence as a real mouse gesture. button defaults to left; middle
    // and right match click_element. holdBeforeReleaseFrames keeps the button
    // down at the end so a concurrent screenshot can observe in-flight drag.
    server.RegisterHandler("simulate_mouse_drag", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        UIElement* root = ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        float startX = 0.0f;
        float startY = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(root, elementId, startX, startY, targetError))
                return targetError;
        }
        else if (ctx.params.contains("startX") && ctx.params.contains("startY"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "startX", "startY", startX, startY, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or startX/startY");
        }

        if (!ctx.params.contains("endX") || !ctx.params.contains("endY"))
            return Editor::RefuseRequest("Provide endX/endY coordinates");

        float endX = 0.0f;
        float endY = 0.0f;
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "endX", "endY", endX, endY, coordError))
                return coordError;
        }
        // Allow a longer single gesture for sustained-drag performance tests.
        // Its deferred-response budget is scoped to this request; all other
        // automation operations retain the normal 30-frame timeout.
        const int holdFrames = std::max(1, std::min(4, ctx.params.value("holdFrames", 1)));
        const int holdBeforeReleaseFrames =
            std::max(0, std::min(60, ctx.params.value("holdBeforeReleaseFrames", 0)));
        const int maxSteps = 240;
        const int steps = std::max(2, std::min(maxSteps, ctx.params.value("steps", 20)));
        const int pollBudget = holdFrames + steps + holdBeforeReleaseFrames + 4;

        int button = 0;
        if (ctx.params.contains("button"))
        {
            if (ctx.params["button"].is_number_integer())
            {
                button = ctx.params["button"].get<int>();
            }
            else if (ctx.params["button"].is_string())
            {
                const std::string buttonName = ctx.params["button"].get<std::string>();
                if (buttonName == "left")
                    button = 0;
                else if (buttonName == "right")
                    button = 1;
                else if (buttonName == "middle")
                    button = 2;
                else
                    return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
            else
            {
                return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
        }
        if (button < 0)
            return Editor::RefuseRequest("button must be non-negative");

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        auto frame = std::make_shared<int>(-holdFrames);
        auto pressedHovered = std::make_shared<std::string>();
        auto pressedCaptureId = std::make_shared<std::string>();
        auto pressedCaptured = std::make_shared<bool>(false);
        auto dragDropSession = std::make_shared<bool>(false);
        server.EnqueueDeferredResponse(ctx.id,
            [ui, wndCtx, startX, startY, endX, endY, steps, holdBeforeReleaseFrames, button, frame,
             pressedHovered, pressedCaptureId, pressedCaptured, dragDropSession](json& outResult) -> bool
            {
                if (const auto* dragDrop = ui->GetDragDropManager(); dragDrop && dragDrop->IsDragging())
                    *dragDropSession = true;
                if (*frame < 0)
                {
                    ++(*frame);
                    return false;
                }

                if (*frame < steps)
                {
                    if (*frame == 0)
                    {
                        *pressedHovered = ui->GetHoveredElementDebugName();
                        *pressedCaptureId = ui->GetCaptureId();
                        *pressedCaptured = ui->IsMouseCaptured();
                    }
                    const float t = static_cast<float>(*frame + 1) / static_cast<float>(steps);
                    const float x = startX + (endX - startX) * t;
                    const float y = startY + (endY - startY) * t;
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
                    ++(*frame);
                    return false;
                }

                if (*frame < steps + holdBeforeReleaseFrames)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                    ++(*frame);
                    return false;
                }

                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                // "dragged" reports the pointer gesture; "dragDropSession" whether it started a
                // drag-and-drop session, which is what a drop needs.
                outResult = json{{"dragged", true},
                                 {"dragDropSession", *dragDropSession},
                                 {"startX", startX},
                                 {"startY", startY},
                                 {"endX", endX},
                                 {"endY", endY},
                                 {"steps", steps},
                                 {"button", button},
                                 {"pressedHoveredElement", *pressedHovered},
                                 {"pressedMouseCaptured", *pressedCaptured},
                                 {"pressedCaptureElementId", *pressedCaptureId}};
                return true;
            }, pollBudget);

        return EditorDebugServer::DeferredMarker(); });

    // perform_drop
    // Drops asset files at a point in a window, through that window's drag-and-drop manager
    // (PerformAssetFileDrop): the drop target under the point runs its own CanDrop and
    // PerformDrop, exactly as for a drop made with the mouse, without a pointer gesture to
    // start the drag. Files outside the asset sources are refused, never moved in.
    server.RegisterHandler("perform_drop", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui || !wndCtx->ui->GetRootElement())
            return Editor::RefuseRequest("No UI manager available");

        if (!ctx.params.contains("paths") || !ctx.params["paths"].is_array() || ctx.params["paths"].empty())
            return Editor::RefuseRequest("Provide paths: an array of asset paths, absolute or project-relative");
        const AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
        std::vector<std::filesystem::path> paths;
        for (const json& entry : ctx.params["paths"])
        {
            if (!entry.is_string())
                return Editor::RefuseRequest("paths must hold strings");
            const std::string requested = entry.get<std::string>();
            const std::filesystem::path resolved = Editor::ResolveOpenAssetPath(assetManager, requested);
            if (resolved.empty())
                return Editor::RefuseRequest("Cannot resolve asset path: " + requested);
            paths.push_back(resolved);
        }
        std::vector<std::filesystem::path> sourceRoots;
        for (const AssetSourceDesc& source : assetManager.GetRegisteredSources())
            sourceRoots.push_back(source.Root);

        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            if (!ctx.params["elementId"].is_string())
                return Editor::RefuseRequest("elementId must be a string: an element id from get_ui_tree");
            json targetError;
            if (!Editor::ResolveElementPointerTarget(wndCtx->ui->GetRootElement(),
                                                     ctx.params["elementId"].get<std::string>(), x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y");
        }

        const Editor::DropOutcome outcome = Editor::PerformAssetFileDrop(*wndCtx->ui, paths, sourceRoots, x, y);
        if (!outcome.Refusal.empty())
            return Editor::RefuseRequest(outcome.Refusal);
        json pathsOut = json::array();
        for (const auto& path : paths)
            pathsOut.push_back(path.lexically_normal().string());
        // "accepted": the target took the payload and its PerformDrop ran; PerformDrop reports
        // no result, so whether the drop did what it should is read from its effect.
        return json{{"accepted", outcome.Accepted}, {"reason", outcome.Reason},
                    {"target", outcome.TargetElement}, {"x", x}, {"y", y}, {"paths", pathsOut}}; });

    // simulate_double_click_drag
    // First click down/up, then a second press on the same spot (timeline double-click
    // insert) while held, move, and release — matches create-key-then-drag in one gesture.
    server.RegisterHandler("simulate_double_click_drag", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->ui)
            return Editor::RefuseRequest("No UI manager available");

        // Unlike its siblings this method takes no windowIndex and has always
        // driven the main window.
        auto* wndCtx = app.m_Windows[0].get();
        auto* ui = wndCtx->ui.get();
        UIElement* root = ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        float startX = 0.0f;
        float startY = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(root, elementId, startX, startY, targetError))
                return targetError;
        }
        else if (ctx.params.contains("startX") && ctx.params.contains("startY"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "startX", "startY", startX, startY, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or startX/startY");
        }

        if (!ctx.params.contains("endX") || !ctx.params.contains("endY"))
            return Editor::RefuseRequest("Provide endX/endY coordinates");

        float endX = 0.0f;
        float endY = 0.0f;
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "endX", "endY", endX, endY, coordError))
                return coordError;
        }
        const int gapFrames = std::max(1, std::min(8, ctx.params.value("gapFrames", 2)));
        const int holdFrames = std::max(0, std::min(4, ctx.params.value("holdFrames", 1)));
        const int maxSteps = std::max(2, 24 - gapFrames - holdFrames);
        const int steps = std::max(2, std::min(maxSteps, ctx.params.value("steps", 16)));

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, true);

        auto frame = std::make_shared<int>(0);
        server.EnqueueDeferredResponse(ctx.id,
            [wndCtx, startX, startY, endX, endY, gapFrames, holdFrames, steps, frame](json& outResult) -> bool
            {
                if (*frame == 0)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, false);
                    ++(*frame);
                    return false;
                }

                if (*frame <= gapFrames)
                {
                    ++(*frame);
                    return false;
                }

                if (*frame == gapFrames + 1)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, true);
                    ++(*frame);
                    return false;
                }

                if (*frame <= gapFrames + 1 + holdFrames)
                {
                    ++(*frame);
                    return false;
                }

                const int dragFrame = *frame - (gapFrames + 2 + holdFrames);
                if (dragFrame < steps)
                {
                    const float t = static_cast<float>(dragFrame + 1) / static_cast<float>(steps);
                    const float x = startX + (endX - startX) * t;
                    const float y = startY + (endY - startY) * t;
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
                    ++(*frame);
                    return false;
                }

                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, false);
                outResult = json{{"doubleClickDragged", true},
                                 {"startX", startX},
                                 {"startY", startY},
                                 {"endX", endX},
                                 {"endY", endY},
                                 {"steps", steps}};
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // 8. create_entity
    server.RegisterHandler("create_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        // Validate before Create so a bad request doesn't leave an orphan entity behind.
        ECS::EntityHandle parentHandle = ECS::EntityHandle::Invalid();
        if (ctx.params.contains("parentId"))
        {
            const uint32_t parentId = ctx.params.value("parentId", 0u);
            parentHandle = ECS::EntityHandle(parentId);
            if (parentId == 0 || !world->IsValid(parentHandle))
                return Editor::RefuseRequest("Invalid parentId");
        }

        // A port edit is one undo step: the entity is built and committed as already applied
        // (the markup_create precedent), so undo removes it and redo revives the same handle.
        // A refused build removes what it began and records nothing.
        constexpr const char* kCreateEntityLabel = "Create Entity";
        ECS::EntityHandle entity{};
        json refusal;
        if (app.m_UndoRedo)
        {
            Editor::CommitGenericEdit(*world, *app.m_UndoRedo, kCreateEntityLabel, [&]() {
                refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
                if (!refusal.is_null())
                    return;
                app.m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::DuplicateEntitiesCommand>(
                    kCreateEntityLabel, world, app.m_ChangeNotifications.get(), std::vector<ECS::EntityHandle>{entity},
                    nullptr, nullptr));
            });
        }
        else
        {
            refusal = BuildRequestedEntityOrNone(world, ctx.params, parentHandle, entity);
        }
        if (!refusal.is_null())
            return refusal;

        // Notify the editor so the hierarchy panel refreshes.
        if (app.m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = world;
            e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            app.m_ChangeNotifications->NotifyWorldStructureChanged(e);
        }

        return json{{"entityId", entity.id}}; });

    // 8b. spawn_model — instantiate a ModelAsset (FBX/glTF) into the scene
    // and optionally attach an AnimationClip to its skinned entities. Wraps
    // ModelEntityFactory so AI agents can drive the same flow as a manual
    // FBX drop into the scene view. Uses the deferred response pattern so
    // async asset loads don't deadlock the IPC main-thread.
    server.RegisterHandler("spawn_model", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const std::string modelPath = ctx.params.value("path", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'path' (e.g. 'Models/BusinessMale.fbx')");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve model path: " + modelPath);

        const std::string clipPath = ctx.params.value("clipPath", std::string{});
        GUID clipGuid;
        if (!clipPath.empty())
        {
            clipGuid = am.ResolveAssetGuid(clipPath);
            if (clipGuid.IsNull())
                return Editor::RefuseRequest("Could not resolve clip path: " + clipPath);
        }

        // Kick async load(s) and return DeferredMarker so the main loop can
        // service the loader between IPC poll cycles.
        struct SpawnState
        {
            GUID modelGuid;
            GUID clipGuid;
            std::string rootName;
            bool hasPosition = false;
            float px = 0, py = 0, pz = 0;
            bool loop = true, paused = false;
            float speed = 1.0f;
            bool kicked = false;
            int polls = 0;
        };
        auto state = std::make_shared<SpawnState>();
        state->modelGuid = modelGuid;
        state->clipGuid = clipGuid;
        state->rootName = ctx.params.value("name", std::filesystem::path(modelPath).stem().string());
        if (ctx.params.contains("position") && ctx.params["position"].is_object())
        {
            const auto& p = ctx.params["position"];
            state->hasPosition = true;
            state->px = p.value("x", 0.0f);
            state->py = p.value("y", 0.0f);
            state->pz = p.value("z", 0.0f);
        }
        state->loop   = ctx.params.value("loop", true);
        state->paused = ctx.params.value("paused", false);
        state->speed  = ctx.params.value("speed", 1.0f);

        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto& am2 = EngineCore::GetInstance().GetAssetManager();
                if (!state->kicked)
                {
                    if (!am2.GetAsset(state->modelGuid))
                        (void)am2.LoadAssetAsync(state->modelGuid, AssetLoadPriority::High);
                    state->kicked = true;
                }

                SharedPtr<Asset> modelAsset = am2.GetAsset(state->modelGuid);
                if (!modelAsset)
                {
                    // Cap waiting at ~10 seconds (600 polls @ 60fps).
                    if (++state->polls > 600)
                    {
                        outResult = Editor::RefuseRequest("ModelAsset load timed out");
                        return true;
                    }
                    return false; // still loading; poll next frame
                }

                auto* modelPtr = dynamic_cast<ModelAsset*>(modelAsset.get());
                if (!modelPtr || !modelPtr->IsLoaded())
                {
                    outResult = Editor::RefuseRequest("Failed to load ModelAsset");
                    return true;
                }

                auto* world2 = EngineCore::GetInstance().GetPrimaryWorld();
                auto* rs = EngineCore::GetInstance().GetRenderServices();
                if (!world2 || !rs)
                {
                    outResult = Editor::RefuseRequest("World or RenderServices unavailable");
                    return true;
                }

                auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
                    *rs, *world2, *modelPtr, state->modelGuid, state->rootName, Editor::GetFbxModelEntityFactoryOptions());
                if (!created.IsValid())
                {
                    outResult = Editor::RefuseRequest("ModelEntityFactory failed");
                    return true;
                }

                if (state->hasPosition)
                {
                    if (auto* xf = world2->GetComponent<Components::Transform>(created.rootEntity))
                    {
                        Components::Transform updated = *xf;
                        updated.matrix[12] = state->px;
                        updated.matrix[13] = state->py;
                        updated.matrix[14] = state->pz;
                        world2->AddComponentImmediate(created.rootEntity, updated);
                    }
                }

                uint32_t clipIndex = 0;
                if (!state->clipGuid.IsNull())
                {
                    clipIndex = Engine::Renderer::ClipStore::Instance().GetOrLoadClipIndex(state->clipGuid, am2);
                    if (clipIndex != 0)
                    {
                        for (auto e : created.submeshEntities)
                        {
                            Components::AnimatorRef anim{};
                            anim.ClipIndex = clipIndex;
                            anim.Time = 0.0f;
                            anim.Speed = state->speed;
                            anim.Flags = (state->loop ? Components::AnimatorRef::kFlag_Loop : 0u)
                                       | (state->paused ? Components::AnimatorRef::kFlag_Paused : 0u);
                            world2->AddComponentImmediate(e, anim);
                        }
                    }
                }

                if (app.m_ChangeNotifications)
                {
                    Editor::EditorChangeNotifications::WorldStructureChangedEvent ev{};
                    ev.world = world2;
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
                    app.m_ChangeNotifications->NotifyWorldStructureChanged(ev);
                }

                json submeshIds = json::array();
                for (auto e : created.submeshEntities)
                    submeshIds.push_back(e.id);
                outResult = json{
                    {"rootEntityId", created.rootEntity.id},
                    {"submeshEntityIds", std::move(submeshIds)},
                    {"skinned", created.skinned},
                    {"clipIndex", clipIndex},
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // 8c. spawn_humanoid_test_pair — synchronous, main-thread spawn of a Model
    // entity (with optional AnimationClip wired into AnimatorRef on every
    // skinned submesh). Bypasses the deferred-response path of `spawn_model`
    // which times out before first-time FBX import completes. Loads assets
    // synchronously via LoadAssetAsync(...).get() (same pattern used by
    // ModelRenderSetup / RenderServices). Intended for autonomous test
    // scaffolding (humanoid retargeting scenarios).
    server.RegisterHandler("spawn_humanoid_test_pair", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices available");

        const std::string modelPath = ctx.params.value("modelPath", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'modelPath' (e.g. 'Models/BusinessMale.fbx')");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve modelPath: " + modelPath);

        // Synchronous load on the main thread. Same pattern used elsewhere in
        // the engine (ModelRenderSetup.cpp:30, RenderServices.cpp:314).
        SharedPtr<Asset> modelAsset = am.GetAsset(modelGuid);
        if (!modelAsset)
            modelAsset = am.LoadAssetAsync(modelGuid, AssetLoadPriority::High).get();
        if (!modelAsset)
            return Editor::RefuseRequest("Failed to load model asset");

        auto* modelPtr = dynamic_cast<ModelAsset*>(modelAsset.get());
        if (!modelPtr || !modelPtr->IsLoaded())
            return Editor::RefuseRequest("Asset is not a loaded ModelAsset");

        const std::string clipPath = ctx.params.value("clipPath", std::string{});
        GUID clipGuid;
        if (!clipPath.empty())
        {
            clipGuid = am.ResolveAssetGuid(clipPath);
            if (clipGuid.IsNull())
                return Editor::RefuseRequest("Could not resolve clipPath: " + clipPath);
            // Force the clip asset to fully import as well — required so
            // ClipStore::GetOrLoadClipIndex() finds non-zero clip data on the
            // first call without polling.
            SharedPtr<Asset> clipAsset = am.GetAsset(clipGuid);
            if (!clipAsset)
                clipAsset = am.LoadAssetAsync(clipGuid, AssetLoadPriority::High).get();
            if (!clipAsset)
                return Editor::RefuseRequest("Failed to load clip asset");

            // If clipPath points to a Model file (e.g. an FBX with embedded
            // animations), normalize to the first embedded clip's derived
            // GUID — same path the Animator inspector takes via
            // NormalizeClipSelection.
            if (clipAsset->GetType() == AssetType::Model)
            {
                auto* clipModel = dynamic_cast<ModelAsset*>(clipAsset.get());
                if (clipModel && !clipModel->GetEmbeddedClipGuids().empty())
                    clipGuid = clipModel->GetEmbeddedClipGuids().front();
                else
                    return Editor::RefuseRequest("clipPath points to a Model with no embedded clips: " + clipPath);
            }
        }

        const std::string rootName = ctx.params.value("name",
            std::filesystem::path(modelPath).stem().string());

        auto created = Engine::Renderer::ModelEntityFactory::CreateFromModel(
            *rs, *world, *modelPtr, modelGuid, rootName, Editor::GetFbxModelEntityFactoryOptions());
        if (!created.IsValid())
            return Editor::RefuseRequest("ModelEntityFactory::CreateFromModel failed");

        // Apply optional position to the root entity's transform.
        if (ctx.params.contains("position") && ctx.params["position"].is_object())
        {
            const auto& p = ctx.params["position"];
            const float px = p.value("x", 0.0f);
            const float py = p.value("y", 0.0f);
            const float pz = p.value("z", 0.0f);
            if (auto* xf = world->GetComponent<Components::Transform>(created.rootEntity))
            {
                Components::Transform updated = *xf;
                updated.matrix[12] = px;
                updated.matrix[13] = py;
                updated.matrix[14] = pz;
                world->AddComponentImmediate(created.rootEntity, updated);
            }
        }

        // Wire AnimatorRef on every skinned submesh entity. ModelEntityFactory
        // already adds a default AnimatorRef (no clip); we replace it with the
        // requested clip index here.
        //
        // Also set Animator.clipGuid on the root entity so play-mode entry
        // doesn't wipe the clip selection. The runtime resolves clipGuid →
        // ClipStore index on play; without it, AnimatorRef.ClipIndex resets
        // to 0 when the editor swaps to play world.
        uint32_t clipIndex = 0;
        if (!clipGuid.IsNull())
        {
            clipIndex = Engine::Renderer::ClipStore::Instance().GetOrLoadClipIndex(clipGuid, am);
            if (clipIndex != 0)
            {
                const bool loop   = ctx.params.value("loop", true);
                const bool paused = ctx.params.value("paused", false);
                const float speed = ctx.params.value("speed", 1.0f);
                for (auto e : created.submeshEntities)
                {
                    Components::AnimatorRef anim{};
                    anim.ClipIndex = clipIndex;
                    anim.Time = 0.0f;
                    anim.Speed = speed;
                    anim.Flags = (loop ? Components::AnimatorRef::kFlag_Loop : 0u)
                               | (paused ? Components::AnimatorRef::kFlag_Paused : 0u);
                    world->AddComponentImmediate(e, anim);
                }

                // Stamp the Animator.clipGuid on whichever entity currently
                // owns the Animator component (root for multi-submesh, or the
                // sole submesh for single-submesh models).
                ECS::EntityHandle animatorOwner = created.rootEntity;
                if (auto* existing = world->GetComponent<Components::Animator>(animatorOwner))
                {
                    Components::Animator updated = *existing;
                    updated.SelectClip(clipGuid);
                    world->AddComponentImmediate(animatorOwner, updated);
                }
                else
                {
                    for (auto e : created.submeshEntities)
                    {
                        if (auto* ex = world->GetComponent<Components::Animator>(e))
                        {
                            Components::Animator updated = *ex;
                            updated.SelectClip(clipGuid);
                            world->AddComponentImmediate(e, updated);
                            break;
                        }
                    }
                }
            }
        }

        if (app.m_ChangeNotifications)
        {
            Editor::EditorChangeNotifications::WorldStructureChangedEvent ev{};
            ev.world = world;
            ev.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            app.m_ChangeNotifications->NotifyWorldStructureChanged(ev);
        }

        json submeshIds = json::array();
        for (auto e : created.submeshEntities)
            submeshIds.push_back(e.id);

        return json{
            {"rootEntityId", created.rootEntity.id},
            {"submeshEntityIds", std::move(submeshIds)},
            {"skinned", created.skinned},
            {"clipIndex", clipIndex},
        }; });

    // 8d. export_model_materials — invoke Editor::ExportModelMaterials on an
    // already-spawned model. Test scaffolding for the provenance integration
    // test. The drag-drop UI path
    // in SceneViewPanel / HierarchyPanel is what normally calls
    // ExportModelMaterials, but those paths require UI events to fire — this
    // RPC lets a test harness run the same flow over IPC.
    //
    // Caller is expected to have spawned the model first (e.g. via
    // spawn_humanoid_test_pair) and passes the resulting submesh entity IDs
    // plus the model's asset-relative path.
    server.RegisterHandler("export_model_materials", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        (void)app;
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const std::string modelPath = ctx.params.value("modelPath", std::string{});
        if (modelPath.empty())
            return Editor::RefuseRequest("Missing 'modelPath'");

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const GUID modelGuid = am.ResolveAssetGuid(modelPath);
        if (modelGuid.IsNull())
            return Editor::RefuseRequest("Could not resolve modelPath: " + modelPath);

        const std::filesystem::path absModelPath = am.ResolveAssetPath(modelPath);

        std::vector<ECS::EntityHandle> entities;
        if (ctx.params.contains("entityIds") && ctx.params["entityIds"].is_array())
        {
            for (const auto& e : ctx.params["entityIds"])
            {
                if (e.is_number_unsigned())
                {
                    ECS::EntityHandle h;
                    h.id = e.get<uint32_t>();
                    entities.push_back(h);
                }
            }
        }
        if (entities.empty())
            return Editor::RefuseRequest("Missing or empty 'entityIds'");

        Editor::ExportModelMaterials(absModelPath, modelGuid, entities, *world, am);

        return json{
            {"ok", true},
            {"modelPath", modelPath},
            {"modelGuid", modelGuid.ToString()},
            {"entityCount", entities.size()},
        }; });

    // 8e. enumerate_provenance — read all rows from the v6 provenance table
    // on the project source's SQLite cache. Test scaffolding for the
    // provenance integration test; lets the harness verify the importer
    // call sites wrote the expected rows.
    server.RegisterHandler("enumerate_provenance", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        (void)ctx;
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();

        json rows = json::array();
        if (const auto* projectSource = registry.ProjectSourcePinned().get())
        {
            if (auto* cache = projectSource->Cache.get())
            {
                for (const auto& row : cache->EnumerateAllProvenance())
                {
                    rows.push_back(json{
                        {"produced", row.Produced.ToString()},
                        {"producer", row.Producer.ToString()},
                        {"importerId", row.ImporterId},
                    });
                }
            }
        }
        return json{{"count", rows.size()}, {"rows", std::move(rows)}}; });

    // 9. set_component
    server.RegisterHandler("set_component", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId") || !ctx.params.contains("component"))
            return Editor::RefuseRequest("Missing entityId or component parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        std::string componentName = ctx.params["component"].get<std::string>();

        // Accept values as an object or default to empty object (add component with defaults).
        json values = json::object();
        if (ctx.params.contains("values"))
        {
            auto& v = ctx.params["values"];
            if (v.is_object())
                values = v;
            else if (v.is_string())
            {
                // LLMs sometimes stringify the JSON object — parse it.
                auto parsed = json::parse(v.get<std::string>(), nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                    return Editor::RefuseRequest("values must be a JSON object (or a JSON string encoding one)");
                values = std::move(parsed);
            }
        }

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // A port edit is one undo step (CommitComponentWrite): the components the write added
        // or changed (a collider shape brings its PhysicsCollider) and a rewritten spline's points.
        const ComponentApplyResult applied = CommitComponentWrite(app.m_UndoRedo.get(), app.m_ChangeNotifications.get(),
                                                                  world, entity, componentName, values);
        if (!applied.Error.is_null())
            return applied.Error;

        // Notify like the editor's own edit paths so event-driven UI (inspector
        // fields, tool availability gates) reacts to debug-driven writes.
        // UndoRedo kind = "values changed outside the inspector UI; rebuild"
        // (the drop-command precedent). Data-only fast paths (Parent) opt out
        // via the apply result — the policy lives in ApplyComponentValues next
        // to the branch that implements it.
        if (app.m_ChangeNotifications && !applied.SuppressNotify)
        {
            Editor::EditorChangeNotifications::ComponentChangedEvent e{};
            e.world = world;
            e.entity = entity;
            e.componentType = ResolveComponentTypeIdByName(componentName);
            e.kind = Editor::EditorChangeNotifications::ChangeKind::UndoRedo;
            app.m_ChangeNotifications->NotifyComponentChanged(e);
        }

        return json{{"ok", true}}; });

    // set_entity_enabled — switch an entity on or off exactly like the Hierarchy panel's icon:
    // the entity's own state (Editor::CommitEntityEnabledToggle, one undo step), announced as a
    // world-structure change. Its descendants keep their own state and follow through the
    // hierarchy pass on the next update. {"entityId": id, "enabled": bool}.
    server.RegisterHandler("set_entity_enabled", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        const uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        const bool enabled = ctx.params.value("enabled", true);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const ECS::EntityHandle entity(entityId);
        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        const ECS::EntityHandle entities[] = {entity};
        Editor::CommitEntityEnabledToggle(*world, app.m_UndoRedo.get(), app.m_ChangeNotifications.get(), entities,
                                          enabled);
        return json{{"ok", true}, {"enabled", enabled}}; });

    // 10. delete_entity — routes through the editor undo system exactly like the
    // Hierarchy / Scene-View delete key (DeleteEntitiesCommand over the collected
    // subtree, preserve-handle). This lands on the undo stack so a subsequent
    // `undo` / `redo` reproduces the real user path (needed for the camera-delete
    // → undo repro; a plain DestroyEntityImmediate could not be undone).
    server.RegisterHandler("delete_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        if (!world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        std::vector<ECS::EntityHandle> ents =
            Editor::DeleteEntitiesCommand::CollectSubtree(*world, entity);
        if (ents.empty())
            return Editor::RefuseRequest("Entity resolved to an empty subtree");

        const uint64_t deletedCount = static_cast<uint64_t>(ents.size());
        const std::string undoName = (ents.size() > 1) ? "Delete Entities" : "Delete Entity";

        if (app.m_UndoRedo)
        {
            app.m_UndoRedo->Execute(std::make_unique<Editor::DeleteEntitiesCommand>(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents)));
        }
        else
        {
            Editor::DeleteEntitiesCommand cmd(
                undoName, world, app.m_ChangeNotifications.get(), std::move(ents));
            cmd.Redo();
        }

        return json{{"ok", true},
                    {"deletedCount", deletedCount},
                    {"undoTracked", app.m_UndoRedo != nullptr},
                    {"undoName", undoName},
                    {"canUndo", app.CanUndo()}}; });

    // 10b. undo / redo — drive the real editor undo stack, identical to Ctrl+Z /
    // Ctrl+Y and the toolbar buttons (EditorApplication::Undo/Redo → UndoRedoService).
    // Runs on the main thread via FlushPendingRequests (same phase as keyboard input).
    server.RegisterHandler("undo", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        const bool could = app.CanUndo();
        const char* nameBefore = app.GetUndoActionName();
        const std::string undoneName = nameBefore ? std::string(nameBefore) : std::string();

        app.Undo();

        const char* nu = app.GetUndoActionName();
        const char* nr = app.GetRedoActionName();
        return json{{"ok", true},
                    {"undone", could},
                    {"undoneName", undoneName},
                    {"canUndo", app.CanUndo()},
                    {"canRedo", app.CanRedo()},
                    {"nextUndo", nu ? std::string(nu) : std::string()},
                    {"nextRedo", nr ? std::string(nr) : std::string()}}; });

    server.RegisterHandler("redo", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        const bool could = app.CanRedo();
        const char* nameBefore = app.GetRedoActionName();
        const std::string redoneName = nameBefore ? std::string(nameBefore) : std::string();

        app.Redo();

        const char* nu = app.GetUndoActionName();
        const char* nr = app.GetRedoActionName();
        return json{{"ok", true},
                    {"redone", could},
                    {"redoneName", redoneName},
                    {"canUndo", app.CanUndo()},
                    {"canRedo", app.CanRedo()},
                    {"nextUndo", nu ? std::string(nu) : std::string()},
                    {"nextRedo", nr ? std::string(nr) : std::string()}}; });

    // 10c. get_undo_stack — dump the undo/redo stacks for inspection. Each entry
    // reports its command type (DeleteEntitiesCommand vs WorldSnapshotCommand vs …),
    // a human label, and (undo side) a wall-clock timestamp. `cursor` is the number
    // of applied commands (the boundary between the undo and redo stacks).
    server.RegisterHandler("get_undo_stack", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* svc = app.m_UndoRedo.get();
        if (!svc)
            return Editor::RefuseRequest("No undo service");

        const std::size_t undoCount = svc->GetUndoCount();
        const std::size_t redoCount = svc->GetRedoCount();

        auto epochMs = [](std::chrono::system_clock::time_point tp) -> int64_t
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
        };

        // Undo: index 0 = oldest, last = next-to-undo (top of stack).
        json undo = json::array();
        for (std::size_t i = 0; i < undoCount; ++i)
        {
            const char* type = svc->GetUndoTypeNameAt(i);
            const char* label = svc->GetUndoNameAt(i);
            undo.push_back({{"index", static_cast<uint64_t>(i)},
                            {"type", type ? std::string(type) : std::string()},
                            {"label", label ? std::string(label) : std::string()},
                            {"timestampMs", epochMs(svc->GetUndoTimestampAt(i))},
                            {"isNextUndo", i + 1 == undoCount}});
        }

        // Redo: index 0 = next-to-redo (most recently undone).
        json redo = json::array();
        for (std::size_t i = 0; i < redoCount; ++i)
        {
            const char* type = svc->GetRedoTypeNameAt(i);
            const char* label = svc->GetRedoNameAt(i);
            redo.push_back({{"index", static_cast<uint64_t>(i)},
                            {"type", type ? std::string(type) : std::string()},
                            {"label", label ? std::string(label) : std::string()},
                            {"isNextRedo", i == 0}});
        }

        const char* nu = svc->PeekUndoName();
        const char* nr = svc->PeekRedoName();
        json result;
        result["undoCount"] = static_cast<uint64_t>(undoCount);
        result["redoCount"] = static_cast<uint64_t>(redoCount);
        result["cursor"] = static_cast<uint64_t>(undoCount);
        result["canUndo"] = svc->CanUndo();
        result["canRedo"] = svc->CanRedo();
        result["nextUndo"] = nu ? std::string(nu) : std::string();
        result["nextRedo"] = nr ? std::string(nr) : std::string();
        result["undo"] = std::move(undo);
        result["redo"] = std::move(redo);
        return result; });

    // 11. select_entity
    server.RegisterHandler("select_entity", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter");

        uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
        ECS::EntityHandle entity(entityId);

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world || !world->IsValid(entity))
            return Editor::RefuseRequest("Invalid entity");

        // Route through SceneViewController which updates gizmos + Inspector.
        if (!app.m_Windows.empty() && app.m_Windows[0]->scene)
        {
            app.m_Windows[0]->scene->OnEntityPicked(entity);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        // Fallback: update the InspectorPanel directly.
        auto* inspector = FindPanel<InspectorPanel>(app.m_PanelStorage);
        if (inspector)
        {
            inspector->ShowEntity(world, entity, /*force=*/false, /*keepMultiSelection=*/false);
            return json{{"ok", true}, {"entityId", entityId}};
        }

        return Editor::RefuseRequest("No scene view or inspector available"); });

    // 12. get_validation_stats — exact per-VUID validation-layer telemetry from the
    // device's ValidationStatsStore. Immune to log-ring eviction and log thinning.
    // Agent workflow: {reset:true} -> run phase -> read -> assert delta == 0.
    server.RegisterHandler("get_validation_stats", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* device = rs ? rs->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No rendering device");
        const bool reset = ctx.params.value("reset", false);
        const auto stats = device->GetValidationStats();
        if (reset)
            device->ResetValidationStats();
        json vuids = json::array();
        for (const auto& v : stats.Vuids)
        {
            vuids.push_back({{"id", v.Vuid},
                             {"severity", v.IsError ? "error" : "warning"},
                             {"count", v.Count},
                             {"suppressedCount", v.SuppressedCount},
                             {"firstFrame", v.FirstFrame},
                             {"lastFrame", v.LastFrame},
                             {"firstMessage", v.FirstMessage},
                             {"objects", v.FirstObjects},
                             {"labels", v.FirstLabels}});
        }
        return json{{"enabled", stats.Enabled},
                    {"frameSerial", stats.FrameSerial},
                    {"errors", stats.ErrorCount},
                    {"warnings", stats.WarningCount},
                    {"suppressed", stats.SuppressedCount},
                    {"overflow", stats.OverflowCount},
                    {"vuids", std::move(vuids)}}; });

    // 12b. get_render_stats
    server.RegisterHandler("get_render_stats", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        json result;
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        result["entityCount"] = world ? static_cast<uint64_t>(world->GetEntityCount()) : 0;
        if (world)
        {
            // Change-signaling telemetry (P1.5 IdleEditorStampCount gate):
            // world-wide column write-grant stamps + the grant counter. Diff
            // across frames - an idle editor frame stamp delta is ~0.
            result["ecsColumnStamps"] = world->GetColumnStampCount();
            result["ecsGlobalSystemVersion"] = world->GetGlobalSystemVersion();
        }
        // App-loop timing straight from Application — independent of the
        // DebugMetrics ring, and frameCount lets callers diff exact frames.
        result["frameCount"] = app.GetFrameCount();
        result["fps"] = app.GetFps();
        result["frameMsMean"] = app.GetFrameMsMean();
        result["frameMsWorst"] = app.GetFrameMsWorst();

        // Scene-close lifetime counters. Poll once per scene open (allowing the
        // deferred-destroy quarantine to drain first — the texture destroy is
        // deferred by frames-in-flight, so an immediate sample cannot tell a
        // pending retire from a leak). slotCount is the load-bearing one: the
        // slot tables never shrink, so a table growing per open is the leak
        // whatever the active counts say.
        {
            json terrain;
            if (auto* svc = TerrainECS::TerrainService::TryGet())
            {
                terrain["activeCount"] = svc->GetActiveTerrainCount();
                terrain["activeTiledCount"] = svc->GetActiveTiledTerrainCount();
                terrain["slotCount"] = svc->GetTerrainSlotCount();
                terrain["tiledSlotCount"] = svc->GetTiledTerrainSlotCount();
            }
            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                if (auto* feature = rs->GetFeature<TerrainECS::TerrainRenderFeature>())
                    terrain["textureRetireGeneration"] =
                        feature->GetTerrainTextureRetireGeneration();
            }
            if (!terrain.empty())
                result["terrain"] = terrain;

            if (auto* pw = PhysicsECS::PhysicsWorldService::TryGet())
                result["physics"] = {{"bodyCount", pw->GetBodyCount()}};
            if (auto* splines = SplineECS::SplineService::TryGet())
                result["splines"] = {{"activeCount", splines->GetActiveSplineCount()}};
        }

        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            json renderServices;
            renderServices["meshEntryCount"] = rs->GetMeshGPURegistry().GetEntryCount();
            renderServices["drawStreamSlots"] =
                rs->GetDrawStreamBuilder() ? static_cast<uint64_t>(rs->GetDrawStreamBuilder()->GetRangeCount()) : 0ull;
            // Non-skipped MaterialParams repacks; flat while idle = the
            // PackMaterialSSBO skip gate is engaging.
            renderServices["materialSSBOPackCount"] = rs->Materials().GetMaterialSSBOPackCount();
            // Frames whose material rows overflowed the MaterialParams ring and
            // shaded from the zero-filled fallback. Non-zero means the scene has
            // outgrown the ring's starting capacity; still climbing after a few
            // frames means it has outgrown the grow cap.
            renderServices["materialParamsOverflowCount"] =
                rs->Materials().GetMaterialParamsOverflowCount();

            // Mesh residency gate. The `*Total` numbers are monotonic for the
            // process and are what a steady-state claim is checked against:
            // `routeBRefusedNonResidentTotal` and `routeASkippedNonResidentTotal`
            // must read 0. The unsuffixed siblings describe the CURRENT frame
            // (Route B, cleared by BeginFrame) and the CURRENT pool-group plan
            // (Route A, rebuilt by every Refresh), so a zero there means only
            // that nothing was refused in the sample window.
            // `routeBRefusedBucketMissing` is a designed state (tombstones,
            // failed allocations, meshes with no built geometry) and is
            // reported without being a defect signal. `routeARefreshes` scopes
            // Route A: it stays 0 with draw consolidation off, where Route A
            // does not run and its zeros carry no information.
            // `windowExhausted` cannot become non-zero while the upload bracket
            // is a stamp's only obligation — see MeshGPUResidencyStats.
            {
                const auto residency = rs->GetMeshGPURegistry().GetResidencyStats();
                const auto poolGroups = rs->GetMeshPoolGroupResidencyStats();
                renderServices["meshResidency"] = {
                    {"routeBRefusedNonResident", residency.RouteBRefusedNonResident},
                    {"routeBRefusedNonResidentTotal", residency.RouteBRefusedNonResidentTotal},
                    {"routeBRefusedBucketMissing", residency.RouteBRefusedBucketMissing},
                    {"routeBRefusedBucketMissingTotal", residency.RouteBRefusedBucketMissingTotal},
                    {"routeASkippedNonResident", poolGroups.SkippedNonResident},
                    {"routeASkippedNonResidentTotal", poolGroups.SkippedNonResidentTotal},
                    {"routeARefreshes", poolGroups.Refreshes},
                    {"windowExhausted", residency.WindowExhausted}
                };
            }

            // Per-pool suballocator occupancy, one entry per pool buffer.
            // `fragRatio` is (capacity - used - largestFreeRange) / capacity:
            // 0.0 is perfectly packed, and a high value with plenty of free
            // bytes means live ranges are splitting the free space rather than
            // the pool being full. That matters because a best-fit failure does
            // not fail the upload — it appends a sibling pool at double the
            // capacity, which grows GPU memory and adds a pool group (and so an
            // indirect draw). `freeRangeCount` is the same story per frame:
            // morph's free-then-reallocate cannot reuse a freed range for
            // kRetireDelay frames, so a climbing count is that churn showing up.
            {
                json meshPools = json::array();
                for (const auto& pool : rs->GetMeshGPURegistry().GetPoolStats())
                {
                    meshPools.push_back({
                        {"bucketKey", static_cast<uint32_t>(pool.BucketKey)},
                        {"stream", pool.Stream},
                        {"poolIndex", pool.PoolIndex},
                        {"capacity", pool.Allocator.capacity},
                        {"used", pool.Allocator.used},
                        {"peak", pool.Allocator.peak},
                        {"largestFreeRange", pool.Allocator.largestFreeRange},
                        {"freeRangeCount", pool.Allocator.freeRangeCount},
                        {"deferredCount", pool.Allocator.deferredCount},
                        {"fragRatio", pool.Allocator.fragRatio}
                    });
                }
                renderServices["meshPools"] = std::move(meshPools);
            }

            // A2.1: RenderExtractionSystem sub-phase wall-clock timing (last
            // frame + short windowed mean/max) plus record/rebuild counters.
            {
                const auto& ex = rs->GetRenderExtractionStats();
                renderServices["extraction"] = {
                    {"totalMs", ex.TotalMs},
                    {"lightsMs", ex.LightsMs},
                    {"gatherMs", ex.GatherMs},
                    {"processMs", ex.ProcessMs},
                    // A2.1 parallel lane sub-splits (0 in the serial OFF lane).
                    {"prepareMs", ex.PrepareMs},
                    {"applyMs", ex.ApplyMs},
                    {"submitMs", ex.SubmitMs},
                    {"totalMsMean", ex.TotalMsMean},
                    {"gatherMsMean", ex.GatherMsMean},
                    {"processMsMean", ex.ProcessMsMean},
                    {"prepareMsMean", ex.PrepareMsMean},
                    {"applyMsMean", ex.ApplyMsMean},
                    {"totalMsMax", ex.TotalMsMax},
                    {"recordCount", ex.RecordCount},
                    {"submissionCount", ex.SubmissionCount},
                    {"rebuildCount", ex.RebuildCount},
                    {"skippedCount", ex.SkippedCount},
                    // GE_EXTRACTION_FEED lane telemetry (fusion S2b): the
                    // soak/bench instrument — fast-frame ratio, per-trigger
                    // escalation bits (E9 split out for OnDemand view churn),
                    // per-shape pending breakdown (a permanent full-lane pin
                    // is attributable in one poll), and the S0 composition
                    // counts (residual ~= CLEANUP only holds when these are
                    // zero).
                    {"fastFrame", ex.FastFrame},
                    {"fastFramesInWindow", ex.FastFramesInWindow},
                    {"windowTicks", ex.WindowTicks},
                    {"escalationReasonBits", ex.EscalationReasonBits},
                    {"feedPatchedCount", ex.FeedPatchedCount},
                    {"subsetRefreshedCount", ex.SubsetRefreshedCount},
                    {"pendingMeshEntry", ex.PendingMeshEntry},
                    {"pendingMaterialLoad", ex.PendingMaterialLoad},
                    {"pendingPipeline", ex.PendingPipeline},
                    {"pendingSentinelIndex", ex.PendingSentinelIndex},
                    {"particleEmitterCount", ex.ParticleEmitterCount},
                    {"liveParticles", ex.LiveParticles},
                    {"particleSimulationMs", ex.ParticleSimulationMs},
                    {"volumeCount", ex.VolumeCount},
                    {"oceanCount", ex.OceanCount}
                };
            }

            // A2 STEP-0: render-thread CPU timeline. Stitches the RenderServices
            // sort/bucketer brackets with the main window's RenderGraph phase +
            // record-floor brackets so one poll = the whole CPU decomposition
            // (alongside the extraction block above). The record-walk bucket is
            // the summed per-pass cpuMs from get_gpu_profiler (profiling-gated).
            {
                const auto& tl = rs->GetRenderTimelineStats();
                json timeline = {
                    {"sortMs", tl.SortMs},
                    {"sortViewCount", tl.SortViewCount},
                    {"sortMsMean", tl.SortMsMean},
                    {"bucketerScheduleMs", tl.BucketerScheduleMs},
                    {"bucketerScheduleMsMean", tl.BucketerScheduleMsMean}
                };
                if (RG::RGFrame* frame = MainRgFrame(app))
                {
                    const RG::RGFrame::FrameStats& fs = frame->Stats();
                    timeline["renderGraph"] = {
                        {"compileMs", fs.CompileMs},
                        {"scheduleMs", fs.ScheduleMs},
                        {"generateBarriersMs", fs.GenerateBarriersMs},
                        {"buildSubmissionPlanMs", fs.BuildSubmissionPlanMs},
                        {"queueSubmitMs", fs.QueueSubmitMs},
                        {"barrierEmitMs", fs.BarrierEmitMs},
                        {"submissionsMade", fs.SubmissionsMade},
                        {"renderPassesBegun", fs.RenderPassesBegun}
                    };
                }
                // Per-ECS-system wave timings (SystemManager performance data;
                // lazily enabled on first read — the parallel wave path times
                // each system on its worker). This is the engineUpdateMs
                // decomposition the A2 P0-D branch decision reads.
                if (auto* loop = EngineCore::GetInstance().GetRenderingLoop())
                {
                    if (auto* sm = loop->GetSystemManager())
                    {
                        sm->SetPerformanceTracking(true);
                        json systemsJson = json::array();
                        for (const auto& perf : sm->GetPerformanceData())
                        {
                            if (perf.UpdateCount == 0)
                                continue;
                            systemsJson.push_back({{"name", perf.Name},
                                                   {"lastMs", perf.LastTime},
                                                   {"avgMs", perf.AverageTime},
                                                   {"maxMs", perf.MaxTime}});
                        }
                        timeline["ecsSystems"] = std::move(systemsJson);
                        // maxMs is the slowest update since tracking began (a scene load's
                        // bake included). resetSystemMaxima clears it after this read, so the
                        // next read's maxMs is the worst single frame in between.
                        if (ctx.params.value("resetSystemMaxima", false))
                            sm->ResetPerformanceMaxima();
                    }
                }
                renderServices["renderThreadCpu"] = std::move(timeline);
            }

            if (auto* gpuScene = rs->GetGPUScene())
            {
                // Cumulative; the bench diffs samples for per-frame upload BW.
                renderServices["instanceUploadBytesTotal"] = gpuScene->GetInstanceUploadBytesTotal();
                renderServices["gpuSceneInstanceCount"] = gpuScene->GetInstanceCount();
                renderServices["gpuSceneLiveInstanceCount"] = gpuScene->GetLiveInstanceCount();
            }
            if (auto* device = rs->GetDevice())
            {
                renderServices["lastGraphicsPipelineFailure"] = device->GetLastGraphicsPipelineFailure();

                // Output-chain facts: the negotiated swapchain and the
                // terminal encode most recently declared for the backbuffer,
                // so a capture/A-B self-documents its output condition instead
                // of relying on log archaeology. swapchainNativeFormat /
                // swapchainNativeColorSpace are the raw VkFormat /
                // VkColorSpaceKHR the WSI negotiation returned (0 before a
                // swapchain exists); backbufferOutEncoding follows the encode
                // shader's arms (1=sRGB, 2=PQ, 3=HLG, 4=passthrough,
                // 5=encoded-input raw requantize, 6=encoded-input D(c) for an
                // _SRGB ROP, 0=not yet declared).
                const Rendering::HdrOutputState hdrState = device->GetHdrOutputState();
                const auto encodeFacts = Rendering::Passes::GetBackbufferEncodeFacts();
                renderServices["output"] = {
                    {"swapchainNativeFormat", hdrState.display.nativeFormat},
                    {"swapchainNativeColorSpace", hdrState.display.nativeColorSpace},
                    {"swapchainTextureFormat", static_cast<int>(device->GetSwapchainTextureFormat())},
                    {"swapchainNeedsManualSRGBEncode", device->SwapchainNeedsManualSRGBEncode()},
                    {"hdrMode", Rendering::HdrOutputModeToString(hdrState.activeMode)},
                    {"backbufferOutEncoding", encodeFacts.OutEncoding},
                    {"backbufferDitherLsb", encodeFacts.DitherLsb}
                };
                // The effective deband gate of the most recent backbuffer encode.
                // On an SDR editor frame that encode is a filter-off transfer
                // (backbufferDitherLsb 0), and the gate reported here is the one
                // the world views used inside their own finalize.
                renderServices["output"]["debandThresholdLsb"] =
                    Rendering::Passes::GetBackbufferOutputDebandThresholdLsb();

                // Deferred-destroy queue depths, for watching a retire path that
                // has stopped retiring: poll across frames and diff. A healthy
                // device plateaus once the scene settles; a monotonic climb while
                // idle means entries are being queued and never freed.
                //
                // Each field is a snapshot sampled under the queue's own lock, not
                // a barrier: the counts are consistent per queue but not with each
                // other, and a submitting thread may push the instant after the
                // read. stagingBuffers retires on the transfer timeline while the
                // other four retire on per-queue destroy tags, so they are reported
                // apart -- a climb in one does not implicate the other.
                const Rendering::IDevice::ResourcePoolStats pools = device->GetResourcePoolStats();
                result["deferred"] = {
                    {"textures", static_cast<uint64_t>(pools.deferredTextures)},
                    {"textureViews", static_cast<uint64_t>(pools.deferredTextureViews)},
                    {"samplers", static_cast<uint64_t>(pools.deferredSamplers)},
                    {"buffers", static_cast<uint64_t>(pools.deferredBuffers)},
                    {"stagingBuffers", static_cast<uint64_t>(pools.deferredStagingBuffers)}
                };

                // Live (created and not yet destroyed) resource counts, the
                // whole-device view the deferred queues cannot give: a resource
                // that is never retired never enters a deferred queue, so a leak
                // is invisible there and shows only here. Sample after the
                // deferred quarantine has drained, or a pending retire reads as
                // a leak.
                result["live"] = {
                    {"textures", static_cast<uint64_t>(pools.liveTextures)},
                    {"textureViews", static_cast<uint64_t>(pools.liveTextureViews)},
                    {"buffers", static_cast<uint64_t>(pools.liveBuffers)}
                };
            }

            if (auto* culling = rs->GetGPUCullingPipeline())
            {
                const char* publishArm = "none";
                switch (culling->LastPublishArm())
                {
                    case Rendering::GPUCullingPipeline::PublishArm::None:
                        publishArm = "none";
                        break;
                    case Rendering::GPUCullingPipeline::PublishArm::RenderGraph:
                        publishArm = "renderGraph";
                        break;
                }

                // Candidate-set layout only: visibility flags live GPU-side
                // (consumed by the scatter) and are never read back to the CPU.
                json rangesJson = json::array();
                const auto& ranges = culling->GetViewVisibilityRanges();
                for (std::size_t i = 0; i < ranges.size(); ++i)
                {
                    rangesJson.push_back({
                        {"viewId", static_cast<uint32_t>(ranges[i].viewId)},
                        {"cascadeIndex", static_cast<uint32_t>(ranges[i].cascadeIndex)},
                        {"slicePhase", static_cast<uint32_t>(ranges[i].slicePhase)},
                        {"visibilityOffset", ranges[i].visibilityOffset},
                        {"visibilityCount", ranges[i].visibilityCount}
                    });
                }

                // Eventually-consistent HZB occlusion counters (zero until a
                // phase-B dispatch runs — i.e. a view opted into HZB culling).
                const auto occ = culling->ReadOcclusionStats();
                renderServices["gpuCulling"] = {
                    {"lastPublishArm", publishArm},
                    {"rangeCount", static_cast<uint64_t>(ranges.size())},
                    {"ranges", std::move(rangesJson)},
                    {"occlusion", {
                        {"frustumPassed", occ.frustumPassed},
                        {"hzbCulled", occ.hzbCulled},
                        {"recovered", occ.recovered},
                        {"prevVisibleNew", occ.prevVisibleNew}
                    }}
                };
            }

            // Resolved (adapted) auto-exposure per metered view: the linear scene-
            // exposure multiplier and the absolute EV100 it represents. Exposure
            // readback is per-view opt-in (Game View reports its auto camera; Scene View seeds Fixed
            // EV100; lens flare ensures it), so the feature is probed, never created,
            // and only views with a landed metered frame appear.
            if (auto* exposureReadback = rs->GetFeature<Engine::Renderer::ExposureReadbackFeature>())
            {
                json meteredViews = json::array();
                for (const auto& view : rs->Views().GetViews())
                {
                    float scale = 0.0f;
                    uint64_t frameIndex = 0;
                    if (exposureReadback->TryResolveAdaptedExposure(view.id, scale, &frameIndex))
                    {
                        auto sample = Editor::DescribeMeteredExposure(scale, frameIndex);
                        sample["viewId"] = static_cast<uint32_t>(view.id);
                        sample["viewName"] = view.debugName ? view.debugName : "";
                        meteredViews.push_back(std::move(sample));
                    }
                }
                renderServices["exposure"] = {
                    {"meteredViews", std::move(meteredViews)}
                };
            }

            if (auto* csm = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>())
            {
                json csmViews = json::array();
                for (const auto& view : rs->Views().GetViews())
                {
                    const auto* fd = csm->GetCachedFrameData(view.id);
                    if (!fd)
                        continue;

                    uint64_t hash = 1469598103934665603ull;
                    auto mixBytes = [&hash](const void* data, std::size_t size)
                    {
                        const auto* bytes = static_cast<const uint8_t*>(data);
                        for (std::size_t i = 0; i < size; ++i)
                        {
                            hash ^= static_cast<uint64_t>(bytes[i]);
                            hash *= 1099511628211ull;
                        }
                    };
                    mixBytes(fd->SplitDistances, sizeof(fd->SplitDistances));
                    mixBytes(fd->OrthoHalfExtent, sizeof(fd->OrthoHalfExtent));
                    mixBytes(fd->DepthSpan, sizeof(fd->DepthSpan));
                    for (uint32_t c = 0; c < fd->NumCascades && c < Engine::Renderer::kMaxShadowCascades; ++c)
                        mixBytes(fd->LightVP[c].Data(), sizeof(float) * 16u);

                    json cascades = json::array();
                    for (uint32_t c = 0; c < fd->NumCascades && c < Engine::Renderer::kMaxShadowCascades; ++c)
                    {
                        const float* m = fd->LightVP[c].Data();
                        cascades.push_back({
                            {"index", c},
                            {"split", fd->SplitDistances[c]},
                            {"orthoHalfExtent", fd->OrthoHalfExtent[c]},
                            {"depthSpan", fd->DepthSpan[c]},
                            {"m00", m[0]},
                            {"m11", m[5]},
                            {"m22", m[10]},
                            {"m30", m[12]},
                            {"m31", m[13]},
                            {"m32", m[14]},
                            // Full matrix: the six scalars above are a partial
                            // view, and for a light whose rotation zeroes a
                            // diagonal element they read as structural zeros —
                            // which makes "the fit did not move" unfalsifiable
                            // from them alone. Shimmer questions need every
                            // element, so emit all 16 in Data() order.
                            {"lightVP", json::array({m[0], m[1], m[2], m[3],
                                                     m[4], m[5], m[6], m[7],
                                                     m[8], m[9], m[10], m[11],
                                                     m[12], m[13], m[14], m[15]})}
                        });
                    }

                    csmViews.push_back({
                        {"viewId", static_cast<uint32_t>(view.id)},
                        {"numCascades", fd->NumCascades},
                        {"hash", hash},
                        {"depthBias", fd->DepthBias},
                        {"normalBias", fd->NormalBias},
                        {"maxShadowDistance", fd->MaxShadowDistance},
                        {"cascades", std::move(cascades)}
                    });
                }
                renderServices["cascadedShadows"] = {
                    {"debugMode", static_cast<int>(csm->GetDebugMode())},
                    {"filterQuality", static_cast<int>(csm->GetFilterQuality())},
                    {"views", std::move(csmViews)}
                };
            }

            // P0 shadow-arc instrumentation: per-slice passed-caster + drawn-
            // triangle counts from the previous frame's scatter (one-frame-stale;
            // valid at steady state). The per-cascade GPU ms lives in the profiler
            // timings (get_gpu_profiler pass "…CascadedShadowMap.View{id}.Cascade{c}")
            // — the harness stitches the two, so it is NOT duplicated here.
            // cascadeIndex 0..3 are directional cascades; 0xFF is the main/color
            // slice; larger values are area/spot/point culling indices.
            if (auto* stream = rs->GetDrawStreamBuilder())
            {
                json slices = json::array();
                for (const auto& s : stream->GetShadowArcStats())
                {
                    slices.push_back({
                        {"viewId", s.viewId},
                        {"cascadeIndex", static_cast<uint32_t>(s.cascadeIndex)},
                        {"phase", static_cast<uint32_t>(s.phase)},
                        {"passedCasters", s.passedCasters},
                        {"drawnTriangles", s.drawnTriangles},
                        // Crossfade tail rows, also included in passedCasters.
                        // Non-zero == a transition was mid-dissolve when this
                        // slice last dispatched; it is the signal the elision
                        // suppression lifts on.
                        {"tailRecords", s.tailRecords},
                        // Dwell-band thrash instrument: instances that changed
                        // LOD this frame. Also SCATTER_STATS-gated, and 0 when
                        // the slice carries no band (set_lod hysteresis == 0).
                        {"lodChanges", s.lodChanges}
                    });
                }
                const auto trip = stream->ReadScatterStats();
                renderServices["shadowArc"] = {
                    {"slices", std::move(slices)},
                    {"tableMisses", trip.tableMisses},
                    {"overflows", trip.overflows},
                    // False when GE_SCATTER_STATS is off (the shipping default):
                    // drawnTriangles is compiled out and reads 0 for every slice,
                    // so a reader must not mistake it for a genuine zero. Tripwires
                    // and passedCasters are unaffected.
                    {"statsEnabled", stream->IntrospectScatter().StatsActive},
                    // M2a fence-gate reject rate: reduceAttempts vs staleRejects.
                    // A staleRejects/reduceAttempts ratio near 1.0 means the
                    // batch-walk early-out almost never has survivor data to act on
                    // (frame pacing keeps the readback in flight) — the M2a slice
                    // has degenerated to a no-op.
                    {"reduceAttempts", stream->GetShadowArcReduceAttempts()},
                    {"staleRejects", stream->GetShadowArcStaleRejects()}
                };
                const auto scatter = stream->IntrospectScatter();
                // Per-view rendered level and phase history. These buffers are
                // created on the device rather than through the render graph, so
                // the graph's resource list cannot show them: this is where a
                // reader sees whether any view is paying for one. Zero views
                // means zero allocation; rotations advance once per view per
                // frame that was actually recorded.
                renderServices["renderedLevelHistory"] = {
                    {"views", scatter.RenderedHistoryViews},
                    {"rotations", scatter.RenderedHistoryRotations}
                };
            }

            json views = json::array();
            for (const auto& view : rs->Views().GetViews())
            {
                const auto entityKeys = rs->GetEntityBatchKeys(view.id);
                const auto forwardCommands = rs->GetForwardCommands(view.id);
                const auto depthCommands =
                    rs->GetDepthCommands(view.id, Engine::Renderer::DepthPassType::Prepass);
                auto* device = rs->GetDevice();
                auto* drawStream = rs->GetDrawStreamBuilder();
                auto* gpuScene = rs->GetGPUScene();
                const auto& meshRegistry = rs->GetMeshGPURegistry();
                const uint64_t gpuInstancesBda =
                    (device && gpuScene && gpuScene->GetInstanceBuffer().IsValid())
                        ? device->GetBufferDeviceAddress(gpuScene->GetInstanceBuffer())
                        : 0ull;

                // Read once per view, never per previewed key: the scatter state
                // is per-builder, and the read must not create the pipeline it
                // reports (IntrospectScatter, not GetOrCreateScatterPipeline).
                const bool drawStreamPipelineReady =
                    drawStream && drawStream->IntrospectScatter().PipelineReady;

                json keyPreview = json::array();
                const std::size_t previewCount = std::min<std::size_t>(entityKeys.size(), 8u);
                // Mirror the color pass exactly: cascade None, phase A, and the
                // same resolved (class, pool group) axes. The published range is
                // keyed on neither raw BatchKey field, so probing with them
                // reports every draw as skipped.
                const std::span<const uint32_t> meshPoolGroups = rs->MeshPoolGroupSpanForDraws();
                for (std::size_t i = 0; i < previewCount; ++i)
                {
                    const auto& key = entityKeys[i];
                    const Engine::Renderer::DrawStreamLookupKey lookup =
                        Engine::Renderer::ResolveDrawStreamLookupKey(
                            key, Engine::Renderer::GPUDrawStreamBuilder::kCascadeIndexNone,
                            Rendering::MaterialDepthClass::MaterialDependent, meshPoolGroups);
                    const auto range = drawStream
                        ? drawStream->FindBatchDrawRange(
                              static_cast<uint32_t>(view.id),
                              Engine::Renderer::GPUDrawStreamBuilder::kCascadeIndexNone,
                              lookup.classKey, lookup.meshKey,
                              Engine::Renderer::GPUDrawStreamBuilder::SlicePhase::A)
                        : Engine::Renderer::GPUDrawStreamBuilder::BatchDrawRange{};
                    const auto* meshEntry = meshRegistry.Find(key.mesh);
                    bool meshDrawable = false;
                    uint32_t indexCount = 0;
                    if (meshEntry)
                    {
                        // The uncounted read: every refusal at the residency
                        // gate's own chokepoint is an EVENT, banked into a total
                        // with no reset, and the gate is accepted on that total
                        // reading zero. A poll previews several keys per view,
                        // so asking there would make the observer the defect.
                        meshDrawable = meshRegistry.IsEntryDrawable(*meshEntry);
                        indexCount = meshEntry->indexCount;
                    }
                    const uint64_t indirectionBda =
                        drawStream ? drawStream->GetSharedIndirectionAddress() : 0ull;

                    // Guard order mirrors the color recorder's: it reports
                    // device-null ahead of draw-range-invalid, and tests both
                    // BDAs only after the range resolved.
                    const char* skipReason = "prebind-ok";
                    if (!key.material)
                        skipReason = "material-null";
                    else if (!meshEntry)
                        skipReason = "mesh-entry-missing";
                    else if (!meshDrawable)
                        skipReason = "mesh-not-drawable";
                    else if (indexCount == 0)
                        skipReason = "index-count-zero";
                    else if (!drawStreamPipelineReady)
                        skipReason = "drawstream-pipeline-invalid";
                    else if (!device)
                        skipReason = "device-null";
                    else if (!range.IsValid())
                        skipReason = "draw-range-invalid";
                    else if (indirectionBda == 0)
                        skipReason = "indirection-bda-zero";
                    else if (gpuInstancesBda == 0)
                        skipReason = "instance-bda-zero";

                    keyPreview.push_back({
                        {"materialIndex", key.materialIndex},
                        {"meshIndex", key.meshIndex},
                        // The axes actually probed: color-class id, and the
                        // pool group when draw consolidation is active.
                        {"drawStreamClassKey", lookup.classKey},
                        {"drawStreamMeshKey", lookup.meshKey},
                        {"hasMaterial", key.material != nullptr},
                        {"meshHandle", static_cast<uint64_t>(key.mesh)},
                        {"skipReason", skipReason},
                        {"meshEntryValid", meshEntry != nullptr},
                        {"meshDrawable", meshDrawable},
                        {"indexCount", indexCount},
                        {"drawStreamPipelineReady", drawStreamPipelineReady},
                        {"rangeValid", range.IsValid()},
                        {"rangeCmdByteOffset", static_cast<uint64_t>(range.even.cmdByteOffset)},
                        {"rangeCountByteOffset", static_cast<uint64_t>(range.even.countByteOffset)},
                        {"rangeMaxDrawCount", range.even.maxDrawCount},
                        {"rangeTailMaxDrawCount", range.evenTail.maxDrawCount},
                        {"indirectionBdaZero", indirectionBda == 0},
                        {"gpuInstancesBdaZero", gpuInstancesBda == 0},
                        {"supportsBufferDeviceAddress",
                         device ? device->GetCapabilities().supportsBufferDeviceAddress : false}
                    });
                }

                views.push_back({
                    {"id", static_cast<uint32_t>(view.id)},
                    {"name", view.debugName ? view.debugName : ""},
                    {"worldId", static_cast<uint64_t>(view.worldId)},
                    {"entityBatchCount", static_cast<uint64_t>(entityKeys.size())},
                    {"forwardCommandCount", static_cast<uint64_t>(forwardCommands.size())},
                    {"cameraPrepassCommandCount", static_cast<uint64_t>(depthCommands.size())},
                    {"keyPreview", std::move(keyPreview)}
                });
            }
            renderServices["views"] = std::move(views);

            json meshDrawDiagnostics = json::array();
            const auto diagSnapshot = rs->GetRecentMeshDrawDiagnostics();
            const std::size_t diagStart =
                diagSnapshot.size() > 40u ? diagSnapshot.size() - 40u : 0u;
            for (std::size_t i = diagStart; i < diagSnapshot.size(); ++i)
            {
                const auto& d = diagSnapshot[i];
                meshDrawDiagnostics.push_back({
                    {"pass", d.pass},
                    {"reason", d.reason},
                    {"viewId", d.viewId},
                    {"materialIndex", d.materialIndex},
                    {"meshIndex", d.meshIndex},
                    {"maxDrawCount", d.maxDrawCount},
                    {"entityKeyCount", static_cast<uint64_t>(d.entityKeyCount)},
                    {"commandCount", static_cast<uint64_t>(d.commandCount)},
                    {"slotCount", static_cast<uint64_t>(d.slotCount)}
                });
            }
            renderServices["meshDrawDiagnostics"] = std::move(meshDrawDiagnostics);
            result["renderServices"] = std::move(renderServices);
        }


        // Per-frame timing from the editor's scene view perf breakdown.
        auto& perf = app.m_LastSceneViewPerf;
        result["timing"] = {
            {"pollMs", perf.pollMs},
            {"inputMs", perf.inputMs},
            {"appUpdateMs", perf.appUpdateMs},
            {"engineUpdateMs", perf.engineUpdateMs},
            {"beginFrameMs", perf.beginFrameMs},
            {"worldViewsMs", perf.worldViewsMs},
            {"thumbnailsMs", perf.thumbnailsMs},
            {"uiRecordMs", perf.uiRecordMs},
            {"terminalPassesMs", perf.terminalPassesMs},
            {"rgCompileMs", perf.rgCompileMs},
            {"rgExecuteMs", perf.rgExecuteMs},
            {"variantCompileMs", perf.variantCompileMs},
            {"prewarmMs", perf.prewarmMs},
            {"presentMs", perf.presentMs}
        };

        // Application's own phase timings (covers the full Render() call,
        // which includes cross-window DnD routing + per-window iteration
        // not captured by the per-window perf sub-phases above). The diff
        // between RenderMs and the render sub-phase sum reveals unmeasured
        // render work; the diff between wallClockDelta and total of all
        // phases reveals frame-loop overhead (profiler/metrics/sleep).
        const auto& phases = app.GetLastFramePhaseTimings();
        result["appPhases"] = {
            {"pollEventsMs", phases.PollEventsMs},
            {"inputMs", phases.InputMs},
            {"appUpdateMs", phases.AppUpdateMs},
            {"engineUpdateMs", phases.EngineUpdateMs},
            {"renderMs", phases.RenderMs},
            {"sleepMs", phases.SleepMs},
            {"renderPreloopMs", app.m_LastRenderPreloopMs}
        };

        // AppUpdate sub-phase breakdown (added with 9af486eb). Lets us
        // attribute the AppUpdateMs column without a CSV capture: preUi
        // (startup+modals+ECS+SceneEditor), debugPanels (metrics+panels),
        // uiWindows (the big one — per-window UIManager::Update loop),
        // tail (post-UI deferred+hotkeys).
        const auto& sub = app.m_LastAppUpdateSubPhases;
        result["appUpdateSubPhases"] = {
            {"preUiMs", sub.preUiMs},
            {"debugPanelsMs", sub.debugPanelsMs},
            {"uiWindowsMs", sub.uiWindowsMs},
            {"tailMs", sub.tailMs}
        };

        double totalMs = perf.pollMs + perf.inputMs + perf.appUpdateMs
                       + perf.engineUpdateMs + perf.beginFrameMs
                       + perf.worldViewsMs + perf.thumbnailsMs + perf.uiRecordMs
                       + perf.terminalPassesMs + perf.rgCompileMs + perf.rgExecuteMs
                       + perf.presentMs;
        result["totalFrameMs"] = totalMs;
        result["estimatedFps"] = totalMs > 0.0 ? 1000.0 / totalMs : 0.0;

        // Slice 2 subtree-skip fast-path counters + UI phase breakdown.
        // Aggregated across ALL windows — the editor has >1 UIManager
        // (scene + floating panels) and uiWindowsMs is the sum, so
        // per-window-[0] data under-reports the dominant stage.
        if (!app.m_Windows.empty())
        {
            uint32_t fastPathHits = 0, slowPathWalks = 0;
            uint32_t mismatches = 0, cumMismatches = 0;
            uint32_t cascadeCalls = 0, cascadeShared = 0, cascadeCallsBuildYoga = 0, cascadeCallsConverge = 0;
            double uiTotalMs = 0.0, schedulerMs = 0.0, buildYogaMs = 0.0, yogaMs = 0.0;
            double postLayoutMs = 0.0, hitTestMs = 0.0, eventDispatchMs = 0.0;
            double geometryMs = 0.0, cleanupMs = 0.0;
            double preSolveDrainMs = 0.0, indicesAndClipsMs = 0.0;
            double solveAndApplyMs = 0.0, focusOrderMs = 0.0;
            uint32_t elementCount = 0;
            bool enabled = false, validatorEnabled = false;
            std::vector<UIManager::UpdateProfileFrame::DirtySourceEntry> dirtySources;
            size_t windowsWithProf = 0;
            for (const auto& wCtx : app.m_Windows)
            {
                if (!wCtx || !wCtx->ui) continue;
                auto* ui = wCtx->ui.get();
                enabled = enabled || ui->IsSubtreeSkipEnabled();
                // Stage 7 step 7 option 2: validator path deleted along with
                // m_Nodes; report disabled / zero mismatches for compatibility.
                (void)validatorEnabled;
                (void)cumMismatches;
                UIManager::UpdateProfileFrame prof{};
                if (!ui->GetLastUpdateProfileFrame(prof)) continue;
                ++windowsWithProf;
                fastPathHits           += prof.SubtreeFastPathHits;
                slowPathWalks          += prof.SubtreeSlowPathWalks;
                cascadeCalls           += prof.CascadeCalls;
                cascadeShared          += prof.CascadeShared;
                cascadeCallsBuildYoga  += prof.CascadeCallsBuildYoga;
                cascadeCallsConverge   += prof.CascadeCallsConverge;
                uiTotalMs              += prof.TotalMs;
                schedulerMs            += prof.SchedulerMs;
                buildYogaMs            += prof.BuildYogaMs;
                yogaMs                 += prof.YogaMs;
                postLayoutMs           += prof.PostLayoutMs;
                hitTestMs              += prof.HitTestMs;
                eventDispatchMs        += prof.EventDispatchMs;
                geometryMs             += prof.GeometryMs;
                cleanupMs              += prof.CleanupMs;
                preSolveDrainMs        += prof.PreSolveDrainMs;
                indicesAndClipsMs      += prof.IndicesAndClipsMs;
                solveAndApplyMs        += prof.SolveAndApplyMs;
                focusOrderMs           += prof.FocusOrderMs;
                elementCount           += prof.ElementCount;
                for (auto& s : prof.DirtySources)
                    dirtySources.push_back(s);
            }
            if (windowsWithProf > 0)
            {
                result["uiSubtreeSkip"] = {
                    {"fastPathHits", fastPathHits},
                    {"slowPathWalks", slowPathWalks},
                    {"validatorMismatches", mismatches},
                    {"cumulativeMismatches", cumMismatches},
                    {"enabled", enabled},
                    {"validatorEnabled", validatorEnabled},
                    {"cascadeCalls", cascadeCalls},
                    {"cascadeShared", cascadeShared},
                    {"cascadeCallsBuildYoga", cascadeCallsBuildYoga},
                    {"cascadeCallsConverge", cascadeCallsConverge},
                    {"windowsWithProf", (uint32_t)windowsWithProf}
                };
                result["uiPhaseMs"] = {
                    {"totalMs", uiTotalMs},
                    {"schedulerMs", schedulerMs},
                    {"buildYogaMs", buildYogaMs},
                    {"yogaMs", yogaMs},
                    {"postLayoutMs", postLayoutMs},
                    {"hitTestMs", hitTestMs},
                    {"eventDispatchMs", eventDispatchMs},
                    {"geometryMs", geometryMs},
                    {"cleanupMs", cleanupMs},
                    {"preSolveDrainMs", preSolveDrainMs},
                    {"indicesAndClipsMs", indicesAndClipsMs},
                    {"solveAndApplyMs", solveAndApplyMs},
                    {"focusOrderMs", focusOrderMs},
                    {"elementCount", elementCount}
                };

                // Peak-over-history frame selected by totalMs across all
                // windows' ring buffers. Lets a polling client observe the
                // worst recent frame even if the last-frame snapshot above
                // is already post-spike.
                const UIManager::UpdateProfileFrame* bestPeak = nullptr;
                const UIManager* bestPeakUi = nullptr;
                for (const auto& wCtx2 : app.m_Windows)
                {
                    if (!wCtx2 || !wCtx2->ui) continue;
                    const auto* ui2 = wCtx2->ui.get();
                    const auto& hist = ui2->GetUpdateProfilingHistory();
                    for (const auto& f : hist)
                    {
                        if (!bestPeak || f.TotalMs > bestPeak->TotalMs)
                        {
                            bestPeak = &f;
                            bestPeakUi = ui2;
                        }
                    }
                }
                if (bestPeak)
                {
                    (void)bestPeakUi;
                    result["uiPeakFrame"] = {
                        {"historySize", (uint32_t)(bestPeakUi ? bestPeakUi->GetUpdateProfilingHistory().size() : 0u)},
                        {"frameIndex", bestPeak->FrameIndex},
                        {"elementCount", bestPeak->ElementCount},
                        {"totalMs", bestPeak->TotalMs},
                        {"schedulerMs", bestPeak->SchedulerMs},
                        {"buildYogaMs", bestPeak->BuildYogaMs},
                        {"cascadeComputeMs", bestPeak->CascadeComputeMs},
                        {"yogaApplyMs", bestPeak->YogaApplyMs},
                        {"yogaMs", bestPeak->YogaMs},
                        {"postLayoutMs", bestPeak->PostLayoutMs},
                        {"hitTestMs", bestPeak->HitTestMs},
                        {"eventDispatchMs", bestPeak->EventDispatchMs},
                        {"geometryMs", bestPeak->GeometryMs},
                        {"cleanupMs", bestPeak->CleanupMs},
                        {"transitionMs", bestPeak->TransitionMs},
                        {"preSolveDrainMs", bestPeak->PreSolveDrainMs},
                        {"indicesAndClipsMs", bestPeak->IndicesAndClipsMs},
                        {"solveAndApplyMs", bestPeak->SolveAndApplyMs},
                        {"focusOrderMs", bestPeak->FocusOrderMs},
                        {"subtreeFastPathHits", bestPeak->SubtreeFastPathHits},
                        {"subtreeSlowPathWalks", bestPeak->SubtreeSlowPathWalks},
                        {"cascadeCalls", bestPeak->CascadeCalls},
                        {"cascadeShared", bestPeak->CascadeShared},
                        {"cascadeCallsBuildYoga", bestPeak->CascadeCallsBuildYoga},
                        {"cascadeCallsConverge", bestPeak->CascadeCallsConverge},
                        {"lateRelayoutCount", bestPeak->LateRelayoutCount},
                        {"lateRelayoutReasons", bestPeak->LateRelayoutReasons},
                        {"layoutOverridePatchCount", bestPeak->LayoutOverridePatchCount}
                    };
                }
                if (!dirtySources.empty())
                {
                    // Sort aggregated list by count descending and keep top 10.
                    std::sort(dirtySources.begin(), dirtySources.end(),
                        [](const auto& a, const auto& b) { return a.Count > b.Count; });
                    const size_t keep = std::min<size_t>(dirtySources.size(), 10);
                    auto arr = nlohmann::json::array();
                    for (size_t i = 0; i < keep; ++i)
                    {
                        const auto& src = dirtySources[i];
                        arr.push_back({
                            {"instanceId", src.InstanceId},
                            {"id", src.Id},
                            {"class", src.ClassName},
                            {"count", src.Count},
                            {"flagsSeen", src.FlagsSeen}
                        });
                    }
                    result["uiDirtySources"] = std::move(arr);
                }
            }
        }

        // Wall-clock FPS from the Application's actual delta time (matches the
        // scene view FPS label). This includes all overhead not covered by
        // the timed phases above (OS scheduling, window messages, etc.).
        const double dt = app.GetDeltaTime();
        const double wallClockDeltaMs = dt * 1000.0;
        result["wallClockDeltaMs"] = wallClockDeltaMs;
        result["wallClockFps"] = dt > 0.0 ? 1.0 / dt : 0.0;

        // Pipeline variant counts. Reveals how many unique pipelines the scene
        // forces — each distinct cache entry maps to its own vkCmdBindPipeline
        // per pass per cascade.
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            result["variantCacheCounts"] = {
                {"colorInstanced", static_cast<uint64_t>(rs->Materials().Variants().InstancedCount())},
                {"depthInstanced", static_cast<uint64_t>(rs->Materials().Variants().DepthInstancedCount())},
                {"sharedDepth", static_cast<uint64_t>(rs->Materials().Variants().SharedDepthCount())},
                // Monotonic count of variant compiles the async publish-gate ran.
                // Unlike the three cache sizes, it moves on an in-place republish
                // (hot-reload replaces an entry without changing the map size), so
                // it is the signal that a shader edit actually landed a new pipeline.
                {"asyncVariantCompiles", rs->Materials().Variants().AsyncVariantCompileCount()}
            };
        }

        // GPU sync timing breakdown from the Vulkan device. Shows where the
        // CPU stalls waiting for the GPU (fence waits, swapchain acquire, present).
        if (!app.m_Windows.empty() && app.m_Windows[0]->renderCtx)
        {
            if (auto* device = app.m_Windows[0]->renderCtx->GetDevice())
            {
                Rendering::IDevice::FrameSyncTimings sync{};
                if (device->GetLastFrameSyncTimings(sync))
                {
                    // A frame whose BeginFrame spends most of its wall time parked on the
                    // swapchain fence is paced by the compositor, not by the engine: an idle
                    // Windows desktop drops DWM to ~4 Hz and every windowed FIFO swapchain
                    // follows it, so wallClockFps then measures DWM and says nothing about
                    // render cost. Derive the verdict here, once, rather than leaving every
                    // caller to re-derive it from the raw ratio (and most never do).
                    const bool measurable = wallClockDeltaMs > 0.0;
                    const bool presentPaced = measurable &&
                                              sync.beginFrameWaitMs >= 0.5 * wallClockDeltaMs;
                    result["gpuSync"] = {
                        {"beginFrameWaitMs", sync.beginFrameWaitMs},
                        {"waitedGraphicsFence", sync.waitedGraphicsFence},
                        {"waitedComputeFence", sync.waitedComputeFence},
                        {"waitedTransferFence", sync.waitedTransferFence},
                        {"acquireMs", sync.acquireMs},
                        {"acquireTimedOut", sync.acquireTimedOut},
                        {"presentTransitionSubmitMs", sync.presentTransitionSubmitMs},
                        {"presentMs", sync.presentMs},
                        {"frameGpuPeriodMs", sync.frameGpuPeriodMs},
                        {"presentPaced", presentPaced}
                    };
                    // No wall-clock delta yet (first frame) means the ratio has no
                    // denominator — unknown, which is not the same as ok.
                    result["measurementValidity"] = !measurable ? "unknown — no wall-clock delta"
                                                    : presentPaced ? "UNMEASURED — display-paced"
                                                                   : "ok";
                }
            }
        }

        return result; });

    // get_ui_clip_stats
    // Live persistent clip-slot usage per UI window (used = currently
    // allocated, total = allocator high-water). Clip slots share a 16-bit
    // index space with the clip SSBO; this tracks pressure from
    // overflow:hidden containers and overflowing-text self-clips.
    server.RegisterHandler("get_ui_clip_stats", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto windowsArr = nlohmann::json::array();
        for (const auto& wCtx : app.m_Windows)
        {
            if (!wCtx || !wCtx->ui) continue;
            windowsArr.push_back({
                {"clipSlotsUsed", wCtx->ui->GetClipSlotsUsed()},
                {"clipSlotsTotal", wCtx->ui->GetClipSlotsTotal()},
            });
        }
        return json{{"windows", windowsArr}}; });

    // get_ui_profile_history
    // Returns the full UpdateProfileFrame ring buffer (last ~120 frames) for
    // each UI window. Intended for offline spike analysis — a polling client
    // can locate the exact mutation frame by scanning for max(totalMs) and
    // inspect its full counter set without relying on burst-sampling tricks.
    //
    // Response:
    //   { windows: [ { frames: [ <UpdateProfileFrame fields>, ... ] } ] }
    //
    // Note: requires GE_UI_UPDATE_PROFILE=1 at launch (no frames otherwise).
    server.RegisterHandler("get_ui_profile_history", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Optional {"maxFrames": N} param to cap response size.
        const int64_t req = ctx.params.value("maxFrames", (int64_t)120);
        size_t maxFrames = (req > 0 && req <= 120) ? (size_t)req : 120;
        nlohmann::json result;
        auto windowsArr = nlohmann::json::array();
        for (const auto& wCtx : app.m_Windows)
        {
            if (!wCtx || !wCtx->ui) continue;
            const auto* ui = wCtx->ui.get();
            const auto& hist = ui->GetUpdateProfilingHistory();
            if (hist.empty()) { continue; }
            auto framesArr = nlohmann::json::array();
            const size_t start = hist.size() > maxFrames ? hist.size() - maxFrames : 0u;
            for (size_t i = start; i < hist.size(); ++i)
            {
                const auto& f = hist[i];
                framesArr.push_back({
                    {"frameIndex", f.FrameIndex},
                    {"elementCount", f.ElementCount},
                    {"totalMs", f.TotalMs},
                    {"idleFrame", f.IdleFrame},
                    {"idleDecline", f.IdleDecline},
                    {"schedulerMs", f.SchedulerMs},
                    {"buildYogaMs", f.BuildYogaMs},
                    {"cascadeComputeMs", f.CascadeComputeMs},
                    {"yogaApplyMs", f.YogaApplyMs},
                    {"yogaMs", f.YogaMs},
                    {"postLayoutMs", f.PostLayoutMs},
                    {"hitTestMs", f.HitTestMs},
                    {"eventDispatchMs", f.EventDispatchMs},
                    {"geometryMs", f.GeometryMs},
                    {"cleanupMs", f.CleanupMs},
                    {"transitionMs", f.TransitionMs},
                    {"preSolveDrainMs", f.PreSolveDrainMs},
                    {"indicesAndClipsMs", f.IndicesAndClipsMs},
                    {"solveAndApplyMs", f.SolveAndApplyMs},
                    {"focusOrderMs", f.FocusOrderMs},
                    {"subtreeFastPathHits", f.SubtreeFastPathHits},
                    {"subtreeSlowPathWalks", f.SubtreeSlowPathWalks},
                    {"cascadeCalls", f.CascadeCalls},
                    {"cascadeShared", f.CascadeShared},
                    {"cascadeCallsBuildYoga", f.CascadeCallsBuildYoga},
                    {"cascadeCallsConverge", f.CascadeCallsConverge},
                    {"lateRelayoutCount", f.LateRelayoutCount},
                    {"lateRelayoutReasons", f.LateRelayoutReasons},
                    {"layoutOverridePatchCount", f.LayoutOverridePatchCount},
                    {"onPostLayoutDispatchMs", f.OnPostLayoutDispatchMs},
                    {"postLayoutRetryMs", f.PostLayoutRetryMs},
                    {"postLayoutRetryCount", f.PostLayoutRetryCount},
                    {"genAllPrimitivesMs", f.GenAllPrimitivesMs},
                    {"genAllPrimitivesSkipped", f.GenAllPrimitivesSkipped},
                    {"genAllPrimitivesRegenCause", f.GenAllPrimitivesRegenCause},
                    {"drainItems", f.DrainItems},
                    {"drainParallelTasks", f.DrainParallelTasks},
                    {"drainEscalated", f.DrainEscalated},
                    {"lateRelayoutBuildYogaMs", f.LateRelayoutBuildYogaMs},
                    {"lateRelayoutYogaMs", f.LateRelayoutYogaMs},
                    {"subtreeSlowPathReasonUnion", f.SubtreeSlowPathReasonUnion},
                    {"subtreeSlowPathReasonCounts", nlohmann::json::array({
                        f.SubtreeSlowPathReasonCounts[0], f.SubtreeSlowPathReasonCounts[1],
                        f.SubtreeSlowPathReasonCounts[2], f.SubtreeSlowPathReasonCounts[3],
                        f.SubtreeSlowPathReasonCounts[4], f.SubtreeSlowPathReasonCounts[5],
                        f.SubtreeSlowPathReasonCounts[6], f.SubtreeSlowPathReasonCounts[7],
                        f.SubtreeSlowPathReasonCounts[8], f.SubtreeSlowPathReasonCounts[9],
                        f.SubtreeSlowPathReasonCounts[10], f.SubtreeSlowPathReasonCounts[11]
                    })}
                });
                // Attach top dirtySources for this frame (only populated when
                // GE_UI_DIRTY_TRACE=1). Lets spike analysis locate the offending
                // elements without a separate call.
                if (!f.DirtySources.empty())
                {
                    auto& frameObj = framesArr.back();
                    auto srcArr = nlohmann::json::array();
                    auto sorted = f.DirtySources;
                    std::sort(sorted.begin(), sorted.end(),
                        [](const auto& a, const auto& b) { return a.Count > b.Count; });
                    const size_t keep = std::min<size_t>(sorted.size(), 10);
                    for (size_t j = 0; j < keep; ++j)
                    {
                        const auto& src = sorted[j];
                        srcArr.push_back({
                            {"instanceId", src.InstanceId},
                            {"id", src.Id},
                            {"class", src.ClassName},
                            {"count", src.Count},
                            {"flagsSeen", src.FlagsSeen}
                        });
                    }
                    frameObj["dirtySources"] = std::move(srcArr);
                }
            }
            windowsArr.push_back({{"frames", std::move(framesArr)}});
        }
        result["windows"] = std::move(windowsArr);
        return result; });

    // 13. take_screenshot
    // Screenshot via async GPU readback.
    //
    // Targets (parameter "target"):
    //   "window"   — full editor window (UI + scene viewport composited). Default.
    //   "viewport" — scene viewport texture only (legacy behavior).
    //   "panel"    — full window readback cropped to the dock panel rect ("panelId").
    //   "element"  — full window readback cropped to a UI element rect ("elementId").
    //   "rect"           — full window readback cropped to a custom rect ("x","y","w","h").
    //   "asset_preview"  — live video preview texture from Asset View (direct GPU readback).
    //
    // For window/panel/element/rect targets we readback the editor's presentation target.
    // asset_preview bypasses the UI composite and reads the video player's GPU texture.
    //
    // The readback is declared into the RenderGraph frame at the right point — after all
    // normal passes are declared but before Execute. We use EnqueueRenderCallback()
    // which fires from the frame driver pre-Execute on the pure RenderGraph path. A deferred
    // poll then checks the ticket's TryGet() each frame until the GPU copy completes.
    server.RegisterHandler("take_screenshot", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        std::string requestId = ctx.id;

        // Parse parameters up-front (before scheduling render callback).
        const std::string target = ctx.params.value("target", std::string("window"));
        const std::string panelId = ctx.params.value("panelId", std::string());
        const std::string elementId = ctx.params.value("elementId", std::string());
        const int32_t rectX = ctx.params.value("x", 0);
        const int32_t rectY = ctx.params.value("y", 0);
        const int32_t rectW = ctx.params.value("w", 0);
        const int32_t rectH = ctx.params.value("h", 0);
        // coords: "logical" (default) treats x/y/w/h as logical UI pixels —
        // matches get_ui_tree / get_panel_tree output and panel/element bounds.
        // "physical" treats them as raw pixel coords on the saved PNG, useful
        // when post-processing a previously-saved screenshot. Only meaningful
        // for target='rect'.
        const std::string coords = ctx.params.value("coords", std::string("logical"));
        // dither: default ON — captures quantize the linear readback with the same
        // TPDF the screen's sRGB encode pass applies, so screenshots match what the
        // eye sees. Harness/golden consumers that diff or hash pixels numerically
        // should pass dither=false for deterministic staircase-quantized output.
        // GE_OUTPUT_DITHER=0 turns the screen's dither off, so it has to turn the
        // mirror's off too — otherwise that A/B silently compares two identically
        // dithered captures. The step stays the capture's own 8-bit LSB (the PNG
        // quantizes at 1/255 in every output mode); only the enable is shared.
        const bool dither =
            ctx.params.value("dither", true) && Rendering::Passes::IsOutputDitherEnabled();
        // deband: default ON — same parity rationale as dither, for the screen's
        // pre-dither gradient deband (also process-gated by GE_DEBAND). Pass
        // deband=false to profile the raw quantization structure of the source.
        const bool deband = ctx.params.value("deband", true);
        // allowWindowCapture: opt in to an OS window capture (PrintWindow) when the
        // render graph cannot hand over the frame. Off by default — a window
        // capture is a picture of what the compositor holds, not the frame the
        // render graph produced, and it is a plausible PNG either way, so serving
        // one unasked turns a rendering failure into evidence the caller trusts.
        // The response states which one it is in `method`.
        const bool allowWindowCapture = ctx.params.value("allowWindowCapture", false);

        // Validate target.
        if (target != "window" && target != "viewport" && target != "panel"
            && target != "element" && target != "rect" && target != "asset_preview")
        {
            return Editor::RefuseRequest("Unknown target '" + target + "'. Use one of: window, viewport, panel, element, rect, asset_preview.");
        }
        if (coords != "logical" && coords != "physical")
            return Editor::RefuseRequest("coords must be 'logical' or 'physical'");
        if (target == "panel" && panelId.empty())
            return Editor::RefuseRequest("target='panel' requires 'panelId'");
        if (target == "element" && elementId.empty())
            return Editor::RefuseRequest("target='element' requires 'elementId'");
        if (target == "rect" && (rectW <= 0 || rectH <= 0))
            return Editor::RefuseRequest("target='rect' requires positive 'w' and 'h'");

        auto cropHolder = std::make_shared<ScreenshotCropDesc>();
        // What the readback pixels hold, stated by whichever declaration arm below
        // schedules the copy. Disengaged means "nobody stated a space" — the
        // conversion refuses rather than guessing from the format, so a new capture
        // target cannot inherit a silent default.
        auto sourceSpaceHolder = std::make_shared<std::optional<UI::UITextureSpace>>();
        // RenderGraph arm (8c-4): ticket readback — declared against the frame driver's
        // published per-frame capture context.
        auto ticketHolder = std::make_shared<std::shared_ptr<Rendering::RGReadbackTicket>>();
        auto assetPreviewPathHolder = std::make_shared<std::string>();
        // Frame the readback was declared on — reported for staleness diagnosis.
        auto frameIndexHolder = std::make_shared<uint64_t>(0);
        // Deband gate + scope snapshotted at readback declaration (right after
        // this frame's encode declared, same render thread) so the deferred
        // conversion mirrors THE frame it read — not whichever window
        // published last while the GPU finished.
        auto encodeFilterStateHolder = std::make_shared<std::optional<CaptureEncodeFilterState>>();

        LOG_DEBUG("Screenshot: request received target={} requestId={}", target, requestId);

        // How long this request keeps asking the render graph for a frame, and
        // what the caller is told when it stops. The attempt below reports into
        // it from the render phase; the poll drives it.
        auto wait = std::make_shared<Editor::ScreenshotCaptureWait>(allowWindowCapture);

        // One render-phase attempt at declaring the readback (pre-Execute on the
        // pure RenderGraph path, where the frame driver publishes the capture
        // context). The poll re-arms it every frame until the wait ends, so a
        // pane that is one frame behind — a scene view catching up on extraction —
        // is a retry rather than a failed capture.
        //
        // At most one attempt is in flight: the poll runs once per tick and the
        // render phase drains the whole callback queue at once, so queuing an
        // attempt per tick would make a starved frame loop spend the whole
        // refusal budget inside a single rendered frame.
        auto attemptQueued = std::make_shared<bool>(false);
        auto attempt = std::make_shared<std::function<void()>>();
        *attempt = [&app, wait, attemptQueued, ticketHolder, cropHolder, sourceSpaceHolder,
                    assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, target,
                    panelId, elementId, rectX, rectY, rectW, rectH, coords]()
        {
            *attemptQueued = false;
            // The poll can end the wait with this attempt still queued, and the
            // request is answered by then: declaring a readback nobody consumes
            // would cost the frame another composite for nothing.
            if (!wait->GiveUpReason().empty())
                return;
            // Every attempt re-declares from scratch: a crop from an earlier
            // frame must never ride along with this frame's pixels.
            *cropHolder = ScreenshotCropDesc{};

            if (app.m_Windows.empty())
            {
                wait->RecordBlocked("the editor has no window");
                return;
            }

            auto* wndCtx = app.m_Windows[0].get();
            if (!wndCtx->renderCtx)
            {
                wait->RecordBlocked("the editor window has no render context");
                return;
            }

            auto* device = wndCtx->renderCtx->GetDevice();
            if (!device)
            {
                wait->RecordBlocked("the editor window has no render device");
                return;
            }

            // 8c-4: RenderGraph capture context, filled by the frame driver right
            // before this callback runs; ids are frame-local and valid only
            // for the published incarnation.
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 =
                rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                wait->RecordRefusal("the frame published no render-graph capture context");
                return;
            }
            *frameIndexHolder = wndCtx->rg2Capture.frameIndex;

            // ---- ASSET PREVIEW path: readback live video frame texture ---------
            if (target == "asset_preview")
            {
                auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage);
                if (!assetViewPanel)
                {
                    wait->RecordBlocked("the Asset View panel is not open");
                    return;
                }

                Rendering::TextureHandle previewTexture{};
                Rendering::TextureFormat previewFormat = Rendering::TextureFormat::RGBA8_UNORM;
                if (!assetViewPanel->TryGetVideoPreviewReadbackTexture(previewTexture, previewFormat))
                {
                    wait->RecordBlocked(
                        "the Asset View has no uploaded video frame — select an .mp4 first");
                    return;
                }

                *assetPreviewPathHolder = assetViewPanel->GetPreviewDebugState().previewPath;
                *ticketHolder = Rendering::RequestDeviceTextureReadbackRG(
                    device, *rg2Frame, previewTexture,
                    Rendering::ResourceState::ShaderResource, "AssetViewVideoPreview");
                // Decoded video frames are authored sRGB — the same stamp the Asset
                // View panel registers this texture to the UI with.
                *sourceSpaceHolder = UI::UITextureSpace::SrgbAuthored();
                return;
            }

            // ---- VIEWPORT path: scene-view pipeline-output readback ------------
            if (target == "viewport")
            {
                if (!wndCtx->scene)
                {
                    wait->RecordBlocked("the editor window has no scene view controller");
                    return;
                }
                auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
                if (!renderServices)
                {
                    wait->RecordBlocked("the editor has no render services");
                    return;
                }
                // The active scene view may be a maximized non-Perspective quad pane
                // (a secondary controller), not ctx->scene. Read back the controller
                // that's actually rendering the visible viewport, else the readback
                // targets a suspended view and never schedules.
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                Rendering::ViewId viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
                if (viewId == 0)
                {
                    wait->RecordRefusal("the scene view has no registered view yet");
                    return;
                }
                // The scene view's pipeline output — the texture the UI
                // samples (post-FX FinalColor).
                const auto out = renderServices->GetPipelineOutputRG(*rg2Frame, viewId);
                if (!out.IsValid())
                {
                    // The pane declared no pipeline output this frame: its tab is
                    // behind another, its rect collapsed, or its view is a frame
                    // behind on extraction. The first two do not clear on their
                    // own; the wait tells them apart by how long the refusal lasts.
                    wait->RecordRefusal(
                        "the scene view produced no render-graph output — its tab may be behind "
                        "another, its pane collapsed, or the view is still catching up");
                    return;
                }
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, out.Out, "ViewportScreenshot");
                // The pipeline's own stamp for what it wrote into FinalColor —
                // LINEAR, overlays and gizmos included. The view's own finalize
                // is downstream of this point, so a viewport capture does its own
                // encode and its own 8-bit filters rather than inheriting the
                // screen's, which are sized to the display's step. Same content,
                // filtered for the file's depth.
                *sourceSpaceHolder = renderServices->GetPipelineOutputSpaceRG();
                return;
            }

            // ---- WINDOW / PANEL / ELEMENT / RECT: presentation-target readback --
            // Every pure editor frame routes through Editor.FinalLinear, so
            // the full composite is always readable there.
            if (wndCtx->rg2Capture.finalLinearId != 0xFFFFFFFFu)
            {
                Rendering::RenderGraph::RGTexture finalLinear{};
                finalLinear.Id = wndCtx->rg2Capture.finalLinearId;
                *ticketHolder = Rendering::RequestTextureReadbackRG(
                    device, *rg2Frame, finalLinear, "WindowScreenshot");
                // The frame driver publishes the space the composite's bytes
                // hold under the declared UI target (#767/#784): SrgbAuthored
                // on SDR frames (encoded blend — no second OETF at readback),
                // DisplayLinearSdr on HDR-display capture frames.
                *sourceSpaceHolder = wndCtx->rg2Capture.compositeSpace;
                // This window's encode declared just before this callback ran —
                // the publish is exactly the state this readback's pixels see.
                *encodeFilterStateHolder = CaptureEncodeFilterState{
                    Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()};
            }
            else
            {
                wait->RecordRefusal("the frame declared no composite to read back");
                return;
            }

            // Compute crop rect for panel/element/rect targets (image-space crop).
            // GetLayoutX/Y/Width/Height returns logical pixels; the readback is in
            // physical pixels — scale by content scale.
            if (target == "window")
                return;

            float layoutX = 0.0f;
            float layoutY = 0.0f;
            float layoutW = 0.0f;
            float layoutH = 0.0f;
            std::string label;
            bool found = false;

            if (target == "panel")
            {
                if (app.m_Docking)
                {
                    if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                    {
                        layoutX = panel->GetLayoutX();
                        layoutY = panel->GetLayoutY();
                        layoutW = panel->GetLayoutWidth();
                        layoutH = panel->GetLayoutHeight();
                        label = "panel:" + panelId;
                        found = true;
                    }
                }
            }
            else if (target == "element")
            {
                if (wndCtx->ui)
                {
                    if (UIElement* root = wndCtx->ui->GetRootElement())
                    {
                        if (UIElement* el = root->FindById(elementId))
                        {
                            layoutX = el->GetLayoutX();
                            layoutY = el->GetLayoutY();
                            layoutW = el->GetLayoutWidth();
                            layoutH = el->GetLayoutHeight();
                            label = "element:" + elementId;
                            found = true;
                        }
                    }
                }
            }
            else if (target == "rect")
            {
                layoutX = static_cast<float>(rectX);
                layoutY = static_cast<float>(rectY);
                layoutW = static_cast<float>(rectW);
                layoutH = static_cast<float>(rectH);
                label = "rect";
                found = true;
            }

            if (!found || layoutW <= 0.0f || layoutH <= 0.0f)
            {
                LOG_WARNING("Screenshot: crop target not found or empty — returning full window");
                return;
            }

            // panel/element values come from layout (always logical). For
            // rect, honor the caller's coords choice.
            const bool applyScale = (target != "rect") || (coords == "logical");
            const float scale = applyScale && wndCtx->ui
                                    ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                    : 1.0f;
            cropHolder->valid = true;
            cropHolder->x = static_cast<int32_t>(std::floor(layoutX * scale));
            cropHolder->y = static_cast<int32_t>(std::floor(layoutY * scale));
            cropHolder->w = static_cast<int32_t>(std::ceil(layoutW * scale));
            cropHolder->h = static_cast<int32_t>(std::ceil(layoutH * scale));
            cropHolder->label = std::move(label);
            LOG_DEBUG("Screenshot: crop {} -> ({},{},{},{})",
                      cropHolder->label, cropHolder->x, cropHolder->y, cropHolder->w, cropHolder->h);
        };
        *attemptQueued = true;
        server.EnqueueRenderCallback([attempt]() { (*attempt)(); });

        // Deferred poll: each frame, re-arm the attempt until the readback is
        // declared, then wait for the GPU copy. The wait decides when to stop and
        // says why; a window capture is served only to a caller that asked for
        // one, because it is a picture of the window rather than the frame the
        // render graph produced.
        const auto requestStart = std::chrono::steady_clock::now();
        // Naming the opt-in flag helps only a caller that has not already passed
        // it, and only where an OS window capture can stand in: asset_preview
        // reads a video texture no picture of the window substitutes for.
        const bool offerWindowCaptureHint = !allowWindowCapture && target != "asset_preview";
        // The job that converts, encodes and serializes the finished readback.
        auto encodeHolder = std::make_shared<JobSystem::TaskHandle>();
        server.EnqueueDeferredResponse(requestId,
            [&app, &server, attempt, attemptQueued, wait, ticketHolder, requestStart, cropHolder,
             sourceSpaceHolder,
             assetPreviewPathHolder, frameIndexHolder, encodeFilterStateHolder, encodeHolder, target,
             panelId, elementId, rectX, rectY, rectW, rectH, coords, dither, deband,
             offerWindowCaptureHint, requestId](json& outResult) -> bool
            {
                auto reportFailure = [&](const std::string& reason,
                                         bool offerWindowCapture = false) -> bool
                {
                    LOG_WARNING("Screenshot ({}): {}", target, reason);
                    outResult =
                        Editor::RefuseRequest(Editor::ScreenshotFailureMessage(reason, offerWindowCapture),
                                              json{{"fallbackReason", reason}});
                    return true;
                };

                // The readback has been handed to the encode job: wait for its line.
                if (encodeHolder->IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(*encodeHolder, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        return reportFailure("the screenshot encode job did not complete: " + jobError);
                    return true;
                }

                auto windowCapture = [&](const std::string& reason) -> bool
                {
                    // asset_preview reads a video texture no window capture can
                    // substitute for — it fails whatever the caller allowed.
                    if (target == "asset_preview")
                        return reportFailure(reason + "; asset_preview has no window-capture substitute");
                    if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->window)
                        return reportFailure(reason + "; no window to capture either");
                    auto* wndCtx = app.m_Windows[0].get();
                    std::vector<uint8_t> rgba8;
                    uint32_t srcW = 0;
                    uint32_t srcH = 0;
                    if (!CaptureWindowClientAreaRGBA8(wndCtx->window.get(), rgba8, srcW, srcH))
                        return reportFailure(reason +
                                             "; the window capture also failed (non-Windows "
                                             "platform, hidden window, or capture error)");
                    LOG_WARNING("Screenshot ({}): {} — served the window capture the request allowed",
                                target, reason);

                    // The render callback that normally fills cropHolder may
                    // never have run on this path — compute the crop from the
                    // live layout instead. viewport degrades to the scene-view
                    // mount's on-screen region of the composite.
                    ScreenshotCropDesc crop;
                    float layoutX = 0.0f;
                    float layoutY = 0.0f;
                    float layoutW = 0.0f;
                    float layoutH = 0.0f;
                    bool found = false;
                    if (target == "panel" && app.m_Docking)
                    {
                        if (UIElement* panel = app.m_Docking->GetPanel(panelId))
                        {
                            layoutX = panel->GetLayoutX();
                            layoutY = panel->GetLayoutY();
                            layoutW = panel->GetLayoutWidth();
                            layoutH = panel->GetLayoutHeight();
                            crop.label = "panel:" + panelId;
                            found = true;
                        }
                    }
                    else if ((target == "element" || target == "viewport") && wndCtx->ui)
                    {
                        if (UIElement* root = wndCtx->ui->GetRootElement())
                        {
                            const std::string lookupId =
                                target == "viewport" ? std::string(EditorPanelIds::MountSceneView)
                                                     : elementId;
                            if (UIElement* el = root->FindById(lookupId))
                            {
                                layoutX = el->GetLayoutX();
                                layoutY = el->GetLayoutY();
                                layoutW = el->GetLayoutWidth();
                                layoutH = el->GetLayoutHeight();
                                crop.label = "element:" + lookupId;
                                found = true;
                            }
                        }
                    }
                    else if (target == "rect")
                    {
                        layoutX = static_cast<float>(rectX);
                        layoutY = static_cast<float>(rectY);
                        layoutW = static_cast<float>(rectW);
                        layoutH = static_cast<float>(rectH);
                        crop.label = "rect";
                        found = true;
                    }
                    if (found && layoutW > 0.0f && layoutH > 0.0f)
                    {
                        const bool applyScale = (target != "rect") || (coords == "logical");
                        const float scale = applyScale && wndCtx->ui
                                                ? std::max(0.01f, wndCtx->ui->GetContentScale())
                                                : 1.0f;
                        crop.valid = true;
                        crop.x = static_cast<int32_t>(std::floor(layoutX * scale));
                        crop.y = static_cast<int32_t>(std::floor(layoutY * scale));
                        crop.w = static_cast<int32_t>(std::ceil(layoutW * scale));
                        crop.h = static_cast<int32_t>(std::ceil(layoutH * scale));
                    }

                    FinishScreenshotResult(outResult, std::move(rgba8), srcW, srcH, crop, target,
                                           "printwindow", PendingMaterialTextureBinds());
                    outResult["fallbackReason"] = reason;
                    return true;
                };

                Rendering::ViewReadbackResult result;
                bool readbackReady = false;
                // RenderGraph arm: deterministic ticket completion (8c-1). The
                // attempt creates the ticket in the render phase.
                if (*ticketHolder)
                {
                    if ((*ticketHolder)->IsConsumed())
                    {
                        // A cancelled ticket (declaring frame abandoned) can never
                        // resolve — drop it and let a later frame declare again.
                        ticketHolder->reset();
                        wait->RecordRefusal("the frame that declared the readback was abandoned "
                                            "before submit");
                    }
                    else
                    {
                        wait->RecordReadbackDeclared();
                        readbackReady = (*ticketHolder)->TryGet(result);
                    }
                }

                if (!readbackReady)
                {
                    // The same condition the render loop skips a window on: with no
                    // framebuffer there is no frame to declare a readback into, and
                    // no later frame changes that on its own.
                    if (!*ticketHolder && !app.m_Windows.empty() && app.m_Windows[0] &&
                        app.m_Windows[0]->window)
                    {
                        int fbW = 0;
                        int fbH = 0;
                        app.m_Windows[0]->window->GetFramebufferSize(fbW, fbH);
                        if (fbW <= 0 || fbH <= 0)
                            wait->RecordBlocked("the editor window has no framebuffer (minimized "
                                                "or hidden), so it renders no frame to capture");
                    }
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - requestStart)
                            .count();
                    const auto outcome = wait->Step(elapsed);
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::ServeWindowCapture)
                        return windowCapture(wait->GiveUpReason());
                    if (outcome == Editor::ScreenshotCaptureWait::Outcome::Fail)
                        return reportFailure(wait->GiveUpReason(), offerWindowCaptureHint);
                    // Keep waiting: the next rendered frame gets its own attempt.
                    if (!*ticketHolder && !*attemptQueued)
                    {
                        *attemptQueued = true;
                        server.EnqueueRenderCallback([attempt]() { (*attempt)(); });
                    }
                    return false;
                }

                LOG_DEBUG("Screenshot: readback complete {}x{} fmt={}", result.width, result.height,
                          static_cast<uint32_t>(result.format));

                // The declaring arm states the space; a scheduled ticket without one is
                // a capture target that forgot to, not a licence to infer.
                if (!*sourceSpaceHolder)
                    return reportFailure("the readback declared no source colour space");

                // The frame's share ends with the readback: conversion, PNG encode,
                // file write, base64 and serialization run on a job, and a later poll
                // sends the line it produced.
                *encodeHolder = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), srgbEncode = !UI::IsEncodedAtRest(**sourceSpaceHolder),
                     dither, deband,
                     filterState = encodeFilterStateHolder->value_or(CaptureEncodeFilterState{
                         Rendering::Passes::GetBackbufferOutputDebandThresholdLsb()}),
                     crop = *cropHolder, target,
                     frameIndex = *frameIndexHolder, previewPath = *assetPreviewPathHolder,
                     pendingBinds = PendingMaterialTextureBinds()]() -> json
                    {
                        auto rgba8 = ConvertToRGBA8(readback.pixels.data(), readback.width, readback.height,
                                                    readback.format, 0.0f, 1.0f, srgbEncode, dither, deband,
                                                    &filterState);
                        json reply;
                        FinishScreenshotResult(reply, std::move(rgba8), readback.width, readback.height, crop,
                                               target, "rendergraph", pendingBinds);
                        if (!Editor::IsRefusal(reply))
                        {
                            reply["frameIndex"] = frameIndex;
                            if (target == "asset_preview" && !previewPath.empty())
                                reply["previewPath"] = previewPath;
                        }
                        return reply;
                    });
                if (!encodeHolder->IsValid())
                    return reportFailure("the job system is shutting down; the capture was not encoded");
                return false;
            },
            Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

    // Shadow/draw-stream readback diagnostics: returns compact hashes only.
    // Intended for backend-specific flicker investigations where screenshots
    // show instability but CPU-side culling/cascade data is stable.
    server.RegisterHandler("capture_shadow_diagnostics", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        const std::string requestId = ctx.id;
        const int requestedViewId = ctx.params.value("viewId", 0);
        const int requestedCascade = ctx.params.value("cascade", -1);
        const uint32_t maxStreams = std::max(1u, ctx.params.value("maxStreams", 8u));
        const uint32_t maxRecords = std::max(1u, ctx.params.value("maxRecords", 64u));

        struct ShadowTicket
        {
            uint32_t cascade = 0;
            std::shared_ptr<Rendering::RGReadbackTicket> ticket;
        };
        struct BufferTicket
        {
            std::string label;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> ticket;
        };
        struct StreamTickets
        {
            uint64_t streamKey = 0;
            uint32_t materialIndex = 0;
            uint32_t meshIndex = 0;
            // The resolved axes the range was published under (depth-class
            // sentinel or color class, and pool group under consolidation).
            uint32_t classKey = 0;
            uint32_t meshKey = 0;
            uint32_t maxDrawCount = 0;
            uint32_t recordsRead = 0;
            BufferTicket count;
            BufferTicket commands;
            BufferTicket indirection;
        };
        struct CaptureState
        {
            Rendering::ViewId viewId = 0;
            uint32_t shadowResolution = 0;
            std::vector<ShadowTicket> shadows;
            std::vector<StreamTickets> streams;
            std::string setupError;
        };
        auto state = std::make_shared<CaptureState>();

        auto fnv64 = [](const uint8_t* data, size_t size) -> uint64_t
        {
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= static_cast<uint64_t>(data[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        };
        (void)fnv64;

        server.EnqueueRenderCallback([&app, state, requestedViewId, requestedCascade, maxStreams, maxRecords]()
        {
            if (app.m_Windows.empty())
            {
                state->setupError = "No editor window";
                return;
            }
            auto* wndCtx = app.m_Windows[0].get();
            auto* renderServices = app.m_EditorContext ? app.m_EditorContext->RenderServices : nullptr;
            if (!renderServices)
            {
                state->setupError = "No RenderServices";
                return;
            }
            auto* device = renderServices->GetDevice();
            if (!device)
            {
                state->setupError = "No render device";
                return;
            }
            auto* rg2Frame = wndCtx->rg2Capture.frame;
            const bool rg2 = rg2Frame && wndCtx->rg2Capture.frameIndex == rg2Frame->FrameIndex();
            if (!rg2)
            {
                state->setupError = "No RenderGraph capture context this frame";
                return;
            }

            Rendering::ViewId viewId = static_cast<Rendering::ViewId>(std::max(0, requestedViewId));
            if (viewId == 0)
            {
                SceneViewController* activeSceneCtrl = wndCtx->scene.get();
                if (wndCtx->ui)
                    if (auto* root = wndCtx->ui->GetRootElement())
                        if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                            if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                                if (auto* ac = svPanel->GetSceneControllerForSlot(svPanel->GetActiveViewportSlot()))
                                    activeSceneCtrl = ac;
                viewId = activeSceneCtrl ? activeSceneCtrl->GetViewId() : 0u;
            }
            if (viewId == 0)
            {
                state->setupError = "Invalid viewId";
                return;
            }
            state->viewId = viewId;

            const auto shadowArr = renderServices->GetShadowMapArrayRG(*rg2Frame, viewId);
            if (shadowArr.IsValid())
            {
                const auto& desc = rg2Frame->Graph().ResourceDesc(shadowArr.Id);
                state->shadowResolution = desc.Width;
                const uint32_t layers = std::max(1u, desc.ArrayLayers);
                const uint32_t firstCascade =
                    requestedCascade >= 0 ? static_cast<uint32_t>(requestedCascade) : 0u;
                const uint32_t endCascade =
                    requestedCascade >= 0 ? std::min(layers, firstCascade + 1u) : layers;
                for (uint32_t c = firstCascade; c < endCascade; ++c)
                {
                    ShadowTicket st{};
                    st.cascade = c;
                    st.ticket = Rendering::RequestTextureSubresourceReadbackRG(
                        device, *rg2Frame, shadowArr, 0, c, 0, 0, 0, 0, "ShadowCascadeDiag");
                    if (st.ticket)
                        state->shadows.push_back(std::move(st));
                }
            }

            auto* drawStream = renderServices->GetDrawStreamBuilder();
            const auto& frameRG = renderServices->FrameRG();
            const bool orderingValid =
                frameRG.For.IsFor(*rg2Frame) &&
                frameRG.DrawStreamOrdering.IsValid();
            if (drawStream && orderingValid)
            {
                const auto keys = renderServices->GetEntityBatchKeys(viewId);
                const uint8_t cascadeIdx =
                    requestedCascade >= 0
                        ? static_cast<uint8_t>(requestedCascade)
                        : 0u;
                // Same resolved axes the shadow recorder looks up on: the
                // shared-depth class sentinel for eligible casters, and the
                // pool group under draw consolidation. Both merges are
                // many-keys-to-one-range, so keys that land on a range already
                // sampled are skipped rather than spending a readback slot.
                const std::span<const uint32_t> meshPoolGroups =
                    renderServices->MeshPoolGroupSpanForDraws();
                std::unordered_set<uint64_t> sampledStreams;
                uint32_t emitted = 0;
                for (const auto& key : keys)
                {
                    if (emitted >= maxStreams)
                        break;
                    const Engine::Renderer::DrawStreamLookupKey lookup =
                        Engine::Renderer::ResolveDrawStreamLookupKey(
                            key, cascadeIdx,
                            renderServices->Materials().GetMaterialDepthClass(key.materialIndex),
                            meshPoolGroups);
                    const uint64_t streamKey = Rendering::GPUDrawStreamBuilder::MakeStreamKey(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!sampledStreams.insert(streamKey).second)
                        continue;
                    const auto range = drawStream->FindBatchDrawRange(
                        static_cast<uint32_t>(viewId), cascadeIdx, lookup.classKey, lookup.meshKey,
                        Rendering::GPUDrawStreamBuilder::SlicePhase::A);
                    if (!range.IsValid())
                        continue;

                    const uint32_t records =
                        std::min(maxRecords, std::max(1u, range.even.maxDrawCount));
                    StreamTickets stream{};
                    stream.streamKey = streamKey;
                    stream.materialIndex = key.materialIndex;
                    stream.meshIndex = key.meshIndex;
                    stream.classKey = lookup.classKey;
                    stream.meshKey = lookup.meshKey;
                    stream.maxDrawCount = range.even.maxDrawCount;
                    stream.recordsRead = records;

                    const std::string prefix =
                        "ShadowStreamDiag." + std::to_string(static_cast<uint32_t>(viewId)) +
                        ".c" + std::to_string(static_cast<uint32_t>(cascadeIdx)) +
                        ".m" + std::to_string(lookup.meshKey) +
                        ".mat" + std::to_string(lookup.classKey);
                    // Shared arena buffers: read this batch's region at the
                    // range's byte offsets. Indirection region starts at the
                    // same record index (one uint per record).
                    const size_t indirByteOffset =
                        range.even.cmdByteOffset / (5u * sizeof(uint32_t)) * sizeof(uint32_t);
                    const auto cmdRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Cmd", range.recordBuffer);
                    const auto countRG = rg2Frame->ImportExternalBuffer("ShadowStreamDiag.Count", range.countBuffer);
                    const auto indirRG = rg2Frame->ImportExternalBuffer(
                        "ShadowStreamDiag.Indirection", drawStream->GetSharedIndirectionBuffer());

                    stream.commands.label = "commands";
                    stream.commands.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, cmdRG, range.even.cmdByteOffset,
                        static_cast<size_t>(records) * 5u * sizeof(uint32_t),
                        (prefix + ".cmd").c_str());
                    stream.count.label = "count";
                    stream.count.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, countRG, range.even.countByteOffset, sizeof(uint32_t),
                        (prefix + ".count").c_str());
                    stream.indirection.label = "indirection";
                    stream.indirection.ticket = Rendering::RequestBufferReadbackRG(
                        device, *rg2Frame, indirRG, indirByteOffset,
                        static_cast<size_t>(records) * sizeof(uint32_t),
                        (prefix + ".indir").c_str());
                    state->streams.push_back(std::move(stream));
                    ++emitted;
                }
            }
        });

        auto pollCount = std::make_shared<int>(0);
        server.EnqueueDeferredResponse(requestId,
            [state, pollCount, fnv64](json& outResult) -> bool
            {
                ++(*pollCount);
                if (!state->setupError.empty())
                {
                    outResult = Editor::RefuseRequest(state->setupError);
                    return true;
                }
                if (state->shadows.empty() && state->streams.empty())
                {
                    if (*pollCount > 5)
                    {
                        outResult = Editor::RefuseRequest("No shadow or stream readbacks were scheduled");
                        return true;
                    }
                    return false;
                }

                std::vector<Rendering::ViewReadbackResult> shadowResults;
                shadowResults.resize(state->shadows.size());
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    if (!state->shadows[i].ticket)
                        continue;
                    if (state->shadows[i].ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A shadow readback ticket was cancelled");
                        return true;
                    }
                    if (!state->shadows[i].ticket->TryGet(shadowResults[i]))
                        return false;
                }

                struct StreamReadback
                {
                    Rendering::BufferReadbackResult count;
                    Rendering::BufferReadbackResult commands;
                    Rendering::BufferReadbackResult indirection;
                };
                std::vector<StreamReadback> streamResults;
                streamResults.resize(state->streams.size());
                auto tryBuffer = [&](const BufferTicket& ticket, Rendering::BufferReadbackResult& out) -> bool
                {
                    if (!ticket.ticket)
                        return true;
                    if (ticket.ticket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("A buffer readback ticket was cancelled");
                        return false;
                    }
                    return ticket.ticket->TryGet(out);
                };
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    if (!tryBuffer(state->streams[i].count, streamResults[i].count))
                        return false;
                    if (!tryBuffer(state->streams[i].commands, streamResults[i].commands))
                        return false;
                    if (!tryBuffer(state->streams[i].indirection, streamResults[i].indirection))
                        return false;
                }

                json shadows = json::array();
                for (size_t i = 0; i < state->shadows.size(); ++i)
                {
                    const auto& rb = shadowResults[i];
                    float minDepth = std::numeric_limits<float>::infinity();
                    float maxDepth = -std::numeric_limits<float>::infinity();
                    uint64_t nonZero = 0;
                    if (rb.format == Rendering::TextureFormat::D32_FLOAT && !rb.pixels.empty())
                    {
                        const size_t count = rb.pixels.size() / sizeof(float);
                        const float* values = reinterpret_cast<const float*>(rb.pixels.data());
                        for (size_t n = 0; n < count; ++n)
                        {
                            minDepth = std::min(minDepth, values[n]);
                            maxDepth = std::max(maxDepth, values[n]);
                            if (values[n] != 0.0f)
                                ++nonZero;
                        }
                    }
                    shadows.push_back({
                        {"cascade", state->shadows[i].cascade},
                        {"width", rb.width},
                        {"height", rb.height},
                        {"format", static_cast<uint32_t>(rb.format)},
                        {"byteCount", static_cast<uint64_t>(rb.pixels.size())},
                        {"hash", fnv64(rb.pixels.data(), rb.pixels.size())},
                        {"minDepth", std::isfinite(minDepth) ? minDepth : 0.0f},
                        {"maxDepth", std::isfinite(maxDepth) ? maxDepth : 0.0f},
                        {"nonZeroDepthSamples", nonZero}
                    });
                }

                json streams = json::array();
                for (size_t i = 0; i < state->streams.size(); ++i)
                {
                    const auto& src = state->streams[i];
                    const auto& rb = streamResults[i];
                    uint32_t drawCount = 0;
                    if (rb.count.bytes.size() >= sizeof(uint32_t))
                        std::memcpy(&drawCount, rb.count.bytes.data(), sizeof(uint32_t));

                    json commands = json::array();
                    const size_t commandCount = rb.commands.bytes.size() / (5u * sizeof(uint32_t));
                    const size_t previewCount = std::min<size_t>(commandCount, 8u);
                    const uint32_t* words = reinterpret_cast<const uint32_t*>(rb.commands.bytes.data());
                    for (size_t c = 0; c < previewCount; ++c)
                    {
                        const uint32_t* cmd = words + c * 5u;
                        int32_t vertexOffset = 0;
                        std::memcpy(&vertexOffset, &cmd[3], sizeof(int32_t));
                        commands.push_back({
                            {"indexCount", cmd[0]},
                            {"instanceCount", cmd[1]},
                            {"firstIndex", cmd[2]},
                            {"vertexOffset", vertexOffset},
                            {"firstInstance", cmd[4]}
                        });
                    }

                    json indirection = json::array();
                    const size_t indirCount = rb.indirection.bytes.size() / sizeof(uint32_t);
                    const size_t indirPreview = std::min<size_t>(indirCount, 16u);
                    const uint32_t* indir = reinterpret_cast<const uint32_t*>(rb.indirection.bytes.data());
                    for (size_t n = 0; n < indirPreview; ++n)
                        indirection.push_back(indir[n]);

                    streams.push_back({
                        {"streamKey", src.streamKey},
                        {"materialIndex", src.materialIndex},
                        {"meshIndex", src.meshIndex},
                        {"drawStreamClassKey", src.classKey},
                        {"drawStreamMeshKey", src.meshKey},
                        {"drawCount", drawCount},
                        {"maxDrawCount", src.maxDrawCount},
                        {"recordsRead", src.recordsRead},
                        {"countHash", fnv64(rb.count.bytes.data(), rb.count.bytes.size())},
                        {"commandHash", fnv64(rb.commands.bytes.data(), rb.commands.bytes.size())},
                        {"indirectionHash", fnv64(rb.indirection.bytes.data(), rb.indirection.bytes.size())},
                        {"commands", std::move(commands)},
                        {"indirection", std::move(indirection)}
                    });
                }

                outResult = {
                    {"viewId", static_cast<uint32_t>(state->viewId)},
                    {"shadowResolution", state->shadowResolution},
                    {"shadows", std::move(shadows)},
                    {"streams", std::move(streams)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // Programmatic scroll: drives a ScrollView's scroll offset directly.
    // Useful for debug repros of virtualization regressions (e.g. take_screenshot
    // after scrolling). Resolves the target by elementId (must reference a
    // ScrollView or an ancestor whose first ScrollView descendant is targeted).
    server.RegisterHandler("set_scroll", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");
        const std::string elementId = ctx.params.value("elementId", std::string());
        if (elementId.empty())
            return Editor::RefuseRequest("set_scroll requires 'elementId'");
        auto* wndCtx = app.m_Windows[0].get();
        if (!wndCtx->ui)
            return Editor::RefuseRequest("No UIManager");
        UIElement* root = wndCtx->ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");
        UIElement* el = root->FindById(elementId);
        // Docked panel content is owned by the docking model and may be
        // mounted through a portal rather than beneath the window root.
        // Match get_panel_tree's reach so MCP can drive virtualized panel
        // controls such as assets-grid.
        if (!el && app.m_Docking)
        {
            for (const auto& [_, panel] : app.m_Docking->GetPanels())
            {
                if (panel && (el = panel->FindById(elementId)))
                    break;
            }
        }
        if (!el)
            return Editor::RefuseRequest("Element not found: " + elementId);
        // Find a ScrollView at or below the targeted element. Most virtualized
        // controls (ListView/TreeView/GridView) own a ScrollView descendant.
        std::function<ScrollView*(UIElement*)> findScroll = [&](UIElement* node) -> ScrollView*
        {
            if (auto* sv = dynamic_cast<ScrollView*>(node))
                return sv;
            for (auto& child : node->GetChildren())
                if (auto* found = findScroll(child.get()))
                    return found;
            return nullptr;
        };
        ScrollView* sv = findScroll(el);
        if (!sv)
            return Editor::RefuseRequest("No ScrollView at or below target");
        if (ctx.params.contains("x"))
            sv->SetScrollX(ctx.params.value("x", 0.0f));
        if (ctx.params.contains("y"))
            sv->SetScrollY(ctx.params.value("y", 0.0f));
        return json{{"ok", true},
                    {"scrollX", sv->GetScrollX()},
                    {"scrollY", sv->GetScrollY()},
                    {"contentHeight", sv->GetContentHeight()},
                    {"viewportHeight", sv->GetViewportHeight()}}; });

    // 14. shutdown
    // Gracefully exits the editor. Useful to release PDB locks before rebuilds.
    server.RegisterHandler("shutdown", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        app.RequestExit();
        return json{{"ok", true}}; });

    // Undocks a panel into a floating OS window — exercises the multi-window
    // render-target path from the test harness.
    server.RegisterHandler("undock_panel", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string panelId = ctx.params.value("panelId", "");
        if (panelId.empty())
            return Editor::RefuseRequest("panelId required");
        app.QueueUndockPanel(panelId);
        return json{{"ok", true}, {"panelId", panelId}}; });

    // 15. execute_command
    // Dispatches a named editor command by its numeric ID or well-known name.
    // This reuses the UI replay command infrastructure.
    server.RegisterHandler("execute_command", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Map well-known string names to command IDs. Play-mode transitions are not
        // here: set_play_mode owns them, because it reports the resulting state and
        // refuses an illegal transition instead of answering ok to a no-op.
        static const std::unordered_map<std::string, uint32_t> kNamedCommands = {
            {"create_empty",          0x4101u},
            {"create_cube",           0x4110u},
            {"create_sphere",         0x4111u},
            {"create_capsule",        0x4112u},
            {"create_plane",          0x4113u},
            {"create_camera",         0x4120u},
            {"create_light_dir",      0x4130u},
            {"create_light_point",    0x4131u},
            {"create_light_spot",     0x4132u},
            {"create_light_ambient",  0x4133u},
            // Every entry of the Create/Terrain menu, so automation can reach the planet
            // presets by name; create_terrain is the small planar default.
            {"create_terrain",        0x4140u},
            {"create_terrain_large",  0x4147u},
            {"create_planet_5km",     0x4148u},
            {"create_planet_50km",    0x4149u},
            {"create_particle_emitter", 0x4142u},
            {"assets_view_grid",      0xF0A10101u},
            {"assets_view_list",      0xF0A10102u},
            {"reimport_lods",         0xF0B10001u},
            {"bake_hlod",             0xF0B10002u},
            {"save_scene",            0xF0B10003u},
        };

        uint32_t commandId = 0;

        if (ctx.params.contains("commandId"))
        {
            commandId = ctx.params["commandId"].get<uint32_t>();
        }
        else if (ctx.params.contains("name"))
        {
            std::string name = ctx.params["name"].get<std::string>();
            auto it = kNamedCommands.find(name);
            if (it == kNamedCommands.end())
                return Editor::RefuseRequest("Unknown command name: " + name);
            commandId = it->second;
        }
        else
        {
            return Editor::RefuseRequest("Provide commandId (uint32) or name (string)");
        }

        // A visible scene modal parks pending opens/saves; dispatching editor
        // commands underneath it acts on half-committed document state, and
        // several command flows raise their own prompts on top. Reject with
        // the modal kind so the caller answers it first (respond_modal /
        // open_scene restorePolicy).
        if (app.m_SceneEditor)
        {
            const std::string modalKind = app.m_SceneEditor->GetActiveModalKind();
            if (!modalKind.empty())
                return Editor::RefuseRequest("Editor is blocked on the '" + modalKind
                           + "' modal — answer it (respond_modal) before execute_command");
        }

        std::string error;
        bool ok = app.InvokeUiReplayCommand(commandId, &error);
        if (!ok)
            return Editor::RefuseRequest(error.empty() ? "Command failed" : error);

        return json{{"ok", true}, {"commandId", commandId}}; });

    // Batch-import ProFlares-authored lens flare content (TexturePacker atlas
    // .txt + PNG pairs, flare .txt docs) from an external folder into the open
    // project as engine-native .flareatlas/.lensflare assets. First (and so far
    // only) caller of LensFlareImport::ImportFolder — the same entry point a
    // future asset-browser menu action will call.
    server.RegisterHandler("import_proflares", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string sourceDir = ctx.params.value("sourceDir", "");
        if (sourceDir.empty())
            return Editor::RefuseRequest("Provide sourceDir (absolute path to the ProFlares folder)");
        std::error_code ec;
        if (!std::filesystem::is_directory(sourceDir, ec))
            return Editor::RefuseRequest("sourceDir is not a directory: " + sourceDir);

        auto& am = EngineCore::GetInstance().GetAssetManager();
        const std::filesystem::path projectRoot = am.GetSourceRoot(kAssetSourceAliasProject);
        if (projectRoot.empty())
            return Editor::RefuseRequest("No project asset root mounted");

        const std::string subDir = ctx.params.value("outputSubDir", std::string{"LensFlares"});
        const std::filesystem::path outputDir = projectRoot / subDir;

        Logger::Log::Info("import_proflares: scanning '{}' -> '{}'", sourceDir, outputDir.string());
        std::vector<LensFlareImport::FolderImportEntry> report;
        int written = 0;
        try
        {
            written = LensFlareImport::ImportFolder(sourceDir, outputDir, subDir, report);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("import_proflares: ImportFolder threw: {}", e.what());
            json entries = json::array();
            for (const auto& r : report)
                entries.push_back(json{{"source", r.Source.string()},
                                       {"note", r.Note},
                                       {"ok", r.Ok}});
            return Editor::RefuseRequest(std::string("ImportFolder threw: ") + e.what(),
                                         json{{"entriesBeforeThrow", std::move(entries)}});
        }
        Logger::Log::Info("import_proflares: wrote {} assets ({} report entries)", written,
                          report.size());

        json entries = json::array();
        for (const auto& e : report)
            entries.push_back(json{{"source", e.Source.string()},
                                   {"output", e.Output.string()},
                                   {"note", e.Note},
                                   {"ok", e.Ok}});
        return json{{"ok", true},
                    {"written", written},
                    {"outputDir", outputDir.string()},
                    {"entries", std::move(entries)}}; });

    server.RegisterHandler("open_asset", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string requestedPath = ctx.params.value("path", std::string{});
        if (requestedPath.empty())
            return Editor::RefuseRequest("Provide path");
        const std::filesystem::path path =
            Editor::ResolveOpenAssetPath(EngineCore::GetInstance().GetAssetManager(), requestedPath);
        if (path.empty())
            return Editor::RefuseRequest("Cannot resolve asset path: " + requestedPath);

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".anim" || ext == ".animation" || ext == ".fbx" || ext == ".gltf" ||
            ext == ".glb" || ext == ".timeline" || ext == ".clipset")
        {
            auto* animationPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::Animation);
            auto* targetPanel = animationPanel;
            const char* targetPanelId = EditorPanelIds::Animation;
            if (ext == ".timeline")
            {
                targetPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::Timeline);
                targetPanelId = EditorPanelIds::Timeline;
            }
            else if (ext == ".clipset")
            {
                targetPanel = FindAnimationPanelOfKind(app.m_PanelStorage, AnimationWindowPanel::PanelKind::ClipEditor);
                targetPanelId = EditorPanelIds::ClipEditor;
            }
            if (!targetPanel)
                targetPanel = animationPanel ? animationPanel : FindPanel<AnimationWindowPanel>(app.m_PanelStorage);
            if (!targetPanel)
                return Editor::RefuseRequest("Animation panel not found");
            if (app.m_PanelManager && !app.m_Windows.empty())
                app.m_PanelManager->ShowOrActivatePanel(app.m_Windows[0].get(), targetPanelId, EditorPanelIds::SceneView);
            if (!targetPanel->OpenAnimationAssetPath(path))
                return Editor::RefuseRequest("Animation panel failed to open asset");
            return json{{"ok", true}, {"path", path.string()}};
        }

        if (ExtensionOpensInGraphPanel(ext))
        {
            if (ext == ".glsl" && !Editor::FileIsShaderGraphGlsl(path))
            {
                return Editor::RefuseRequest("GLSL is not a shader graph file (use script editor instead)");
            }

            const auto started = std::chrono::steady_clock::now();
            if (!app.OpenGraphAsset(path))
                return Editor::RefuseRequest("Graph panel failed to open asset");

            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            GraphPanel* opened = GraphPanel::FindLiveLastOpened();
            const std::string kind = opened ? std::string(opened->PanelKindId())
                                            : KindIdFromGraphExtension(ext);
            std::string panelId;
            if (opened && !app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->docking)
            {
                for (const auto& [id, element] : app.m_Windows[0]->docking->GetPanels())
                {
                    if (element == opened)
                    {
                        panelId = id;
                        break;
                    }
                }
            }
            if (panelId.empty())
            {
                if (const char* kindDock = GraphDockPanelIdForKind(kind))
                    panelId = kindDock;
                else
                    panelId = EditorPanelIds::NodeGraph;
            }
            return json{{"ok", true},
                        {"path", path.string()},
                        {"panel", panelId},
                        {"kindId", kind},
                        {"openMs", elapsedMs}};
        }

        return Editor::RefuseRequest("Unsupported asset type: " + ext); });

    server.RegisterHandler("graph_set_viewport", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        float panX = 0.f;
        float panY = 0.f;
        float zoom = 1.f;
        canvas->GetPanZoom(panX, panY, zoom);
        panX = ctx.params.value("panX", panX);
        panY = ctx.params.value("panY", panY);
        zoom = ctx.params.value("zoom", zoom);
        canvas->SetPanZoom(panX, panY, zoom);
        canvas->GetPanZoom(panX, panY, zoom);
        return json{{"ok", true}, {"panX", panX}, {"panY", panY}, {"zoom", zoom}}; });

    server.RegisterHandler("graph_spike_dummy_nodes", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        const int count = ctx.params.value("count", 60);
        canvas->SpikeSpawnDummyNodes(count);
        return json{{"ok", true}, {"count", count}}; });

    server.RegisterHandler("graph_get_nodes", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        json nodes = json::array();
        for (const Graph::Node& node : model->Nodes)
        {
            nodes.push_back(json{{"id", node.Id},
                                 {"typeId", node.TypeId},
                                 {"x", node.PositionX},
                                 {"y", node.PositionY}});
        }
        return json{{"ok", true}, {"nodes", nodes}}; });

    server.RegisterHandler("graph_delete_node", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string nodeId = ctx.params.value("nodeId", std::string{});
        if (nodeId.empty())
            return Editor::RefuseRequest("Provide nodeId");
        GraphPanel* nodeGraphPanel = ResolveDebugGraphPanel();
        if (!nodeGraphPanel)
            return Editor::RefuseRequest("Node Graph panel not found");
        auto* canvas = dynamic_cast<GraphCanvas*>(nodeGraphPanel->FindById("NodeGraphCanvas"));
        if (!canvas)
            return Editor::RefuseRequest("NodeGraphCanvas not found");
        const Graph::Model* model = nodeGraphPanel->ActiveGraphModel();
        if (!model)
            return Editor::RefuseRequest("No graph model");
        const auto& nodes = model->Nodes;
        const bool exists = std::any_of(nodes.begin(), nodes.end(),
                                        [&nodeId](const Graph::Node& n) { return n.Id == nodeId; });
        if (!exists)
            return Editor::RefuseRequest("No node with id: " + nodeId);
        /* Same path as the Delete key: selection + DeleteSelectedNodes keeps
           undo, link cleanup and widget recycling on the one implementation. */
        canvas->SetSelectedNodeId(nodeId);
        canvas->DeleteSelectedNodes();
        return json{{"ok", true}, {"deleted", nodeId}}; });

    // Select by logical name instead of a visible tree-row element. This remains
    // deterministic when the Smart Folders section is collapsed or off-screen.
    server.RegisterHandler("select_smart_folder", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* assetsPanel = app.m_Docking
            ? dynamic_cast<AssetsPanel*>(app.m_Docking->GetPanel(EditorPanelIds::Assets))
            : nullptr;
        if (!assetsPanel)
            assetsPanel = FindPanel<AssetsPanel>(app.m_PanelStorage);
        if (!assetsPanel)
            return Editor::RefuseRequest("Assets panel not found");
        const std::string name = ctx.params.value("name", std::string{});
        if (name.empty())
            return Editor::RefuseRequest("Provide smart folder name");
        if (!assetsPanel->SelectSmartFolderByName(name))
            return Editor::RefuseRequest("Smart folder not found: " + name);
        return json{{"ok", true},
                    {"name", name},
                    {"itemCount", assetsPanel->GetVisibleAssetCount()},
                    {"directory", assetsPanel->GetVisibleAssetDirectory().string()}}; });

    server.RegisterHandler("select_asset", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string requestedPath = ctx.params.value("path", std::string{});
        if (requestedPath.empty())
            return Editor::RefuseRequest("Provide path");
        const std::filesystem::path assetPath =
            Editor::ResolveOpenAssetPath(EngineCore::GetInstance().GetAssetManager(), requestedPath);
        if (assetPath.empty())
            return Editor::RefuseRequest("Cannot resolve asset path: " + requestedPath);

        auto* assetsPanel = FindPanel<AssetsPanel>(app.m_PanelStorage);
        if (!assetsPanel)
            return Editor::RefuseRequest("Assets panel not found");

        if (app.m_PanelManager && !app.m_Windows.empty())
        {
            auto* window = app.m_Windows[0].get();
            app.m_PanelManager->ShowOrActivatePanel(window, EditorPanelIds::Assets, EditorPanelIds::SceneView);
            app.m_PanelManager->ShowOrActivatePanel(window, EditorPanelIds::AssetView, EditorPanelIds::Assets);
        }

        const bool silent = ctx.params.value("silent", false);
        if (silent)
            assetsPanel->NavigateToAndSelectAssetSilent(assetPath);
        else
            assetsPanel->NavigateToAndSelectAsset(assetPath);

        json result{{"ok", true}, {"path", assetPath.string()}};
        if (auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage))
        {
            assetViewPanel->SetAssetPreview(assetPath, true);
            const auto preview = assetViewPanel->GetPreviewDebugState();
            result["assetView"] = {
                {"previewPath", preview.previewPath},
                {"isVideoPreview", preview.isVideoPreview},
                {"videoPlayerLoaded", preview.videoPlayerLoaded},
                {"videoTextureUploaded", preview.videoTextureUploaded},
                {"videoNeedsBackgroundBind", preview.videoNeedsBackgroundBind},
                {"videoTextureWidth", preview.videoTextureWidth},
                {"videoTextureHeight", preview.videoTextureHeight},
                {"videoTextureResourceName", preview.videoTextureResourceName},
            };
        }

        return result; });

    server.RegisterHandler("get_asset_preview", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* assetViewPanel = FindPanel<AssetViewPanel>(app.m_PanelStorage);
        if (!assetViewPanel)
            return Editor::RefuseRequest("Asset View panel not found");

        const auto preview = assetViewPanel->GetPreviewDebugState();
        return json{
            {"panelId", EditorPanelIds::AssetView},
            {"previewElementId", "asset-view-preview-image"},
            {"previewPath", preview.previewPath},
            {"previewEnabled", preview.previewEnabled},
            {"isVideoPreview", preview.isVideoPreview},
            {"videoPlayerLoaded", preview.videoPlayerLoaded},
            {"videoTextureUploaded", preview.videoTextureUploaded},
            {"videoNeedsBackgroundBind", preview.videoNeedsBackgroundBind},
            {"videoTextureWidth", preview.videoTextureWidth},
            {"videoTextureHeight", preview.videoTextureHeight},
            {"videoTextureResourceName", preview.videoTextureResourceName},
            {"displayedEngineResource", preview.displayedEngineResource},
        }; });

    // Movie recording automation: drives GameViewController's recording
    // directly (the panel UI flow is interaction-heavy and its embedded
    // variant swallows failure statuses). action=start|stop|status.
    // The debug recording path bypasses MovieRecorderPanel's normal start/stop
    // callbacks, so retain the requested path and publish it when stopping.
    auto debugMoviePath = std::make_shared<std::filesystem::path>();
    server.RegisterHandler("movie_record", [&app, debugMoviePath](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->gameView)
            return Editor::RefuseRequest("No game view available");
        auto* gameView = app.m_Windows[0]->gameView.get();

        const std::string action = ctx.params.value("action", "status");
        if (action == "start")
        {
            const std::string path = ctx.params.value("path", std::string{});
            if (path.empty())
                return Editor::RefuseRequest("Provide 'path'");
            Video::VideoWriterOptions options{};
            options.path = path;
            options.width = ctx.params.value("width", 1280u);
            options.height = ctx.params.value("height", 720u);
            options.fps = ctx.params.value("fps", 60.0);
            options.codec = Video::GuessCodecForMoviePath(options.path);
            options.requireHardwareAcceleration = false;
            const std::string hdr = ctx.params.value("hdr", std::string("off"));
            if (hdr == "pq" || hdr == "hdr10" || hdr == "pq10")
            {
                options.hdrMode = Video::VideoHdrMode::HDR10_PQ;
                options.codec = Video::VideoCodec::HEVC;
            }
            else if (hdr == "hlg")
            {
                options.hdrMode = Video::VideoHdrMode::HLG;
                options.codec = Video::VideoCodec::HEVC;
            }
            const uint64_t frameLimit = ctx.params.value("frames", 0ull);
            std::string error;
            if (!gameView->GetMovieCapture().StartRecording(options, frameLimit, &error))
                return Editor::RefuseRequest(error.empty() ? "StartRecording failed" : error);
            *debugMoviePath = path;
            return json{{"ok", true}, {"recording", true}};
        }
        if (action == "stop")
        {
            gameView->GetMovieCapture().StopRecording("Recording stopped via debug server.",
                                         /*waitForFinalization=*/true);
            const auto completedPath = *debugMoviePath;
            debugMoviePath->clear();
            if (!completedPath.empty() && app.m_MovieRecorder)
                app.m_MovieRecorder->NotifyExternalRecordingFinished(completedPath);
            return json{{"ok", true}, {"recording", gameView->GetMovieCapture().IsRecording()},
                        {"framesWritten", gameView->GetMovieCapture().GetFramesWritten()}};
        }
        return json{{"recording", gameView->GetMovieCapture().IsRecording()},
                    {"framesWritten", gameView->GetMovieCapture().GetFramesWritten()},
                    {"width", gameView->GetMovieCapture().GetWidth()},
                    {"height", gameView->GetMovieCapture().GetHeight()}}; });

    // 16. set_scene_tool
    // Switch the active scene view tool.
    server.RegisterHandler("set_scene_tool", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string toolName = ctx.params.value("tool", "");
        if (toolName.empty())
            return Editor::RefuseRequest("Provide 'tool' name: selection, transform, navgrid, or a tool strip entry's id (spline, terrainBrush, markups)");

        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* ctrl = app.m_Windows[0]->scene.get();

        using TK = SceneViewController::ToolKind;
        // The built-in kinds by name; any other name is a tool strip entry's id.
        if (toolName == "selection")           ctrl->SetActiveTool(TK::Selection);
        else if (toolName == "transform")      ctrl->SetActiveTool(TK::Transform);
        else if (toolName == "navgrid")        ctrl->SetActiveTool(TK::NavGridBrush);
        else if (!ctrl->SetActiveRegisteredTool(toolName))
            return Editor::RefuseRequest(
                Editor::RegisteredToolRefusal(Editor::SceneViewToolStripRegistry::Get(), toolName, &ctrl->GetWorld()));
        return json{{"ok", true}, {"tool", toolName}}; });

    // 16b. set_terrain_brush — drive brush radius/strength/mode from a stroke harness.
    // Params (all optional): radius (world m), strength, mode ("raise"|"lower"|"paint").
    server.RegisterHandler("set_terrain_brush", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* brush = dynamic_cast<Editor::SceneTools::TerrainBrushTool*>(
            app.m_Windows[0]->scene->GetRegisteredTool("terrainBrush"));
        if (!brush)
            return Editor::RefuseRequest("No terrain brush tool");

        if (ctx.params.contains("radius"))
            brush->SetRadius(ctx.params["radius"].get<float>());
        if (ctx.params.contains("strength"))
            brush->SetStrength(ctx.params["strength"].get<float>());
        if (ctx.params.contains("mode"))
        {
            using Mode = GameEngine::Editor::SceneTools::TerrainBrushMode;
            const std::string m = ctx.params["mode"].get<std::string>();
            if (m == "raise")      brush->SetMode(Mode::Raise);
            else if (m == "lower") brush->SetMode(Mode::Lower);
            else if (m == "paint") brush->SetMode(Mode::Paint);
            else return Editor::RefuseRequest("mode must be raise|lower|paint");
        }
        return json{{"ok", true}, {"radius", brush->GetRadius()}, {"strength", brush->GetStrength()}}; });

    // 16c. set_gizmos_visibility — show or hide the scene view's gizmo overlays so a
    // screenshot shows the scene rather than the editor's light rings and transform
    // handles.
    // Params (all optional bools): "all" is the master gate — "light", "transform" and
    // "markups" only take effect while it is true. An omitted group keeps its current state, so
    // a request with no params reports the state without changing it.
    // Writes every controller in the main window: the render coordinator copies this
    // state from the active viewport slot onto the others each frame, so writing only
    // the main-slot controller would be reverted on the next frame in quad view.
    server.RegisterHandler("set_gizmos_visibility", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");

        // Name the offending group: an uncaught type_error reports only
        // "type must be boolean", which does not say which one was wrong.
        for (const char* key : {"all", "light", "transform", "markups"})
        {
            if (ctx.params.contains(key) && !ctx.params[key].is_boolean())
                return Editor::RefuseRequest(std::string("'") + key + "' must be a boolean");
        }

        Editor::SceneViewRenderCoordinator::ForEachController(
            app.m_Windows[0].get(),
            [&ctx](SceneViewController* controller)
            {
                if (ctx.params.contains("all"))
                    controller->SetGizmosVisible(ctx.params["all"].get<bool>());
                if (ctx.params.contains("light"))
                    controller->SetLightGizmosVisible(ctx.params["light"].get<bool>());
                if (ctx.params.contains("transform"))
                    controller->SetTransformGizmosVisible(ctx.params["transform"].get<bool>());
                if (ctx.params.contains("markups"))
                    controller->SetMarkupGizmosVisible(ctx.params["markups"].get<bool>());
            });

        const auto* ctrl = app.m_Windows[0]->scene.get();
        return json{{"ok", true},
                    {"all", ctrl->AreGizmosVisible()},
                    {"light", ctrl->AreLightGizmosVisible()},
                    {"transform", ctrl->AreTransformGizmosVisible()},
                    {"markups", ctrl->AreMarkupGizmosVisible()}}; });

    // 17. get_scene_tool
    // Query the currently active scene view tool.
    server.RegisterHandler("get_scene_tool", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* ctrl = app.m_Windows[0]->scene.get();

        using TK = SceneViewController::ToolKind;
        std::string name = "unknown";
        switch (ctrl->GetActiveToolKind())
        {
            case TK::Selection:     name = "selection"; break;
            case TK::Transform:     name = "transform"; break;
            case TK::NavGridBrush:  name = "navgrid"; break;
            case TK::Registered:    name = std::string(ctrl->GetActiveRegisteredToolId()); break;
        }
        return json{{"tool", name}}; });

    // ── Render Graph introspection ──────────────────────────────────────────────
    //
    // These read the MAIN window's live RGFrame at request time (see the
    // MainRgFrame helpers above). The frame holds the previous frame's compiled
    // graph — a stable snapshot, since the next BeginFrame (which clears it)
    // runs later in the loop. Per-pass GPU timings are present only while
    // profiling is armed (Visual Profiler / Render Graph panel Profile button).
    static constexpr const char* kRgNoFrameMsg =
        "No live render graph (headless, or no frame rendered yet)";

    // Build a name -> {cpuMs, gpuSpanMs} overlay from the last resolved timings.
    // See get_gpu_profiler's timingSemantics for what the GPU value measures.
    auto rgTimingMap = [](RG::RGFrame* frame) {
        std::unordered_map<std::string, std::pair<double, double>> m;
        if (frame)
            for (const auto& t : frame->LastFrameTimings())
                m[t.Name] = {t.CpuMs, t.GpuSpanMs};
        return m;
    };

    // 16. get_render_graph_overview
    server.RegisterHandler("get_render_graph_overview", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const RG::RGFrame::FrameStats& s = frame->Stats();

        size_t transient = 0, persistent = 0, imported = 0;
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            if (!g.IsResourceUsed(r)) continue;
            if (g.IsImported(r)) ++persistent;
            else if (g.IsExternal(r)) ++imported;
            else ++transient;
        }

        json result;
        result["passCount"] = g.ScheduledOrder().size();
        result["declaredPassCount"] = g.PassCount();
        result["culledPassCount"] = g.CulledPassCount();
        result["resourceCount"] = resCount;
        result["barrierCount"] = g.BarrierCount();
        result["barrierBatches"] = s.BarrierBatchesEmitted;
        result["barriersHoisted"] = s.BarriersHoisted;
        result["crossQueueTransitions"] = g.CrossQueueTransitionsConsidered();
        result["crossQueueRelocationsDeclined"] = g.CrossQueueRelocationsDeclined();
        result["renderPassesBegun"] = s.RenderPassesBegun;
        result["submissionCount"] = s.SubmissionsMade;
        result["transientCount"] = transient;
        result["persistentCount"] = persistent;
        result["importedCount"] = imported;
        result["workingSetTransitions"] = g.WorkingSetTransitions();
        result["profilingEnabled"] = frame->ProfilingEnabled();
        result["frameIndex"] = frame->FrameIndex();
        // Render pipeline compiles so far: a count that climbs every frame is a pipeline compiled
        // every frame.
        if (const Engine::Renderer::RenderServices* rs = EngineCore::GetInstance().GetRenderServices())
            result["pipelineCompiles"] = rs->Spine().LastCompileReport().Compiles;
        return result; });

    // 17. get_render_graph_passes
    server.RegisterHandler("get_render_graph_passes", [&app, rgTimingMap](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        const auto timings = rgTimingMap(frame);
        const bool includeAccesses = ctx.params.value("includeAccesses", false);

        json passes = json::array();
        uint32_t idx = 0;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* name = g.PassName(p);
            json jp;
            jp["id"] = p;
            jp["scheduledIndex"] = idx++;
            jp["name"] = name ? name : "(unnamed)";
            jp["queue"] = RgQueueName(g.PassQueue(p));
            jp["phase"] = g.PassPhase(p);
            jp["level"] = g.Level(p);
            jp["barrierCount"] = RgBarrierCountForPass(g, p);
            if (name)
            {
                auto it = timings.find(name);
                if (it != timings.end())
                {
                    jp["cpuMs"] = it->second.first;
                    jp["gpuSpanMs"] = it->second.second;
                }
            }
            if (includeAccesses)
            {
                json accesses = json::array();
                for (const RG::RGAccessRecord& a : g.Accesses())
                {
                    if (a.Pass != p) continue;
                    const char* rn = g.ResourceName(a.Resource);
                    accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                        {"access", RgAccessName(a.Access)},
                                        {"write", RG::IsWrite(a.Access)}});
                }
                jp["accesses"] = std::move(accesses);
            }
            passes.push_back(std::move(jp));
        }
        return json{{"passCount", passes.size()},
                    {"profilingEnabled", frame->ProfilingEnabled()},
                    {"passes", std::move(passes)}}; });

    // 18. get_render_graph_resources
    server.RegisterHandler("get_render_graph_resources", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();

        const Editor::RenderGraphResourceFilter filter(ctx.params);
        if (!filter.Error().empty())
            return Editor::RefuseRequest(filter.Error());

        json resources = json::array();
        const size_t resCount = g.ResourceCount();
        for (RG::RGResourceId r = 0; r < resCount; ++r)
        {
            const RG::RGResourceDesc& d = g.ResourceDesc(r);
            const bool used = g.IsResourceUsed(r);
            const char* kind = d.Kind == RG::RGResourceKind::Buffer                  ? "Buffer"
                               : d.Kind == RG::RGResourceKind::AccelerationStructure ? "AccelerationStructure"
                                                                                       : "Texture";
            const char* lifetime = RgResourceLifetimeName(*frame, r);
            if (!filter.Accepts(kind, lifetime, used))
                continue;

            const char* name = g.ResourceName(r);
            json jr;
            jr["id"] = r;
            jr["name"] = name ? name : "(unnamed)";
            jr["kind"] = kind;
            jr["used"] = used;
            jr["external"] = g.IsExternal(r);
            jr["imported"] = g.IsImported(r);
            jr["lifetime"] = lifetime;
            if (d.Kind == RG::RGResourceKind::Buffer)
            {
                jr["sizeBytes"] = d.SizeBytes;
            }
            else if (d.Kind == RG::RGResourceKind::Texture)
            {
                jr["width"] = d.Width;
                jr["height"] = d.Height;
                jr["format"] = d.Format;
                jr["mipLevels"] = d.MipLevels;
                jr["arrayLayers"] = d.ArrayLayers;
                jr["sampleCount"] = d.SampleCount;
            }
            resources.push_back(std::move(jr));
        }
        // totalResourceCount is the unfiltered size, so a caller can see that a
        // filter applied rather than guessing from the count alone.
        return json{{"resourceCount", resources.size()},
                    {"totalResourceCount", resCount},
                    {"resources", std::move(resources)}}; });

    // 19. get_render_graph_pass_detail
    server.RegisterHandler("get_render_graph_pass_detail", [&app, rgTimingMap](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const std::string wanted = ctx.params.value("name", std::string());
        if (wanted.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_passes to list names)");
        const RG::RGGraph& g = frame->Graph();

        RG::RGPassId target = RG::kInvalidId;
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* n = g.PassName(p);
            if (n && wanted == n) { target = p; break; }
        }
        if (target == RG::kInvalidId)
            return Editor::RefuseRequest("Pass not in the current scheduled graph");

        json accesses = json::array();
        for (const RG::RGAccessRecord& a : g.Accesses())
        {
            if (a.Pass != target) continue;
            const char* rn = g.ResourceName(a.Resource);
            accesses.push_back({{"resource", rn ? rn : "(resource)"},
                                {"access", RgAccessName(a.Access)},
                                {"write", RG::IsWrite(a.Access)}});
        }

        json barriers = json::array();
        const auto& flat = g.Barriers();
        for (const auto& batch : g.BarrierBatches())
        {
            if (batch.Pass != target) continue;
            for (uint32_t i = 0; i < batch.Count; ++i)
            {
                const uint32_t bi = batch.First + i;
                if (bi >= flat.size()) break;
                const RG::RGBarrier& b = flat[bi];
                const char* rn = g.ResourceName(b.Resource);
                barriers.push_back({{"resource", rn ? rn : "(resource)"},
                                    {"from", RgLayoutName(b.OldLayout)},
                                    {"to", RgLayoutName(b.NewLayout)}});
            }
        }

        json successors = json::array();
        for (RG::RGPassId succ : g.PassSuccessors(target))
        {
            const char* n = g.PassName(succ);
            successors.push_back(n ? n : "(unnamed)");
        }

        json detail;
        detail["name"] = wanted;
        detail["queue"] = RgQueueName(g.PassQueue(target));
        detail["phase"] = g.PassPhase(target);
        detail["level"] = g.Level(target);
        detail["barrierCount"] = RgBarrierCountForPass(g, target);
        detail["accesses"] = std::move(accesses);
        detail["barriers"] = std::move(barriers);
        detail["successors"] = std::move(successors);
        const auto timings = rgTimingMap(frame);
        auto it = timings.find(wanted);
        if (it != timings.end())
        {
            detail["cpuMs"] = it->second.first;
            detail["gpuSpanMs"] = it->second.second;
        }
        return detail; });

    // 20. get_render_graph_dependencies
    server.RegisterHandler("get_render_graph_dependencies", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        json edges = json::array();
        for (RG::RGPassId p : g.ScheduledOrder())
        {
            const char* from = g.PassName(p);
            for (RG::RGPassId succ : g.PassSuccessors(p))
            {
                const char* to = g.PassName(succ);
                edges.push_back({{"from", from ? from : "(unnamed)"},
                                 {"to", to ? to : "(unnamed)"}});
            }
        }
        return json{{"edgeCount", edges.size()}, {"edges", std::move(edges)}}; });

    // 21. get_render_graph_validation — the immediate-mode graph has no separate
    // validation layer; surface culled passes (declared but never run) and their
    // reason as warnings, which is the actionable signal here.
    server.RegisterHandler("get_render_graph_validation", [&app](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);
        const RG::RGGraph& g = frame->Graph();
        auto reasonName = [](RG::RGCullReason r) -> const char* {
            switch (r)
            {
            case RG::RGCullReason::NotCulled:      return "NotCulled";
            case RG::RGCullReason::NoConsumer:     return "NoConsumer";
            case RG::RGCullReason::ProducerCulled: return "ProducerCulled";
            case RG::RGCullReason::Cycle:           return "Cycle";
            }
            return "?";
        };
        json issues = json::array();
        const size_t passCount = g.PassCount();
        for (RG::RGPassId p = 0; p < passCount; ++p)
        {
            if (!g.IsCulled(p)) continue;
            const char* n = g.PassName(p);
            issues.push_back({{"pass", n ? n : "(unnamed)"},
                              {"severity", "warning"},
                              {"reason", reasonName(g.CullReason(p))}});
        }
        return json{{"errorCount", 0},
                    {"warningCount", issues.size()},
                    {"passIssues", std::move(issues)}}; });

    // 22. get_camera — read the selected view's camera and projection.
    server.RegisterHandler("get_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string view = ctx.params.value("view", std::string("scene"));
        if (view != "scene" && view != "game")
            return Editor::RefuseRequest("view must be 'scene' or 'game'");
        if (view == "game")
        {
            if (app.m_Windows.empty() || !app.m_Windows[0]->gameView)
                return Editor::RefuseRequest("No game view");
            const auto& game = *app.m_Windows[0]->gameView;
            const auto& camera = game.GetDeclaredCamera();
            if (!camera)
                return Editor::RefuseRequest("No declared game camera frame");
            auto result = Editor::DescribeGameViewCamera(camera->Data, camera->ExposureMode,
                                                         camera->Width, camera->Height);
            result["view"] = "game";
            result["viewId"] = static_cast<uint32_t>(game.GetViewId());
            result["source"] = "last_declared";
            result["frameIndex"] = camera->FrameIndex;
            result["waitingForExtraction"] = game.IsWaitingForExtraction();
            if (camera->ExposureMode == Components::ExposureMode::Auto)
            {
                if (auto* services = EngineCore::GetInstance().GetRenderServices())
                {
                    if (auto* readback = services->GetFeature<Engine::Renderer::ExposureReadbackFeature>())
                    {
                        float scale = 0.0f;
                        uint64_t frameIndex = 0;
                        if (readback->TryResolveAdaptedExposure(game.GetViewId(), scale, &frameIndex))
                            result["exposure"]["metered"] = Editor::DescribeMeteredExposure(scale, frameIndex);
                    }
                }
            }
            return result;
        }
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        // Null while no frame has been recorded, never a stand-in value: a
        // fabricated 60 degrees is the guess this field exists to retire.
        json projection = json(nullptr);
        float proj[16] = {};
        uint32_t viewW = 0;
        uint32_t viewH = 0;
        if (scene->GetLastFrameProjection(proj, viewW, viewH))
            projection = Editor::DescribeCameraProjection(proj, viewW, viewH);

        // is2D separates the two ways a projection turns orthographic: 2D mode,
        // which also makes the view ignore the 3D pose reported here, and a 3D
        // orthographic view, which does not. The projection type alone conflates
        // them, and the difference decides whether the pose above means anything.
        return json{
            {"position", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance},
            {"is2D", pose.Is2D},
            {"projection", std::move(projection)}
        }; });

    // 23. set_camera — set the editor scene view camera pose.
    // Accepts any combination of: position [x,y,z], yawDeg, pitchDeg, distance.
    // Omitted fields keep their current values.
    server.RegisterHandler("set_scene_view_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        // Scene View 2D/3D mode. A 2D (orthographic) scene view ignores the 3D
        // camera pose, which silently breaks camera-driven automation — the
        // integration harness forces 3D before framing screenshots.
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");
        auto* scene = app.m_Windows[0]->scene.get();
        const bool was2D = scene->Is2DMode();
        if (ctx.params.contains("is2D"))
        {
            const bool want2D = ctx.params["is2D"].get<bool>();
            if (want2D != was2D)
                scene->Set2DMode(want2D);
        }
        return json{{"ok", true}, {"was2D", was2D}, {"is2D", scene->Is2DMode()}}; });

    server.RegisterHandler("set_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        json warnings;
        if (json error = ValidateCameraParams(ctx.params,
                                              {"position"},
                                              {"yawDeg", "pitchDeg", "distance"},
                                              {},
                                              warnings);
            !error.is_null())
            return error;

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        if (ctx.params.contains("position"))
        {
            auto& p = ctx.params["position"];
            pose.Pos[0] = p[0].get<float>();
            pose.Pos[1] = p[1].get<float>();
            pose.Pos[2] = p[2].get<float>();
        }
        if (ctx.params.contains("yawDeg"))   pose.YawDeg   = ctx.params["yawDeg"].get<float>();
        if (ctx.params.contains("pitchDeg")) pose.PitchDeg = ctx.params["pitchDeg"].get<float>();
        if (ctx.params.contains("distance")) pose.Distance = ctx.params["distance"].get<float>();

        scene->SetCameraPose(pose);

        // Push the new angles into the SceneViewPanel as well. Each frame
        // RecordWindowWorldViews overwrites the controller's yaw/pitch from
        // the panel via SetCameraAnglesDeg — without this sync, our MCP-set
        // angles would be clobbered on the next frame.
        if (auto* root = app.m_Windows[0]->ui->GetRootElement())
            if (auto* mount = dynamic_cast<Mount*>(root->FindById(EditorPanelIds::MountSceneView)))
                if (auto* svPanel = dynamic_cast<SceneViewPanel*>(mount->GetTarget()))
                    svPanel->SetYawPitch(pose.YawDeg, pose.PitchDeg);

        json response{
            {"ok", true},
            {"position", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance}
        };
        if (!warnings.empty())
            response["warnings"] = warnings;
        return response; });

    // 24. look_at — point the camera at a world position or entity from a
    // given direction and distance. Computes yaw/pitch from the direction
    // vector and positions the camera at target - dir * distance. For
    // entities the target is the subtree bounds union and, unless the caller
    // passes an explicit distance, the distance is fitted to those bounds.
    server.RegisterHandler("look_at", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        json warnings;
        if (json error = ValidateCameraParams(ctx.params,
                                              {"target", "direction"},
                                              {"distance"},
                                              {"entityId"},
                                              warnings);
            !error.is_null())
            return error;

        float target[3] = {0.0f, 0.0f, 0.0f};
        bool framedBounds = false;
        float autoDistance = 0.0f; // subtree-derived; used when the caller omits distance

        // Target can be explicit coordinates or derived from an entity.
        if (ctx.params.contains("entityId"))
        {
            uint32_t entityId = ctx.params["entityId"].get<uint32_t>();
            auto* world = EngineCore::GetInstance().GetPrimaryWorld();
            if (!world)
                return Editor::RefuseRequest("No world available");

            ECS::EntityHandle entity(entityId);
            if (!world->IsValid(entity))
                return Editor::RefuseRequest("Invalid entity");

            // Frame the subtree bounds union: a large model's Transform origin
            // can sit inside its own geometry, and a fixed default distance
            // used to place the camera inside the mesh.
            Editor::SubtreeWorldBounds subtree;
            if (Editor::ComputeSubtreeWorldBounds(*world, entity, subtree) && subtree.HasBounds)
            {
                const Mathematics::Vector3 center = subtree.Center();
                target[0] = center.x;
                target[1] = center.y;
                target[2] = center.z;
                autoDistance = std::max(subtree.Radius() * 1.8f, 0.5f);
                framedBounds = true;
            }
            else if (auto* xf = world->GetComponent<Components::Transform>(entity))
            {
                auto pos = xf->GetPosition();
                target[0] = pos.x;
                target[1] = pos.y;
                target[2] = pos.z;
            }
        }
        else if (ctx.params.contains("target"))
        {
            auto& t = ctx.params["target"];
            target[0] = t[0].get<float>();
            target[1] = t[1].get<float>();
            target[2] = t[2].get<float>();
        }
        else
        {
            return Editor::RefuseRequest("Provide 'entityId' or 'target' [x,y,z]");
        }

        // Explicit distance always wins; otherwise fit the framed bounds, or
        // fall back to the historical default when no bounds exist.
        float distance = ctx.params.value("distance", framedBounds ? autoDistance : 5.0f);

        // Direction: default to looking from front-right-above if not specified.
        float dirX = 1.0f, dirY = -0.5f, dirZ = 1.0f;
        if (ctx.params.contains("direction"))
        {
            auto& d = ctx.params["direction"];
            dirX = d[0].get<float>();
            dirY = d[1].get<float>();
            dirZ = d[2].get<float>();
        }

        SceneViewCameraPose pose{};
        if (!Editor::ComputeLookAtPose(Mathematics::Vector3(target[0], target[1], target[2]),
                                       Mathematics::Vector3(dirX, dirY, dirZ), distance, pose))
            return Editor::RefuseRequest("Direction vector is zero");
        const float camX = pose.Pos[0];
        const float camY = pose.Pos[1];
        const float camZ = pose.Pos[2];
        const float yawDeg = pose.YawDeg;
        const float pitchDeg = pose.PitchDeg;

        ApplyMainSceneViewPose(app, pose);

        json response{
            {"ok", true},
            {"position", {camX, camY, camZ}},
            {"target", {target[0], target[1], target[2]}},
            {"yawDeg", yawDeg},
            {"pitchDeg", pitchDeg},
            {"distance", distance},
            {"framedBounds", framedBounds}
        };
        if (!warnings.empty())
            response["warnings"] = warnings;
        return response; });

    // 25. move_camera — smoothly animate the camera to a target pose.
    // Same parameters as set_camera, plus optional durationSec (default 0.7).
    server.RegisterHandler("move_camera", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view");

        auto* scene = app.m_Windows[0]->scene.get();
        auto pose = scene->GetCameraPose();

        if (ctx.params.contains("position") && ctx.params["position"].is_array())
        {
            auto& p = ctx.params["position"];
            if (p.size() >= 3)
            {
                pose.Pos[0] = p[0].get<float>();
                pose.Pos[1] = p[1].get<float>();
                pose.Pos[2] = p[2].get<float>();
            }
        }
        if (ctx.params.contains("yawDeg"))   pose.YawDeg   = ctx.params["yawDeg"].get<float>();
        if (ctx.params.contains("pitchDeg")) pose.PitchDeg = ctx.params["pitchDeg"].get<float>();
        if (ctx.params.contains("distance")) pose.Distance = ctx.params["distance"].get<float>();

        float duration = ctx.params.value("durationSec", 0.7f);
        scene->StartCameraTween(pose, duration);

        return json{
            {"ok", true},
            {"tweening", true},
            {"targetPosition", {pose.Pos[0], pose.Pos[1], pose.Pos[2]}},
            {"yawDeg", pose.YawDeg},
            {"pitchDeg", pose.PitchDeg},
            {"distance", pose.Distance},
            {"durationSec", duration}
        }; });

    // 26. capture_resource
    // Capture a named render-graph resource — a texture as a PNG, a buffer as
    // bytes — from the live immediate-mode frame.
    //
    // The readback is declared into the frame at the same seam take_screenshot
    // uses: RunRenderCallbacks fires after every normal pass is declared and
    // before Execute, so the whole frame's resource table is visible and the
    // capture reads each resource's FINAL contents for the frame.
    //
    // Correctness rests on the declared CopySrc read, not on pass ordering: the
    // RAW edge from the last writer is what sequences the copy after the
    // producer, and PreventCulling (inside the readback helper) keeps a producer
    // alive that nothing else consumes — capturing a resource is what makes it
    // live, so an otherwise-culled target still realizes and runs.
    server.RegisterHandler("capture_resource", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string resourceName = ctx.params.value("name", "");
        if (resourceName.empty())
            return Editor::RefuseRequest("Provide a 'name' parameter (use get_render_graph_resources to list names)");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        struct CaptureState
        {
            std::string requestedName;
            std::string resolvedName;
            float rangeMin = 0.0f;
            float rangeMax = 1.0f;
            uint32_t mip = 0;
            uint32_t layer = 0;
            uint64_t bufferOffset = 0;
            uint64_t bufferBytes = 0;
            std::string decode;
            uint32_t srcX = 0, srcY = 0, regionW = 0, regionH = 0;
            // Return the texels as they sit in memory instead of an 8-bit PNG.
            // An HDR intermediate does not survive the PNG path: values outside
            // [rangeMin,rangeMax] CLAMP, so a bright region reads as "identical"
            // in two captures that actually differ. Attribution needs the numbers.
            bool raw = false;

            bool isTexture = true;
            Rendering::TextureFormat format = Rendering::TextureFormat::RGBA8_UNORM;
            uint64_t frameIndex = 0;
            bool writtenThisFrame = false;
            std::shared_ptr<Rendering::RGReadbackTicket> textureTicket;
            std::shared_ptr<Rendering::RGBufferReadbackTicket> bufferTicket;
            json error;       // set by the render callback; ends the poll
            bool resolved = false;
            uint32_t polls = 0;
            JobSystem::TaskHandle encodeJob;
        };
        auto state = std::make_shared<CaptureState>();
        state->requestedName = resourceName;
        state->rangeMin = ctx.params.value("rangeMin", 0.0f);
        state->rangeMax = ctx.params.value("rangeMax", 1.0f);
        state->mip = ctx.params.value("mip", 0u);
        state->layer = ctx.params.value("layer", 0u);
        state->bufferOffset = ctx.params.value("offset", 0ull);
        state->bufferBytes = ctx.params.value("byteCount", 0ull);
        state->decode = ctx.params.value("decode", std::string("hex"));
        state->srcX = ctx.params.value("x", 0u);
        state->srcY = ctx.params.value("y", 0u);
        state->regionW = ctx.params.value("w", 0u);
        state->regionH = ctx.params.value("h", 0u);
        state->raw = ctx.params.value("raw", false);

        // Declaration phase: runs on the render thread at the RenderGraph capture seam.
        server.EnqueueRenderCallback([&app, state]()
        {
            if (state->resolved || app.m_Windows.empty())
                return;

            auto* wndCtx = app.m_Windows[0].get();
            auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
            auto* frame = wndCtx ? wndCtx->rg2Capture.frame : nullptr;
            if (!device || !frame || wndCtx->rg2Capture.frameIndex != frame->FrameIndex())
                return; // no live frame this tick — the poll retries on the next one

            state->resolved = true;
            state->frameIndex = frame->FrameIndex();

            CaptureResourceMatch match;
            std::vector<std::string> candidates;
            if (!ResolveCaptureResource(*frame, state->requestedName, match, candidates))
            {
                if (candidates.size() > kMaxReportedCandidates)
                    candidates.resize(kMaxReportedCandidates);
                state->error = Editor::RefuseRequest("No unique render-graph resource matches '" + state->requestedName +
                                                         "' this frame (exact name, else a unique case-insensitive substring)",
                                                     json{{"candidates", candidates}});
                return;
            }
            state->resolvedName = match.Name;
            state->isTexture = match.IsTexture;

            // A persistent/imported resource that NO pass writes this frame still
            // has contents — whatever the last frame that did write it left in the
            // pool. Copying that back succeeds and looks like a fresh capture, so
            // a disabled feature reads as "unchanged" rather than "not produced".
            // Report the distinction instead of letting the caller infer it.
            // Our own readback declares a read only, so this never counts itself.
            state->writtenThisFrame = false;
            for (const auto& a : frame->Graph().Accesses())
            {
                if (a.Resource == match.Id && Rendering::RenderGraph::IsWrite(a.Access))
                {
                    state->writtenThisFrame = true;
                    break;
                }
            }

            const auto& desc = frame->Graph().ResourceDesc(match.Id);

            if (match.IsTexture)
            {
                if (desc.SampleCount > 1)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is multisampled (" +
                                                             std::to_string(desc.SampleCount) +
                                                             " samples) — copy-to-buffer is invalid on an MSAA image; "
                                                             "capture its resolve target instead",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const auto fmt = static_cast<Rendering::TextureFormat>(desc.Format);
                if (Rendering::BytesPerPixel(fmt) == 0)
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' has no linear texel size (" +
                                                             std::string(Rendering::ToString(fmt)) +
                                                             ") — block-compressed resources cannot be read back",
                                                         json{{"resourceName", match.Name}});
                    return;
                }
                const uint32_t mipCount = desc.MipLevels > 0 ? desc.MipLevels : 1u;
                const uint32_t layerCount = desc.ArrayLayers > 0 ? desc.ArrayLayers : 1u;
                if (state->mip >= mipCount || state->layer >= layerCount)
                {
                    state->error = Editor::RefuseRequest("subresource out of range for '" + match.Name + "'",
                                                         json{{"resourceName", match.Name},
                                                              {"mipLevels", mipCount},
                                                              {"arrayLayers", layerCount}});
                    return;
                }
                state->format = fmt;

                Rendering::RenderGraph::RGTexture tex = frame->FindTexture(match.Name.c_str());
                if (!tex.IsValid())
                {
                    state->error = Editor::RefuseRequest("'" + match.Name + "' is not a texture this frame");
                    return;
                }
                // A raw capture ships texels verbatim, so it must be a region: a
                // full 3072x902 RGBA16F target is 22 MB of bytes (30 MB base64)
                // and would stall the debug channel.
                if (state->raw)
                {
                    const uint32_t mipW = std::max(1u, desc.Width >> state->mip);
                    const uint32_t mipH = std::max(1u, desc.Height >> state->mip);
                    const uint32_t w = state->regionW != 0 ? state->regionW : mipW;
                    const uint32_t h = state->regionH != 0 ? state->regionH : mipH;
                    const uint64_t bytes = static_cast<uint64_t>(w) * h * Rendering::BytesPerPixel(fmt);
                    if (bytes > kMaxRawCaptureBytes)
                    {
                        state->error = Editor::RefuseRequest("raw capture of '" + match.Name + "' would be " +
                                                                 std::to_string(bytes) + " bytes (limit " +
                                                                 std::to_string(kMaxRawCaptureBytes) +
                                                                 ") — narrow it with x/y/w/h",
                                                             json{{"resourceName", match.Name},
                                                                  {"width", mipW},
                                                                  {"height", mipH},
                                                                  {"bytesPerPixel", Rendering::BytesPerPixel(fmt)}});
                        return;
                    }
                }

                state->textureTicket = Rendering::RequestTextureSubresourceReadbackRG(
                    device, *frame, tex, state->mip, state->layer, state->srcX, state->srcY,
                    state->regionW, state->regionH, match.Name.c_str());
                if (!state->textureTicket)
                    state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name +
                                                             "' (region out of bounds?)",
                                                         json{{"resourceName", match.Name}});
                return;
            }

            // ---- buffer ----
            const uint64_t size = desc.SizeBytes;
            if (size == 0 || state->bufferOffset >= size)
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' offset " +
                                                         std::to_string(state->bufferOffset) +
                                                         " is outside its " + std::to_string(size) + " bytes",
                                                     json{{"resourceName", match.Name}});
                return;
            }
            uint64_t want = state->bufferBytes != 0 ? state->bufferBytes : size - state->bufferOffset;
            want = std::min(want, size - state->bufferOffset);
            want = std::min(want, kMaxBufferCaptureBytes);
            state->bufferBytes = want;

            Rendering::RenderGraph::RGBuffer buf = frame->FindBuffer(match.Name.c_str());
            if (!buf.IsValid())
            {
                state->error = Editor::RefuseRequest("'" + match.Name + "' is not a buffer this frame");
                return;
            }
            state->bufferTicket = Rendering::RequestBufferReadbackRG(
                device, *frame, buf, static_cast<size_t>(state->bufferOffset),
                static_cast<size_t>(want), match.Name.c_str());
            if (!state->bufferTicket)
                state->error = Editor::RefuseRequest("Failed to declare a readback of '" + match.Name + "'",
                                                     json{{"resourceName", match.Name}});
        });

        // Resolution phase: poll until the declaring frame's copy has completed.
        server.EnqueueDeferredResponse(ctx.id,
            [state, requestId = ctx.id](json& outResult) -> bool
            {
                if (state->encodeJob.IsValid())
                {
                    std::string jobError;
                    if (!Editor::PollHandlerReplyJob(state->encodeJob, outResult, jobError))
                        return false;
                    if (!jobError.empty())
                        outResult = Editor::RefuseRequest("The resource encode job did not complete: " + jobError);
                    return true;
                }
                ++state->polls;
                if (!state->error.is_null())
                {
                    outResult = state->error;
                    return true;
                }
                if (!state->textureTicket && !state->bufferTicket)
                {
                    // The callback needs a live frame; if none arrives the editor
                    // is not driving frames (minimized / paused render loop).
                    constexpr uint32_t kMaxPollsAwaitingFrame = 10;
                    if (state->polls > kMaxPollsAwaitingFrame)
                    {
                        outResult = Editor::RefuseRequest("No render-graph frame was declared while the "
                                                          "capture was pending — is the editor rendering?");
                        return true;
                    }
                    return false;
                }

                constexpr uint32_t kMaxPollsAwaitingGpu = 25;

                if (state->bufferTicket)
                {
                    if (state->bufferTicket->IsConsumed())
                    {
                        outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                        return true;
                    }
                    Rendering::BufferReadbackResult r;
                    if (!state->bufferTicket->TryGet(r))
                    {
                        if (state->polls > kMaxPollsAwaitingGpu)
                        {
                            outResult = Editor::RefuseRequest("Buffer readback never completed");
                            return true;
                        }
                        return false;
                    }
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "buffer"},
                        {"frameIndex", state->frameIndex},
                        {"offset", state->bufferOffset},
                        {"byteCount", r.bytes.size()},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"bytesBase64", Base64Encode(r.bytes.data(), r.bytes.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these bytes are whatever the last frame that "
                            "did write it left in the pool, not a current value";
                    if (state->decode == "float")
                    {
                        std::vector<float> v(std::min(r.bytes.size() / sizeof(float),
                                                      kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(float));
                        outResult["floats"] = v;
                    }
                    else if (state->decode == "uint")
                    {
                        std::vector<uint32_t> v(std::min(r.bytes.size() / sizeof(uint32_t),
                                                         kMaxDecodedElements));
                        std::memcpy(v.data(), r.bytes.data(), v.size() * sizeof(uint32_t));
                        outResult["uints"] = v;
                    }
                    return true;
                }

                if (state->textureTicket->IsConsumed())
                {
                    outResult = Editor::RefuseRequest("Declaring frame was abandoned before submit");
                    return true;
                }
                Rendering::ViewReadbackResult result;
                if (!state->textureTicket->TryGet(result))
                {
                    if (state->polls > kMaxPollsAwaitingGpu)
                    {
                        outResult = Editor::RefuseRequest("Texture readback never completed");
                        return true;
                    }
                    return false;
                }

                if (state->raw)
                {
                    outResult = json{
                        {"resourceName", state->resolvedName},
                        {"requestedName", state->requestedName},
                        {"kind", "texture"},
                        {"encoding", "raw"},
                        {"width", result.width},
                        {"height", result.height},
                        {"x", state->srcX},
                        {"y", state->srcY},
                        {"format", Rendering::ToString(result.format)},
                        {"bytesPerPixel", Rendering::BytesPerPixel(result.format)},
                        {"channels", FormatChannelCount(result.format)},
                        {"mip", state->mip},
                        {"layer", state->layer},
                        {"frameIndex", state->frameIndex},
                        {"writtenThisFrame", state->writtenThisFrame},
                        {"byteCount", result.pixels.size()},
                        {"rawBase64", Base64Encode(result.pixels.data(), result.pixels.size())}};
                    if (!state->writtenThisFrame)
                        outResult["staleWarning"] =
                            "No pass wrote '" + state->resolvedName +
                            "' this frame — these texels are whatever the last frame that "
                            "did write it left in the pool, not a current image";
                    return true;
                }

                json metadata{
                    {"resourceName", state->resolvedName},
                    {"requestedName", state->requestedName},
                    {"kind", "texture"},
                    {"width", result.width},
                    {"height", result.height},
                    {"format", Rendering::ToString(result.format)},
                    {"channels", FormatChannelCount(result.format)},
                    {"mip", state->mip},
                    {"layer", state->layer},
                    {"frameIndex", state->frameIndex},
                    {"writtenThisFrame", state->writtenThisFrame}};
                if (!state->writtenThisFrame)
                    metadata["staleWarning"] =
                        "No pass wrote '" + state->resolvedName +
                        "' this frame — these pixels are whatever the last frame that "
                        "did write it left in the pool, not a current image";
                state->encodeJob = Editor::SubmitHandlerReplyJob(
                    EngineCore::GetInstance().GetJobSystem(), requestId,
                    [readback = std::move(result), rangeMin = state->rangeMin,
                     rangeMax = state->rangeMax, metadata = std::move(metadata)]() mutable
                    {
                        return FinishResourceCaptureResult(std::move(readback), rangeMin, rangeMax,
                                                           std::move(metadata));
                    });
                if (!state->encodeJob.IsValid())
                {
                    outResult = Editor::RefuseRequest(
                        "The job system is shutting down; the resource capture was not encoded");
                    return true;
                }
                return false;
            }, Editor::kScreenshotPollBackstop);

        return EditorDebugServer::DeferredMarker(); });

    // trigger_capture — RenderDoc frame capture via in-app API.
    // Resolves RenderDoc lazily through app so handlers only need to be
    // registered once (RenderDoc is initialized after first few frames).
    server.RegisterHandler("trigger_capture", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        if (!rd || !rd->IsAvailable())
            return Editor::RefuseRequest("RenderDoc not available — the editor is not running under "
                                         "RenderDoc. Relaunch it with launch_editor renderdoc=true, or "
                                         "under 'renderdoccmd capture'.");

        uint32_t countBefore = rd->GetCaptureCount();

        rd->TriggerCapture();

        // RenderDoc publishes the capture only once the .rdc is fully written.
        // The render loop stalls while it serializes, so this costs far fewer
        // frames than the wall clock suggests — a measured 133 MB capture took
        // ~2.4 s yet consumed only tens of frames, landing right on the 30-frame
        // default and timing out intermittently. get_capture_status recovers the
        // path when the budget does run out.
        constexpr int kCapturePollBudgetFrames = 300;

        // The capture completes after the next present. Poll for the new
        // capture file over several frames via the deferred response mechanism.
        auto state = std::make_shared<uint32_t>(countBefore);
        server.EnqueueDeferredResponse(ctx.id,
            [&app, state](json& outResult) -> bool
            {
                auto* rd = app.m_RenderDoc.get();
                if (!rd)
                    return false;
                uint32_t current = rd->GetCaptureCount();
                if (current <= *state)
                    return false; // not ready yet

                std::string path = rd->GetLastCapturePath();
                outResult = {
                    {"captured", true},
                    {"captureIndex", current - 1},
                    {"filePath", path}
                };
                return true;
            },
            kCapturePollBudgetFrames);

        return EditorDebugServer::DeferredMarker(); });

    // get_capture_status — RenderDoc availability plus the newest capture on
    // disk. Answers on the calling frame, so it recovers the .rdc path when a
    // trigger_capture call timed out before RenderDoc finished writing.
    server.RegisterHandler("get_capture_status", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* rd = app.m_RenderDoc.get();
        const bool available = rd && rd->IsAvailable();
        return json{
            {"available", available},
            {"captureCount", available ? rd->GetCaptureCount() : 0u},
            {"lastCapturePath", available ? rd->GetLastCapturePath() : std::string{}}
        }; });

    // get_pass_descriptors — capture descriptor set bindings at draw time for a named pass.
    server.RegisterHandler("get_pass_descriptors", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string passName = ctx.params.value("passName", "");
        if (passName.empty())
            return Editor::RefuseRequest("Provide a 'passName' parameter");

        if (app.m_Windows.empty())
            return Editor::RefuseRequest("No editor window");

        auto* wndCtx = app.m_Windows[0].get();
        auto* device = (wndCtx && wndCtx->renderCtx) ? wndCtx->renderCtx->GetDevice() : nullptr;
        if (!device)
            return Editor::RefuseRequest("No render device");

        // Enable capture for the next frame, filtered to the requested pass.
        device->DebugClearDescriptorCaptures();
        device->DebugSetDescriptorCaptureEnabled(true, passName);

        std::string requestId = ctx.id;
        auto frameCounter = std::make_shared<int>(0);

        server.EnqueueDeferredResponse(requestId,
            [device, passName, frameCounter](json& outResult) -> bool
            {
                ++(*frameCounter);
                // Wait one full frame for the capture to execute.
                if (*frameCounter < 2)
                    return false;

                // Disable capture.
                device->DebugSetDescriptorCaptureEnabled(false, "");

                auto captures = device->DebugGetDescriptorCaptures();

                json draws = json::array();
                for (const auto& entry : captures)
                {
                    json bindings = json::array();
                    for (const auto& b : entry.Bindings)
                    {
                        json bj;
                        bj["binding"] = b.Binding;
                        bj["resourceType"] = b.ResourceType;
                        bj["resourceHandle"] = "0x" + ([](uint64_t v) {
                            char buf[20];
                            snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                            return std::string(buf);
                        })(b.ResourceHandle);
                        if (!b.DebugName.empty())
                            bj["debugName"] = b.DebugName;
                        bindings.push_back(std::move(bj));
                    }

                    json drawEntry;
                    drawEntry["passName"] = entry.PassName;
                    drawEntry["drawIndex"] = entry.DrawIndex;
                    drawEntry["descriptorSet"] = "0x" + ([](uint64_t v) {
                        char buf[20];
                        snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                        return std::string(buf);
                    })(entry.DescriptorSetHandle);
                    drawEntry["setIndex"] = entry.SetIndex;
                    drawEntry["bindings"] = std::move(bindings);
                    draws.push_back(std::move(drawEntry));
                }

                outResult = {
                    {"passName", passName},
                    {"drawCount", draws.size()},
                    {"draws", std::move(draws)}
                };
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });

    // 28. toggle_shadow_debug — toggle cascade color, shadow factor, or PCSS
    // branch classification debug visualization.
    // mode: 0=off, 1=cascade colors, 2=combined factor, 3=PCSS branch
    // 4=base factor, 5=contact factor, 6=contact-only lighting, 7=base-only lighting
    // (green=early-out lit, red=early-out shadowed, blue=full penumbra path).
    // Omit to cycle through modes.
    server.RegisterHandler("toggle_shadow_debug", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        using Mode = Engine::Renderer::ShadowDebugMode;
        Mode mode;
        if (ctx.params.contains("mode"))
        {
            int m = ctx.params["mode"].get<int>();
            mode = static_cast<Mode>(std::clamp(m, 0, static_cast<int>(Mode::Count) - 1));
        }
        else
        {
            // Cycle through all shadow debug views.
            int current = static_cast<int>(feature->GetDebugMode());
            mode = static_cast<Mode>((current + 1) % static_cast<int>(Mode::Count));
        }

        feature->SetDebugMode(mode);

        static const char* kModeNames[] = {"off", "cascade_colors", "shadow_factor", "pcss_branch", "base_shadow_factor", "contact_shadow_factor", "contact_only", "base_only"};
        return json{
            {"ok", true},
            {"mode", static_cast<int>(mode)},
            {"modeName", kModeNames[static_cast<int>(mode)]}
        }; });

    // 29. toggle_shadow_thumbnails — toggle shadow map cascade thumbnails overlay.
    // enable: true/false. Omit to toggle.
    // The overlay pass uses an activation predicate that checks this flag each frame.
    server.RegisterHandler("toggle_shadow_thumbnails", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        bool enable;
        if (ctx.params.contains("enable"))
            enable = ctx.params["enable"].get<bool>();
        else
            enable = !feature->GetShowThumbnails();

        feature->SetShowThumbnails(enable);

        return json{{"ok", true}, {"enabled", enable}}; });

    // 29a2. get_mesh_registry — dump every live MeshGPURegistry entry with its
    // source identity (asset GUID + submesh) and LOD chain. This is the mapping
    // a GPU capture cannot recover on its own: GPUScene mesh-row index -> asset,
    // per-LOD index counts, achieved simplify errors, sloppy flags, and the
    // derived screen-coverage switch points the scatter consumes. Read-only.
    server.RegisterHandler("get_mesh_registry", [](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        const auto& reg = rs->GetMeshGPURegistry();
        // Report the thresholds the scatter actually reads — the uploaded
        // GPUMesh row — rather than re-deriving them here. A second derivation
        // silently stops matching the moment the upload path gains a step.
        const Rendering::GPUScene* scene = reg.GetGPUScene();
        json rows = json::array();
        reg.ForEachKeyedEntry(
            [&rows, scene](const Rendering::MeshGPUKey& key, const Rendering::MeshGPUEntry& e)
            {
                const Rendering::GPUMesh* row = nullptr;
                if (scene && e.gpuMeshIndex != ~0u && e.gpuMeshIndex < scene->GetMeshes().size())
                    row = &scene->GetMeshes()[e.gpuMeshIndex];
                json lods = json::array();
                for (uint32_t k = 0; k < e.lodCount && k < Rendering::MeshGPUEntry::kMaxEntryLODs; ++k)
                {
                    json lod{{"indexCount", e.lodIndexCount[k]},
                             {"firstIndex", e.lodFirstIndex[k]},
                             {"error", e.lodError[k]},
                             {"sloppy", e.lodSloppy[k] != 0}};
                    // Absent when the entry holds no GPU row (nothing uploaded yet).
                    if (row && k < Rendering::kMaxMeshLODs)
                    {
                        lod["threshold"] = row->lodThreshold[k];
                        // SSE-normalized slot (MeshLODThresholds.h): threshold
                        // compares against sse-scaled coverage, and the switch
                        // happens at spherePx = 2 * budgetPx * threshold.
                        lod["sse"] = (row->lodFlags & (1u << k)) != 0u;
                    }
                    lods.push_back(std::move(lod));
                }
                // boundingRadius/maxExtent describe the CULLING envelope, which
                // encloses every admitted LOD. The switch points above are built
                // from the LOD0 reference metric instead, so both are reported:
                // reading a threshold against the envelope would not reproduce it.
                json entry{{"guid", key.assetGuid.ToString()},
                           {"submesh", key.submeshIndex},
                           {"lodCount", e.lodCount},
                           {"authored", e.lodAuthored},
                           {"indexCount", e.indexCount},
                           {"boundingRadius", e.bounds.Radius()},
                           {"maxExtent", 2.0f * std::max({e.bounds.halfExtents.x,
                                                          e.bounds.halfExtents.y,
                                                          e.bounds.halfExtents.z})},
                           {"lodReferenceRadius", e.lodReferenceRadius},
                           {"lodReferenceMaxExtent", e.lodReferenceMaxExtent},
                           {"lods", std::move(lods)}};
                if (row)
                {
                    entry["gpuMeshIndex"] = e.gpuMeshIndex;
                    entry["lodFlags"] = row->lodFlags;
                    // The scatter multiplies its coverage by this to undo the
                    // envelope; 1 on every mesh whose bounds never grew.
                    entry["lodCoverageScale"] = row->lodCoverageScale;
                    entry["tightClass"] =
                        (row->lodFlags & Rendering::kGPUMeshLodTightClassBit) != 0u;
                }
                rows.push_back(std::move(entry));
            });
        // Key iteration order is arbitrary (unordered_map); sort so successive
        // dumps of the same scene diff cleanly.
        std::sort(rows.begin(), rows.end(),
                  [](const json& a, const json& b)
                  {
                      const auto& ga = a.at("guid").get_ref<const std::string&>();
                      const auto& gb = b.at("guid").get_ref<const std::string&>();
                      if (ga != gb)
                          return ga < gb;
                      return a.at("submesh").get<uint32_t>() < b.at("submesh").get<uint32_t>();
                  });
        return json{{"entryCount", rows.size()}, {"entries", std::move(rows)}}; });

    // 29b. set_lod — drive the GPU LOD selection knobs for diagnosing coverage
    // thresholds and simplified-mesh seams.
    // {forceLevel?: int, bias?: float, shadowBias?: float, cullCoverage?: float,
    //  budgetPx?: float, skinnedScale?: float, crossfadeDuration?: float,
    //  mode?: string}.
    //   forceLevel:  0..3 pins every instance to that LOD; -1 (or omit) restores
    //                screen-coverage auto-selection.
    //   bias:        log2 coverage bias; positive keeps more detail, negative
    //                switches to coarser LODs earlier. Applies to the auto path.
    //   shadowBias:  extra log2 bias added on shadow buckets only (cascades,
    //                spot/point/area), so shadow maps can run coarser levels
    //                than the view that casts them. Camera slices — including
    //                the depth prepass — never take it: prepass geometry must
    //                stay identical to the forward pass it primes.
    //                TRAP: the bias multiplies COVERAGE, not the threshold
    //                (draw_command_scatter.comp ge_SelectLOD), so a negative
    //                value scales every switch point by 2^-bias — including
    //                kLodSloppyThresholdCap (MeshLODThresholds.h), the floor
    //                that holds sloppy levels back. Sloppy levels drop whole
    //                regions instead of eroding evenly, and submeshes simplify
    //                independently (ModelAsset::GenerateLODs), so admitting
    //                them earlier in shadow reads as missing caster geometry
    //                and as light leaking between the submeshes of a solid
    //                object. Judge a negative value on the shadow, not on the
    //                lit image the coverage thresholds were tuned against.
    //   cullCoverage: small-object cull threshold (prototype; 0 disables) —
    //                camera-slice instances below this projected coverage drop.
    //   budgetPx:    SSE projected-error budget in render-target pixels
    //                (generated chains); <= 0 disables SSE coarsening.
    //   skinnedScale: budget multiplier for skinned/character chains.
    //   gameViewBudget / sceneViewBudget: per-view-class override of budgetPx,
    //                {"enabled": bool, "percent": 10..400}. Omitted sub-keys keep
    //                their current value. Disabled = that class spends budgetPx
    //                unchanged. Scene View covers every scene viewport.
    //   crossfadeDuration: dithered LOD crossfade length in seconds (0
    //                disables). A camera-slice level change dissolves over
    //                this window instead of popping. Shadow buckets never take
    //                it, for the same reason they never take cullCoverage.
    //                Prepass-safe: a fading instance writes its record PAIR to
    //                the row's TAIL region and no head record, and BOTH raster
    //                passes draw that tail through the same dither, so the
    //                prepass writes depth for exactly the fragments the colour
    //                pass keeps — each level on its own half, neither writing
    //                where the other does.
    //                NOTE: a non-zero duration makes a frame's records depend on
    //                the fade CLOCK, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    //   mode:        selection mapping — "sse", "coverage" (the pre-SSE
    //                reference) or "off" (every mesh draws LOD0). Validated
    //                before any knob is applied, so a typo changes nothing.
    //   hysteresis:  LOD dwell band as a fraction of the switch coverage (0
    //                disables). Gaining detail needs coverage >= threshold*(1+band);
    //                holding or losing it needs only the threshold. Camera slices
    //                only. NOTE: a non-zero band makes selection depend on the
    //                previous frame, so captures stop being reproducible from
    //                pose alone — set 0 before any A/B that assumes they are.
    // Returns the resolved knobs so a caller can A/B a fixed pose across levels.
    server.RegisterHandler("set_lod", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        // Parsed up front: a rejected mode must not leave the other knobs
        // half-applied under an ok:false reply.
        std::optional<Rendering::LodSelectionMode> requestedMode;
        if (ctx.params.contains("mode"))
        {
            Rendering::LodSelectionMode parsed{};
            const std::string mode = ctx.params["mode"].get<std::string>();
            if (!Rendering::TryParseLodSelectionMode(mode, parsed))
                return Editor::RefuseRequest("mode must be \"off\", \"coverage\" or \"sse\"");
            requestedMode = parsed;
        }

        constexpr uint32_t kAutoLevel = 0xFFFFFFFFu;
        if (ctx.params.contains("forceLevel"))
        {
            int level = ctx.params["forceLevel"].get<int>();
            rs->SetLODForceLevel(level < 0 ? kAutoLevel
                                           : static_cast<uint32_t>(std::min(
                                                 level, static_cast<int>(MeshLODConfig::kMaxLODs) - 1)));
        }
        if (ctx.params.contains("bias"))
            rs->SetLODGlobalBias(ctx.params["bias"].get<float>());
        if (ctx.params.contains("shadowBias"))
            rs->SetShadowLODBias(ctx.params["shadowBias"].get<float>());
        if (ctx.params.contains("cullCoverage"))
            rs->SetSmallObjectCullCoverage(
                std::max(0.0f, ctx.params["cullCoverage"].get<float>()));
        // SSE-budget knobs (MeshLODThresholds.h). budgetPx <= 0 disables SSE
        // coarsening (keep-detail fail-safe); skinnedScale multiplies the
        // budget for skinned/character chains. Runtime-only, like every knob
        // here: these write the same RenderServices/registry state the project
        // settings (rendering.lodErrorBudgetPx / lodSkinnedBudgetScale /
        // lodMode) seed at startup, but nothing here is written back to the
        // project, so a restart returns to the persisted values.
        if (ctx.params.contains("budgetPx"))
            rs->SetLODErrorBudgetPx(ctx.params["budgetPx"].get<float>());
        if (ctx.params.contains("skinnedScale"))
            rs->SetLODSkinnedBudgetScale(ctx.params["skinnedScale"].get<float>());
        // Per-view-class budget overrides, keyed by ViewPurpose. Each accepts
        // {"enabled": bool, "percent": float}; a missing sub-key keeps the
        // current value, so a lane can flip "enabled" without restating the
        // percent. Only the two classes the settings page exposes are settable:
        // preview/thumbnail views frame each model to fill the view, so they sit
        // at LOD0 whatever the budget.
        auto applyViewBudget = [&ctx, rs](const char* key, Rendering::ViewPurpose purpose)
        {
            if (!ctx.params.contains(key) || !ctx.params[key].is_object())
                return;
            const auto& node = ctx.params[key];
            Rendering::LodViewBudgetOverride budget = rs->GetLODViewBudgetOverride(purpose);
            if (node.contains("enabled") && node["enabled"].is_boolean())
                budget.Enabled = node["enabled"].get<bool>();
            if (node.contains("percent") && node["percent"].is_number())
                budget.BudgetPercent = std::clamp(node["percent"].get<float>(),
                                                  Rendering::kMinLodBudgetPercent,
                                                  Rendering::kMaxLodBudgetPercent);
            rs->SetLODViewBudgetOverride(purpose, budget);
        };
        applyViewBudget("gameViewBudget", Rendering::ViewPurpose::Game);
        applyViewBudget("sceneViewBudget", Rendering::ViewPurpose::EditorScene);
        if (ctx.params.contains("crossfadeDuration"))
            rs->SetLODCrossfadeDuration(ctx.params["crossfadeDuration"].get<float>());
        // Switching the mapping re-derives every registered GPUMesh row in
        // place, so both arms of an A/B run in one process at one GPU clock
        // state. rowsRewritten is reported because a silent 0 would mean the
        // arm never actually changed.
        size_t lodRowsRewritten = 0;
        if (requestedMode.has_value())
            lodRowsRewritten = rs->GetMeshGPURegistry().SetLodSelectionMode(*requestedMode);
        if (ctx.params.contains("hysteresis"))
            rs->SetLODHysteresisBand(ctx.params["hysteresis"].get<float>());

        const uint32_t force = rs->GetLODForceLevel();
        const float globalBudgetPx = rs->GetLODErrorBudgetPx();
        auto viewBudgetJson = [rs, globalBudgetPx](Rendering::ViewPurpose purpose)
        {
            const Rendering::LodViewBudgetOverride budget = rs->GetLODViewBudgetOverride(purpose);
            return json{{"enabled", budget.Enabled},
                        {"percent", budget.BudgetPercent},
                        {"effectiveBudgetPx",
                         Rendering::ResolveLodBudgetPx(globalBudgetPx, budget)}};
        };
        return json{
            {"ok", true},
            {"forceLevel", force == kAutoLevel ? -1 : static_cast<int>(force)},
            {"bias", rs->GetLODGlobalBias()},
            {"shadowBias", rs->GetShadowLODBias()},
            {"cullCoverage", rs->GetSmallObjectCullCoverage()},
            {"budgetPx", rs->GetLODErrorBudgetPx()},
            {"skinnedScale", rs->GetLODSkinnedBudgetScale()},
            {"crossfadeDuration", rs->GetLODCrossfadeDuration()},
            {"mode", std::string(Rendering::ToString(
                         rs->GetMeshGPURegistry().GetLodSelectionMode()))},
            {"rowsRewritten", lodRowsRewritten},
            // Per-view-class overrides, with the budget each class actually
            // spends resolved for the reader — a disabled override reports the
            // global budget, which is what its views really select against.
            {"gameViewBudget", viewBudgetJson(Rendering::ViewPurpose::Game)},
            {"sceneViewBudget", viewBudgetJson(Rendering::ViewPurpose::EditorScene)},
            {"hysteresis", rs->GetLODHysteresisBand()}
        }; });

    // 30. set_shadow_quality — switch the shadow filter quality.
    // quality: 0 = 5x5 grid PCF, 1 = 3x3 grid PCF, 2 = Poisson PCF,
    //          3 = PCSS, 4 = MSM4 (Moment Shadow Maps).
    // PCSS / MSM4 silently fall back to PoissonPCF when their prerequisites
    // (bindless / a ready moments texture) aren't met — see
    // ShadowMapRenderFeature::BuildShadowDataGPU.
    server.RegisterHandler("set_shadow_quality", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        int quality = ctx.params.value("quality", 0);
        feature->SetPcfQuality(std::clamp(
            quality, 0, static_cast<int>(Engine::Renderer::ShadowFilterQuality::DPCF)));

        static const char* kNames[] = {
            "5x5 weighted PCF (9 taps)",
            "3x3 grid PCF (9 taps)",
            "Poisson PCF (variable taps)",
            "PCSS (contact hardening)",
            "MSM4 (Moment Shadow Maps)",
            "DPCF (contact hardening, no blocker search)",
        };
        return json{
            {"ok", true},
            {"quality", feature->GetPcfQuality()},
            {"description", kNames[feature->GetPcfQuality()]}
        }; });

    // 30b. set_shadow_pcss — toggle PCSS (contact-hardening shadows) and tune.
    // Optional params:
    //   enabled (bool), maxPenumbra (float, world units; shader caps physical
    //   penumbra width), receiverPlaneBias (bool).
    // Penumbra WIDTH is authored per light (Components::Light::ShadowAngularDiameter,
    // read/written through get_entity_components / set_component) — deliberately not
    // exposed here, because a global setter is the defect this slice removed.
    server.RegisterHandler("set_shadow_pcss", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");

        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");

        if (ctx.params.contains("enabled"))
            feature->SetPcssEnabled(ctx.params.value("enabled", true));
        if (ctx.params.contains("maxPenumbra"))
            feature->SetPcssMaxPenumbra(ctx.params.value("maxPenumbra", feature->GetPcssMaxPenumbra()));
        if (ctx.params.contains("receiverPlaneBias"))
            feature->SetPcssReceiverPlaneBias(ctx.params.value("receiverPlaneBias", feature->IsPcssReceiverPlaneBias()));
        // Tap count bounds the kernel now that the penumbra cap is derived from
        // it, so tuning softness against banding needs it live rather than per
        // rebuild. The setter snaps to {8,16,32,64}.
        if (ctx.params.contains("tapCount"))
            feature->SetPcssTapCount(ctx.params.value("tapCount", feature->GetPcssTapCount()));
        // Dither basis is a look A/B rather than a quality tier, so it belongs
        // on the live knob for the same reason tapCount does: the question it
        // answers is "which of these two reads better at this range", and that
        // cannot be asked one rebuild at a time.
        // Cascade projection mode: 0 = Stable, 1 = Close, 2 = WorldTexel.
        // Live because the whole point of WorldTexel is a look/stability A/B
        // against the two fitted modes, and a rebuild per arm makes that
        // comparison unusable.
        if (ctx.params.contains("projection"))
        {
            const int mode = std::clamp(ctx.params.value("projection", 1), 0, 2);
            feature->SetShadowProjection(static_cast<Engine::Renderer::ShadowProjection>(mode));
        }
        if (ctx.params.contains("cascade0TexelSize") || ctx.params.contains("cascadeTexelRatio"))
        {
            feature->SetWorldTexelLadder(
                ctx.params.value("cascade0TexelSize", feature->GetCascade0TexelSize()),
                ctx.params.value("cascadeTexelRatio", feature->GetCascadeTexelRatio()));
        }
        if (ctx.params.contains("ditherBasis"))
        {
            const int basis = ctx.params.value("ditherBasis", 0);
            feature->SetDitherBasis(basis == 1 ? Engine::Renderer::ShadowDitherBasis::ShadowSpace
                                               : Engine::Renderer::ShadowDitherBasis::Screen);
        }

        const bool stableShadowFallback =
            rs->GetDevice() &&
            rs->GetDevice()->GetCapabilities().prefersStableShadowFiltering;

        // Resolve through the same fallback chain the shader actually runs
        // this frame, including the MSM4-without-moments promotion into PCSS
        // -- a hand-rolled IsPcssEnabled() check only sees the requested
        // FilterQuality and misses that promotion entirely.
        Rendering::ViewId viewId = 0;
        if (!app.m_Windows.empty() && app.m_Windows[0]->scene)
            viewId = app.m_Windows[0]->scene->GetViewId();
        const bool effective =
            feature->ResolveEffectiveFilterQuality(*rs, viewId) ==
            Engine::Renderer::ShadowFilterQuality::PCSS;

        return json{
            {"ok", true},
            {"enabled", feature->IsPcssEnabled()},
            {"maxPenumbra", feature->GetPcssMaxPenumbra()},
            {"receiverPlaneBias", feature->IsPcssReceiverPlaneBias()},
            // Every settable field reports back. A knob that accepts a value and
            // returns only {"ok":true} cannot be distinguished from one that
            // silently ignored it -- tapCount was write-only until now.
            {"tapCount", feature->GetPcssTapCount()},
            {"ditherBasis", static_cast<int>(feature->GetDitherBasis())},
            {"projection", static_cast<int>(feature->GetShadowProjection())},
            {"cascade0TexelSize", feature->GetCascade0TexelSize()},
            {"cascadeTexelRatio", feature->GetCascadeTexelRatio()},
            {"bindlessAvailable", rs->Textures().IsBindlessEnabled()},
            {"stableShadowFallback", stableShadowFallback},
            {"effective", effective}
        }; });

    // 30c. set_moments_resolution — set MSM4 moments resolution (snaps to {512,1024,2048}).
    // Param: resolution (uint).
    server.RegisterHandler("set_moments_resolution", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");
        uint32_t res = ctx.params.value("resolution", 1024u);
        feature->SetMomentsResolution(res);
        return json{
            {"ok", true},
            {"resolution", feature->GetMomentsResolution()},
        }; });

    // 30d. set_msm_blur_mode — set MSM4 blur kernel mode.
    // Param: mode (int): 0 = Linear5Tap (fast), 1 = Discrete9Tap.
    server.RegisterHandler("set_msm_blur_mode", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("No RenderServices");
        auto* feature = rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>();
        if (!feature)
            return Editor::RefuseRequest("ShadowMapRenderFeature not initialized");
        int mode = ctx.params.value("mode", 0);
        feature->SetMsmBlurMode(static_cast<Engine::Renderer::MsmBlurMode>(std::clamp(mode, 0, 1)));
        static const char* kNames[] = {"Linear5Tap", "Discrete9Tap"};
        return json{
            {"ok", true},
            {"mode", static_cast<int>(feature->GetMsmBlurMode())},
            {"description", kNames[static_cast<int>(feature->GetMsmBlurMode())]},
        }; });

    // --- Asset Registry Debug Handlers ---

    server.RegisterHandler("get_asset_sources", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();
        auto sources = registry.GetRegisteredSources();
        json result = json::array();
        for (const auto& s : sources)
        {
            json src = json::object();
            src["alias"] = s.Alias;
            src["root"] = s.Root.string();
            src["priority"] = s.Priority;
            src["derivedIdentity"] = s.DerivedIdentity;
            auto guids = registry.GetAssetsForSource(s.Alias);
            src["assetCount"] = guids.size();
            result.push_back(std::move(src));
        }
        return json{{"sources", result}}; });

    server.RegisterHandler("get_asset_list", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string sourceAlias = ctx.params.value("source", "project");
        int limit = ctx.params.value("limit", 50);

        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& registry = am.GetRegistry();
        auto guids = registry.GetAssetsForSource(sourceAlias);

        json assets = json::array();
        int count = 0;
        for (const auto& guid : guids)
        {
            if (count >= limit) break;
            AssetMetadata meta;
            if (registry.TryGetAssetMetadata(guid, meta))
            {
                json a = json::object();
                a["guid"] = guid.ToString();
                a["path"] = meta.Path.string();
                a["type"] = static_cast<int>(meta.Type);
                a["name"] = std::string(meta.Name);
                assets.push_back(std::move(a));
                ++count;
            }
        }
        return json{{"source", sourceAlias}, {"total", guids.size()}, {"assets", assets}}; });

    server.RegisterHandler("trigger_build", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!app.m_PanelManager)
            return Editor::RefuseRequest("No panel manager");

        auto* buildPanel = app.m_PanelManager->FindFirstPanelOfType<BuildPanel>();
        if (!buildPanel)
            return Editor::RefuseRequest("BuildPanel not found. Open it via the toolbar first.");

        if (buildPanel->GetBuildStatus().running)
            return Editor::RefuseRequest("A build is already running");

        std::string platformName;
        if (ctx.params.contains("platformName"))
            platformName = ctx.params["platformName"].get<std::string>();
        else if (ctx.params.contains("platform"))
            platformName = ctx.params["platform"].get<std::string>();
        if (!platformName.empty() && !buildPanel->SetOnlyEnabledPlatform(platformName))
            return Editor::RefuseRequest("Unknown build platform: " + platformName);

        // This handler already runs on the main thread (FlushPendingRequests), where
        // PostAction has no UI dispatcher and gets dropped — so start the build
        // synchronously rather than via TriggerBuild()'s deferred PostAction.
        buildPanel->TriggerBuildImmediate();
        return json{{"ok", true}, {"platformName", platformName},
                    {"generation", buildPanel->GetBuildStatus().generation}, {"message", "Build requested"}}; });

    server.RegisterHandler("get_build_status", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        auto* panel = app.m_PanelManager ? app.m_PanelManager->FindFirstPanelOfType<BuildPanel>() : nullptr;
        if (!panel)
            return Editor::RefuseRequest("BuildPanel not found. Open it via the toolbar first.");
        const auto status = panel->GetBuildStatus();
        return json{{"running", status.running}, {"succeeded", status.succeeded},
                    {"outputDirectory", status.outputDirectory.string()},
                    {"generation", status.generation}, {"cancelled", status.progress.cancelled},
                    {"progress", status.progress.progress}, {"message", status.progress.statusMessage},
                    {"errors", status.progress.errors}, {"warnings", status.progress.warnings}}; });

    server.RegisterHandler("get_build_settings", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto& engine = EngineCore::GetInstance();
        const auto& wsRoot = engine.GetWorkspaceRoot();
        const auto& assetRoot = engine.GetResolvedAssetRoot();

        json result = json::object();
        result["workspaceRoot"] = wsRoot.string();
        result["assetRoot"] = assetRoot.string();
        result["sdkPath"] = (PathUtils::GetExecutableDirectory() / "SDK").string();
        result["sdkLibDebugExists"] = std::filesystem::is_directory(PathUtils::GetExecutableDirectory() / "SDK" / "lib" / "Debug");
        result["sdkLibReleaseExists"] = std::filesystem::is_directory(PathUtils::GetExecutableDirectory() / "SDK" / "lib" / "Release");

        // Count files in asset root
        int assetFileCount = 0;
        std::error_code ec;
        if (std::filesystem::is_directory(assetRoot, ec))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
                if (entry.is_regular_file()) ++assetFileCount;
        }
        result["assetFileCount"] = assetFileCount;

        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string settingsError;
        prefs.Load(&settingsError);

        json buildSettings = json::object();
        buildSettings["settingsPath"] = prefs.GetFilePath().string();
        if (!settingsError.empty())
            buildSettings["settingsError"] = settingsError;

        std::string globalScenesRaw;
        prefs.TryGetString("build.globalScenes", globalScenesRaw);
        buildSettings["globalScenes"] = ParseDebugBuildSceneList(globalScenesRaw);

        if (const auto& rootJson = prefs.Json(); rootJson.is_object())
        {
            const auto itRendering = rootJson.find("rendering");
            if (itRendering != rootJson.end() && itRendering->is_object())
            {
                const auto itPipeline = itRendering->find("activeRenderPipeline");
                if (itPipeline != itRendering->end() && itPipeline->is_string())
                    buildSettings["activeRenderPipeline"] = itPipeline->get<std::string>();
            }
        }

        json platforms = json::array();
        for (const GameEngine::BuildPlatformInfo& info : GameEngine::GetBuildPlatforms())
        {
            const std::string platform(info.Name);
            json platformSettings = json::object();
            platformSettings["name"] = platform;
            if (info.DisplayName != info.Name)
                platformSettings["displayName"] = std::string(info.DisplayName);

            const std::string prefix = "build.platform." + platform + ".";
            bool useGlobalScenes = true;
            prefs.TryGetBool(prefix + "useGlobalScenes", useGlobalScenes);
            platformSettings["useGlobalScenes"] = useGlobalScenes;

            std::string scenesRaw;
            prefs.TryGetString(prefix + "scenes", scenesRaw);
            platformSettings["platformScenes"] = ParseDebugBuildSceneList(scenesRaw);
            platformSettings["effectiveScenes"] = useGlobalScenes
                ? ParseDebugBuildSceneList(globalScenesRaw)
                : ParseDebugBuildSceneList(scenesRaw);

            std::string outputDir;
            prefs.TryGetString(prefix + "outputDir", outputDir);
            platformSettings["outputDir"] = outputDir;

            std::string buildConfig;
            prefs.TryGetString(prefix + "buildConfig", buildConfig);
            platformSettings["buildConfig"] = buildConfig;
            // The configuration a build would actually use. The stored value
            // stays empty until someone edits the field, so reporting it alone
            // says nothing about what this editor would package.
            platformSettings["effectiveBuildConfig"] =
                buildConfig.empty() ? std::string(DefaultPlayerBuildConfig()) : buildConfig;

            platforms.push_back(std::move(platformSettings));
        }
        buildSettings["platforms"] = std::move(platforms);
        result["buildSettings"] = std::move(buildSettings);

        return result; });

    // Skeleton store diagnostics: per-world runtime state for debugging multi-world skinning isolation.
    server.RegisterHandler("get_skeleton_store_state", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& store = Engine::Renderer::SkeletonStore::Instance();
        const uint32_t count = store.GetCount();

        // Resolve the optional GPU stores so we can label each runtime's
        // atlas owner. The skin-matrix probe below operates on the runtime's
        // CompactSkinMatrices (CPU cache), which is intentionally not
        // refreshed when GPU owns the slot — without an owner tag, the
        // identity-looking probe values can be misread as a rendering bug.
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        const auto* gpuAnim = rs ? &rs->GetGPUAnimationDataStore() : nullptr;
        const auto* retargetFeat = rs ? rs->GetFeature<Engine::Renderer::RetargetRenderFeature>() : nullptr;
        const auto* retargetStore = (retargetFeat && retargetFeat->IsInitialized())
            ? &retargetFeat->GetDataStore() : nullptr;

        auto* primaryWorld = EngineCore::GetInstance().GetPrimaryWorld();
        const uint64_t primaryWorldId = primaryWorld ? primaryWorld->GetWorldId() : 0;

        const bool withBoneNames = ctx.params.value("withBoneNames", false);
        const int onlySkeletonId = ctx.params.value("skeletonId", 0);

        json skeletons = json::array();
        for (uint32_t id = 1; id <= count; ++id)
        {
            if (onlySkeletonId != 0 && static_cast<int>(id) != onlySkeletonId) continue;
            const auto* skel = store.Get(id);
            if (!skel) continue;

            json skelJson;
            skelJson["id"] = id;
            skelJson["boneCount"] = skel->BoneCount;
            skelJson["skinJointCount"] = skel->SkinJointCount;
            skelJson["jointNodeCount"] = static_cast<uint32_t>(skel->JointNodes.size());

            if (withBoneNames)
            {
                json names = json::array();
                for (const auto& n : skel->BoneNames)
                    names.push_back(n);
                skelJson["boneNames"] = std::move(names);

                // Compute per-bone world rest rotations (composing local
                // RestRotation from root). Returns {x, y, z, w} per bone.
                // Useful for retarget triage: comparing source vs target
                // bind world rotations identifies whether two rigs share
                // a canonical T-pose at bind.
                json worldRest = json::array();
                std::vector<std::array<float, 4>> worldQuats(skel->BoneCount);
                const bool useTopo = !skel->TopologicalOrder.empty()
                    && skel->TopologicalOrder.size() == skel->BoneCount;
                for (uint32_t i = 0; i < skel->BoneCount; ++i)
                {
                    const uint32_t b = useTopo ? skel->TopologicalOrder[i] : i;
                    const float* rp = (b * 4u + 3u < skel->RestRotation.size())
                        ? &skel->RestRotation[b * 4u] : nullptr;
                    glm::quat local = rp
                        ? glm::quat(rp[3], rp[0], rp[1], rp[2])
                        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                    const int32 parent = (b < skel->Parent.size()) ? skel->Parent[b] : -1;
                    glm::quat world;
                    if (parent < 0) world = local;
                    else
                    {
                        const auto& p = worldQuats[static_cast<size_t>(parent)];
                        glm::quat parentW(p[3], p[0], p[1], p[2]);
                        world = parentW * local;
                    }
                    world = glm::normalize(world);
                    worldQuats[b] = {world.x, world.y, world.z, world.w};
                }
                for (uint32_t b = 0; b < skel->BoneCount; ++b)
                {
                    const auto& w = worldQuats[b];
                    worldRest.push_back({w[0], w[1], w[2], w[3]});
                }
                skelJson["restWorld"] = std::move(worldRest);
            }

            skeletons.push_back(skelJson);
        }

        // Per-entity runtimes — required to debug retarget / skinning data
        // flow (atlas offset routing, palette content). Probes the first
        // non-identity transform to indicate whether the per-frame writer
        // (AnimationSystem CPU branch, HumanoidRetargetSystem, or GPU
        // compute) actually populated motion vs. left bind-pose identity.
        const size_t runtimeHighWater = store.GetRuntimeHighWaterMark();
        json runtimes = json::array();
        for (size_t i = 0; i < runtimeHighWater; ++i)
        {
            const uint32_t rtId = static_cast<uint32_t>(i + 1);
            const uint32_t skelId = store.GetRuntimeSkeletonId(rtId);
            if (skelId == 0) continue; // freed slot

            const auto* runtime = store.GetRuntime(rtId);
            if (!runtime) continue;

            json rtJson;
            rtJson["runtimeId"] = rtId;
            rtJson["skeletonId"] = skelId;
            // How many live SkeletonRef components name this runtime — one per
            // entity of the model instance. Deleting one sibling drops it by
            // one; the slot is freed only when the last reference goes.
            rtJson["refCount"] = store.GetRuntimeRefCount(rtId);
            rtJson["atlasPaletteOffsetBones"] = runtime->AtlasPaletteOffsetBones;
            rtJson["paletteMatrixCount"] = static_cast<uint32_t>(runtime->CompactSkinMatrices.size() / 16);

            // Atlas owner: which subsystem wrote this runtime's atlas slot
            // this frame. The skin-matrix probe below reads the CPU mirror,
            // which is only updated by the CPU eval paths. When the GPU owns
            // the slot, the probe values reflect the last CPU eval (often
            // never), NOT what the renderer reads.
            const char* atlasOwner = "cpu"; // SkinningUploadSystem default
            if (retargetStore && retargetStore->IsRetargetGPUHandled(rtId))
                atlasOwner = "gpu-retarget";
            else if (gpuAnim && gpuAnim->IsGPUHandled(rtId))
                atlasOwner = "gpu-skinning";
            rtJson["atlasOwner"] = atlasOwner;
            rtJson["paletteProbeIsStale"] = (atlasOwner[0] == 'g'); // gpu-* owners

            // Probe for the first non-identity matrix and report its
            // translation magnitude + max diagonal deviation. This tells
            // the caller whether the palette is bind-pose-identity
            // (suggesting the writer never ran or produced no motion) or
            // contains real animated transforms.
            uint32_t firstNonIdentityIdx = UINT32_MAX;
            float maxTranslationMag = 0.0f;
            float maxOffDiagonalAbs = 0.0f;
            const auto& m = runtime->CompactSkinMatrices;
            const size_t mats = m.size() / 16;
            for (size_t j = 0; j < mats; ++j)
            {
                const float* p = &m[j * 16];
                // Column-major: translation is at indices [12..14]; diagonal at [0],[5],[10].
                const float tx = p[12], ty = p[13], tz = p[14];
                const float tmag = std::sqrt(tx*tx + ty*ty + tz*tz);
                // Off-diagonal deviation captures any rotation away from identity.
                const float od = std::max({
                    std::fabs(p[1]), std::fabs(p[2]), std::fabs(p[4]),
                    std::fabs(p[6]), std::fabs(p[8]), std::fabs(p[9])
                });
                if (tmag > maxTranslationMag) maxTranslationMag = tmag;
                if (od > maxOffDiagonalAbs) maxOffDiagonalAbs = od;
                const bool isIdentityIsh =
                    tmag < 1e-5f &&
                    std::fabs(p[0] - 1.0f) < 1e-5f &&
                    std::fabs(p[5] - 1.0f) < 1e-5f &&
                    std::fabs(p[10] - 1.0f) < 1e-5f &&
                    od < 1e-5f;
                if (!isIdentityIsh && firstNonIdentityIdx == UINT32_MAX)
                    firstNonIdentityIdx = static_cast<uint32_t>(j);
            }
            rtJson["firstNonIdentityJointIndex"] = firstNonIdentityIdx == UINT32_MAX
                ? -1
                : static_cast<int>(firstNonIdentityIdx);
            rtJson["maxTranslationMag"] = maxTranslationMag;
            rtJson["maxRotationOffDiagonal"] = maxOffDiagonalAbs;
            runtimes.push_back(rtJson);
        }

        json result;
        result["skeletonCount"] = count;
        result["primaryWorldId"] = primaryWorldId;
        result["skeletons"] = skeletons;
        result["runtimes"] = runtimes;
        result["runtimeHighWaterMark"] = static_cast<uint32_t>(runtimeHighWater);
        return result; });

    // CPU profiler: the last completed frame as a main-thread call tree, plus
    // the flat aggregate that is the only view carrying scopes which ran off
    // the main thread. Auto-enables on first call so external tooling doesn't
    // need to bounce through the F2 hotkey; enable:false disarms, which matters
    // because an armed profiler takes its mutex in every scope exit on every
    // thread, so leaving it armed taxes every measurement taken afterwards.
    //
    // A disarm holds only while the CPU Profiler panel is closed: that panel
    // re-arms every Update() it spends visible and unpaused, and nothing
    // arbitrates between the two. wasEnabledOnEntry on the next read tells the
    // caller whether the disarm survived.
    server.RegisterHandler("get_cpu_profiler", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& prof = Profiling::CpuProfiler::Get();
        const bool wasEnabled = prof.IsEnabled();
        const bool wantEnabled = ctx.params.value("enable", true);

        std::vector<Profiling::CpuProfiler::TreeRow> tree;
        prof.CopyFrameTree(tree);

        std::vector<std::pair<std::string_view, Profiling::CpuProfiler::Sample>> flat;
        prof.CopyFrameSamples(flat);
        std::sort(flat.begin(), flat.end(),
                  [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

        const std::size_t maxRows = ctx.params.value("maxRows", static_cast<std::size_t>(64));

        json treeJson = json::array();
        const std::size_t nt = std::min(tree.size(), maxRows);
        for (std::size_t i = 0; i < nt; ++i)
        {
            const auto& r = tree[i];
            treeJson.push_back({
                {"name", r.name},
                {"depth", r.depth},
                {"totalMs", r.totalMs},
                {"selfMs", r.selfMs},
                {"calls", r.callCount}
            });
        }

        json flatJson = json::array();
        const std::size_t nf = std::min(flat.size(), maxRows);
        for (std::size_t i = 0; i < nf; ++i)
        {
            const auto& sample = flat[i].second;
            // Which thread paid for it. The call tree is main-thread only, so
            // for an off-main scope this row is the whole report, and reading
            // it as main-thread cost would misattribute a job worker's work.
            const char* thread = "main";
            if (sample.offMainCount > 0)
                thread = (sample.offMainCount == sample.count) ? "off-main" : "mixed";

            flatJson.push_back({
                {"name", std::string(flat[i].first)},
                {"totalMs", sample.totalMs},
                {"calls", sample.count},
                {"thread", thread}
            });
        }

        // State change last: a disarming call still returns the frame the
        // profiler had already paid for.
        if (wantEnabled != wasEnabled)
            prof.SetEnabled(wantEnabled);

        json result;
        result["enabled"] = prof.IsEnabled();
        result["wasEnabledOnEntry"] = wasEnabled;
        result["tree"] = treeJson;
        result["flat"] = flatJson;
        return result; });

    // GPU profiler: per-pass CPU/GPU timings from the main window's render graph
    // for the last resolved frame. Pass {"enabled": true|false} to arm/disarm
    // per-pass GPU timestamp profiling (results resolve ~FramesInFlight frames
    // later; the first call after arming returns empty until they land).
    // timingSemantics says what gpuSpanMs measures; under "encoder-span" (Metal)
    // the values are overlapping encoder spans and there is deliberately no
    // frame GPU total in the payload — summing them is wrong. spanCounted marks
    // the one pass per measurement that resolveStats.distinctSpanGpuMs counts
    // (RGPassTiming::SpanCounted), so a total over any set of passes adds only
    // those.
    server.RegisterHandler("get_gpu_profiler", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        RG::RGFrame* frame = MainRgFrame(app);
        if (!frame)
            return Editor::RefuseRequest(kRgNoFrameMsg);

        if (ctx.params.contains("enabled"))
            frame->SetProfilingEnabled(ctx.params.value("enabled", false));

        const auto timings = frame->LastFrameTimings();
        const RG::RGFrame::RGProfilingResolveStats rs = frame->LastResolveStats();
        const bool encoderSpan = frame->TimingSemantics() == Rendering::TimestampSemantics::EncoderSpan;

        json passes = json::array();
        double totalCpu = 0.0;
        for (const auto& t : timings)
        {
            totalCpu += t.CpuMs;
            passes.push_back({{"name", t.Name},
                              {"queue", RgQueueName(t.Queue)},
                              {"phase", t.Phase},
                              {"cpuMs", t.CpuMs},
                              {"gpuSpanMs", t.GpuSpanMs},
                              {"spanShared", t.SpanShared},
                              {"spanCounted", t.SpanCounted}});
        }

        json warnings = json::array();
        if (encoderSpan)
        {
            warnings.push_back(
                "gpuSpanMs values are summed command-encoder spans, not additive pass costs: "
                "one encoder can carry several passes and is then reported to each of them "
                "(spanShared), and encoders overlap one another because the GPU runs them "
                "concurrently. Treat each as an upper bound and do NOT sum them — use "
                "resolveStats.distinctSpanGpuMs (itself an upper bound) or the device's GPU frame "
                "period for a frame total. To total a group of passes, add only the ones with "
                "spanCounted true: each measurement is counted on exactly one pass.");
        }
        if (rs.NonMonotonic > 0)
        {
            warnings.push_back("resolveStats.nonMonotonicTimestamps = " +
                               std::to_string(rs.NonMonotonic) +
                               ": that many passes' raw timestamp pairs closed before they opened "
                               "and were discarded, so their gpuSpanMs reads 0.");
        }
        if (rs.ReadFailures > 0 || rs.InvalidQueryIndices > 0)
        {
            warnings.push_back("resolveStats readFailures/invalidQueryIndices are non-zero: those "
                               "passes have no measurement this frame and read 0.");
        }

        json result;
        result["enabled"] = frame->ProfilingEnabled();
        if (!frame->ProfilingEnabled())
            result["hint"] = "Profiling disarmed — call with {\"enabled\": true}, then poll again.";
        result["timingSemantics"] = encoderSpan ? "encoder-span" : "pipeline-point";
        result["warnings"] = std::move(warnings);
        result["resolveStats"] = {
            {"invalidQueryIndices", rs.InvalidQueryIndices},
            {"readFailures", rs.ReadFailures},
            {"nonMonotonicTimestamps", rs.NonMonotonic},
            {"resolvedPasses", rs.ResolvedPasses},
            {"sharedSpanPasses", rs.SharedSpanPasses},
            {"distinctSpanGpuMs", rs.DistinctSpanGpuMs}
        };
        result["lastFrame"] = {
            {"passes", std::move(passes)},
            {"totalCpuMs", totalCpu}
        };
        return result; });

    // Named monitor history (frame-time ring buffer, memory, draw counts, ...).
    // Returns the list of registered monitors with their latest/peak/average,
    // and optionally the full history for a single monitor when 'name' is set.
    server.RegisterHandler("get_monitors", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto& metrics = Debug::DebugMetrics::Get();

        json result;
        result["paused"] = metrics.IsPaused();

        std::vector<Debug::MonitorInfo> infos;
        metrics.Snapshot(infos);
        std::sort(infos.begin(), infos.end(),
                  [](const Debug::MonitorInfo& a, const Debug::MonitorInfo& b) {
                      return a.Name < b.Name;
                  });

        json monitors = json::array();
        for (const auto& info : infos)
        {
            const char* typeStr = "Quantity";
            switch (info.Type)
            {
                case Debug::MonitorType::Memory:  typeStr = "Memory";  break;
                case Debug::MonitorType::TimeMs:  typeStr = "TimeMs";  break;
                case Debug::MonitorType::Percent: typeStr = "Percent"; break;
                default: break;
            }
            monitors.push_back({
                {"name", info.Name},
                {"type", typeStr},
                {"unit", info.Unit},
                {"latest", info.Latest},
                {"peak", info.Peak},
                {"average", info.Average}
            });
        }
        result["monitors"] = monitors;

        if (auto it = ctx.params.find("name"); it != ctx.params.end() && it->is_string())
        {
            std::vector<float> history;
            const std::size_t count = metrics.CopyHistory(it->get<std::string>(), history);
            // Return only the valid tail so the JSON isn't padded with zeros.
            json histJson = json::array();
            const std::size_t start = history.size() > count ? 0 : 0;
            const std::size_t len = std::min(count, history.size());
            for (std::size_t i = start; i < len; ++i)
                histJson.push_back(history[i]);
            result["history"] = {
                {"name", it->get<std::string>()},
                {"samples", histJson},
                {"count", count}
            };
        }

        return result; });

    // R0.11 diagnostic: the device's HDR output state (swapchain mode, encode
    // anchors, static metadata) next to the OS per-monitor SDR white levels so
    // the paper-white divisor can be verified against what Windows reports.
    server.RegisterHandler("get_hdr_output", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        json result;
        // The main window UI's most recent RenderRG declaration: the
        // TARGET-derived encoding and the subpixel gate it resolved (#784).
        // Under a pending capture these reflect the capture frame's SDR
        // declaration; steady-state they follow the display-bound target.
        if (!app.m_Windows.empty() && app.m_Windows[0] && app.m_Windows[0]->ui)
        {
            result["uiOutputEncoding"] = app.m_Windows[0]->ui->GetLastResolvedOutputEncoding();
            result["uiTextSubpixelActive"] = app.m_Windows[0]->ui->GetLastTextSubpixelActive();
        }
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = rs->GetDevice())
            {
                const Rendering::HdrOutputState st = device->GetHdrOutputState();
                result["enabled"] = st.enabled;
                result["requestedMode"] = Rendering::HdrOutputModeToString(st.requestedMode);
                result["activeMode"] = Rendering::HdrOutputModeToString(st.activeMode);
                result["swapchainBitDepth"] = static_cast<int>(st.swapchainBitDepth);
                result["scRGBFramebufferWhiteNits"] = Rendering::GetScRGBFramebufferWhiteNits(st);
                result["outputMaxLinearValue"] = Rendering::GetHdrOutputMaxLinearValue(st);
                result["peakLuminanceNits"] = Rendering::GetHdrOutputPeakLuminanceNits(st);
                result["staticMetadata"] = {
                    {"paperWhiteNits", st.staticMetadata.paperWhiteNits},
                    {"maxMasteringLuminance", st.staticMetadata.maxMasteringLuminance},
                    {"minMasteringLuminance", st.staticMetadata.minMasteringLuminance},
                    {"maxContentLightLevel", st.staticMetadata.maxContentLightLevel},
                    {"maxFrameAverageLightLevel", st.staticMetadata.maxFrameAverageLightLevel}};
                result["display"] = {
                    {"id", st.display.id},
                    {"name", st.display.name},
                    {"activeMonitor", st.display.activeMonitor},
                    {"hdrAvailable", st.display.hdrAvailable},
                    {"hdrActive", st.display.hdrActive},
                    {"supportsHDR10_PQ", st.display.supportsHDR10_PQ},
                    {"supportsScRGB", st.display.supportsScRGB},
                    {"swapchainFormat", static_cast<int>(st.display.swapchainFormat)},
                    {"maxLuminance", st.display.maxLuminance},
                    {"minLuminance", st.display.minLuminance},
                    {"maxFullFrameLuminance", st.display.maxFullFrameLuminance},
                    {"paperWhiteNits", st.display.paperWhiteNits},
                    {"outputMaxLinearValue", st.display.outputMaxLinearValue},
                    {"requestedMode", Rendering::HdrOutputModeToString(st.display.requestedMode)},
                    {"resolvedMode", Rendering::HdrOutputModeToString(st.display.resolvedMode)},
                    {"colorSpaces", st.display.colorSpaces},
                    {"diagnosticHints", st.display.diagnosticHints}};
            }
        }
        json monitors = json::array();
        for (const auto& m : Platform::EnumerateMonitors())
        {
            monitors.push_back({{"index", m.index},
                                {"id", m.id},
                                {"name", m.name},
                                {"primary", m.primary},
                                {"hdrAvailable", m.hdrAvailable},
                                {"hdrActive", m.hdrActive},
                                {"supportsScRGB", m.supportsScRGB},
                                {"supportsHDR10_PQ", m.supportsHDR10_PQ},
                                {"maxLuminance", m.maxLuminance},
                                {"minLuminance", m.minLuminance},
                                {"maxFullFrameLuminance", m.maxFullFrameLuminance},
                                {"sdrWhiteLevelNits", m.sdrWhiteLevelNits},
                                {"diagnosticHints", m.diagnosticHints}});
        }
        result["osMonitors"] = monitors;
        // Which values are explicit project-settings overrides. Absent = auto
        // (monitor-derived); that distinction is exactly what this diagnostic
        // exists to make visible.
        {
            json overrides = json::object();
            Editor::SettingsStore store =
                Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
            std::string error;
            if (store.Load(&error) && store.Json().is_object())
            {
                const auto& root = store.Json();
                if (auto itR = root.find("rendering"); itR != root.end() && itR->is_object())
                {
                    if (auto itH = itR->find("hdr"); itH != itR->end() && itH->is_object())
                    {
                        for (const char* k : {"enabled", "mode", "uiPaperWhiteNits",
                                              "uiBlackLiftNits", "metadata"})
                            if (itH->contains(k))
                                overrides[k] = (*itH)[k];
                    }
                }
            }
            result["settingsOverrides"] = overrides;
        }
        return result; });

    server.RegisterHandler("set_hdr_output", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const bool enabled = ctx.params.value("enabled", true);
        const bool hasExplicitMode = ctx.params.contains("mode") && ctx.params["mode"].is_string();
        const std::string modeText = ctx.params.value("mode", std::string("Auto"));

        auto& engine = EngineCore::GetInstance();
        Editor::SettingsStore store = Editor::OpenProjectSettings(engine.GetWorkspaceRoot());
        std::string error;
        if (!store.Load(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to load project settings" : error);

        nlohmann::json hdr = nlohmann::json::object();
        const auto& root = store.Json();
        if (root.is_object())
        {
            const auto itRendering = root.find("rendering");
            if (itRendering != root.end() && itRendering->is_object())
            {
                const auto itHdr = itRendering->find("hdr");
                if (itHdr != itRendering->end() && itHdr->is_object())
                    hdr = *itHdr;
            }
        }
        // A caller-supplied mode is validated, never silently coerced: an
        // unrecognized spelling used to parse as Off and then get normalized to
        // Auto, so a typo applied a mode the caller never asked for and reported
        // success.
        std::optional<Rendering::HdrOutputMode> requestedMode;
        if (hasExplicitMode)
        {
            requestedMode = Rendering::TryParseHdrOutputMode(modeText);
            if (!requestedMode)
                return Editor::RefuseRequest("Unrecognized HDR mode '" + modeText +
                                                 "'. Valid values: Off, Auto, HDR10_PQ, HLG, scRGB, HDR10+.");
        }

        hdr["enabled"] = enabled;
        if (enabled)
        {
            Rendering::HdrOutputMode mode = requestedMode.value_or(Rendering::HdrOutputMode::Auto);
            if (mode == Rendering::HdrOutputMode::Off)
                mode = Rendering::HdrOutputMode::Auto;
            hdr["mode"] = Rendering::HdrOutputModeToString(mode);
        }
        else
        {
            Rendering::HdrOutputMode mode = Rendering::HdrOutputMode::Off;
            if (hdr.contains("mode") && hdr["mode"].is_string())
                mode = Rendering::HdrOutputModeFromString(hdr["mode"].get<std::string>());
            if (requestedMode && *requestedMode != Rendering::HdrOutputMode::Off)
                mode = *requestedMode;
            if (mode == Rendering::HdrOutputMode::Off)
                mode = Rendering::HdrOutputMode::Auto;
            hdr["mode"] = Rendering::HdrOutputModeToString(mode);
        }
        // Persist only caller-provided values. Materializing defaults here made
        // every field read as an explicit user override and permanently disabled
        // the monitor-derived tuning (paperWhiteNits pinned at 203 while the OS
        // SDR white level said otherwise). null erases a key = back to auto.
        auto writeOrErase = [&ctx](nlohmann::json& target, const char* key)
        {
            const auto it = ctx.params.find(key);
            if (it == ctx.params.end())
                return;
            if (it->is_null())
                target.erase(key);
            else if (it->is_number())
                target[key] = it->get<float>();
        };
        auto& metadata = hdr["metadata"];
        if (!metadata.is_object())
            metadata = nlohmann::json::object();
        writeOrErase(metadata, "paperWhiteNits");
        writeOrErase(metadata, "maxMasteringLuminance");
        writeOrErase(metadata, "maxContentLightLevel");
        writeOrErase(metadata, "maxFrameAverageLightLevel");
        if (metadata.empty())
            hdr.erase("metadata");
        writeOrErase(hdr, "uiPaperWhiteNits");
        writeOrErase(hdr, "uiBlackLiftNits");
        auto& writableRoot = store.Json();
        if (!writableRoot.is_object())
            writableRoot = nlohmann::json::object();
        auto& rendering = writableRoot["rendering"];
        if (!rendering.is_object())
            rendering = nlohmann::json::object();
        rendering["hdr"] = hdr;
        if (!store.Save(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to save project settings" : error);

        app.RequestHdrOutputRefreshForAllWindows();
        return json{{"ok", true}, {"queued", true}, {"enabled", enabled}, {"mode", enabled ? hdr["mode"].get<std::string>() : "Off"}}; });

    // get_msaa — report the engine-wide default MSAA sample count (the value cameras
    // with MSAASamples==0 inherit), the device cap, and the persisted project setting.
    //
    // The sample count alone does NOT say whether anything is multisampled: it
    // keeps its value while aaMode is off or taa, so a caller reading only
    // "samples" can report 8x on a run with no MSAA at all. aaMode is therefore
    // reported alongside it and is the field that decides whether samples is in
    // effect (it is honored only in msaa mode). aaModePersisted is the project
    // setting, which names a concrete mode: opening a project writes one for it
    // if it never chose. It is empty only for a project whose file has not been
    // materialized, and aaMode then reports the capability default the renderer
    // resolved in memory.
    //
    // Both fields describe the ENGINE-WIDE default, which is not necessarily what
    // a given view renders. At least two things reroute it downstream, so treat
    // this as a floor rather than an exhaustive list: any TAA-resolved mode is
    // dropped per-view where TAA does not apply (SceneViewController /
    // GameViewController drop it for orthographic, 2D, fixed-orientation,
    // letterboxed or already-multisampled views, and they do so identically
    // for every stored mode, including a materialized default), and a camera can
    // override the mode outright via RenderServices::ResolveAntiAliasing.
    server.RegisterHandler("get_msaa", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (!rs)
            return Editor::RefuseRequest("RenderServices unavailable");
        uint32_t deviceMax = 1u;
        if (auto* device = rs->GetDevice())
            deviceMax = std::max(1u, device->GetCapabilities().maxMSAASamples);
        std::string persisted;
        std::string persistedAAMode;
        {
            Editor::SettingsStore store =
                Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
            std::string error;
            if (store.Load(&error) && store.Json().is_object())
            {
                const auto& root = store.Json();
                if (auto itR = root.find("rendering"); itR != root.end() && itR->is_object())
                {
                    if (auto itM = itR->find("msaa"); itM != itR->end() && itM->is_string())
                        persisted = itM->get<std::string>();
                    if (auto itA = itR->find("aaMode"); itA != itR->end() && itA->is_string())
                        persistedAAMode = itA->get<std::string>();
                }
            }
        }
        return json{{"samples", rs->GetDefaultMSAASampleCount()},
                    {"deviceMax", deviceMax},
                    {"persisted", persisted},
                    {"aaMode", Rendering::ToAAModeToken(rs->GetDefaultAntiAliasingMode())},
                    {"aaModePersisted", persistedAAMode}}; });

    // set_msaa — persist rendering.msaa (same form the Settings panel uses) and
    // re-apply live (fans out to every window). samples: 1 = off, or 2/4/8 —
    // every accepted value is a concrete count, so what lands in the file is
    // what the renderer runs. The device clamps unsupported counts down. Shadow
    // maps are unaffected (depth-only, always single-sample).
    server.RegisterHandler("set_msaa", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("samples") || !ctx.params["samples"].is_number_integer())
            return Editor::RefuseRequest("set_msaa requires integer 'samples' (1=off, 2, 4, or 8)");
        const int requested = ctx.params["samples"].get<int>();
        if (requested != 1 && requested != 2 && requested != 4 && requested != 8)
            return Editor::RefuseRequest("invalid 'samples' (expected 1, 2, 4, or 8)");

        // Persist FIRST: the fan-out below re-applies from the settings file,
        // so a failed save would silently apply the old value.
        const std::string normalized =
            requested == 1 ? std::string("off") : std::to_string(requested);
        Editor::SettingsStore store =
            Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string error;
        if (!store.Load(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to load project settings" : error);
        auto& root = store.Json();
        if (!root.is_object())
            root = nlohmann::json::object();
        auto& rendering = root["rendering"];
        if (!rendering.is_object())
            rendering = nlohmann::json::object();
        rendering["msaa"] = normalized;
        if (!store.Save(&error))
            return Editor::RefuseRequest(error.empty() ? "Failed to save project settings" : error);

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        const uint32_t resolved =
            rs ? rs->GetDefaultMSAASampleCount() : static_cast<uint32_t>(std::max(1, requested));
        return json{{"ok", true}, {"samples", resolved}, {"persisted", normalized}}; });

    // set_aa_mode — persist rendering.aaMode and re-apply live (fans out to
    // every window; targets re-spec next frame). mode: "off" | "msaa" | "taa" |
    // "fxaa" | "smaa" | "temporalfxaa" | "ssaa".
    //
    // "ssaa" is the one that is not an engine mode: it is editor-level and
    // applies as Off, with set_taa_render_scale carrying the supersampling.
    // Every mode leaves the sample count as rendering.msaa / set_msaa
    // configured it (honored only in msaa mode). The response reports the
    // APPLIED mode and samples, which differ from the request only under
    // GE_AA_MODE — that override persists the value but suppresses the live
    // apply, because the env var outranks project settings. get_msaa reports
    // the same applied mode without mutating anything.
    server.RegisterHandler("set_aa_mode", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string mode = ctx.params.value("mode", "");
        if (mode != "ssaa" && mode != Rendering::ToAAModeToken(Rendering::ParseAAModeToken(mode)))
            return Editor::RefuseRequest("set_aa_mode requires 'mode' of off|msaa|taa|fxaa|smaa|temporalfxaa|ssaa");

        // Persist FIRST: the fan-out below re-applies from the settings file.
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        auto settings = Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
        settings.ModeChosen = true;
        settings.AAMode = Rendering::ParseAAModeToken(mode);
        if (!Editor::SaveProjectAntiAliasingSettings(workspaceRoot, settings))
            return Editor::RefuseRequest("Failed to save project settings");

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        return json{{"ok", true},
                    {"mode", mode},
                    {"applied", Rendering::ToAAModeToken(
                                    rs ? rs->GetDefaultAntiAliasingMode() : settings.AAMode)},
                    {"samples", rs ? rs->GetDefaultMSAASampleCount() : 1u}}; });

    // set_taa_render_scale — persist rendering.taaRenderScale and re-apply
    // live (fans out to every window; the pipeline pre-pass re-derives the
    // internal extent next frame). scale: [0.5, 2.0]; 1.0 = native, above 1.0
    // supersamples (SSAA). Fully live, including the material texture mip bias
    // (per-view ViewParams uniform derived from the actual extent ratio).
    // Under GE_TAA_RENDER_SCALE the value persists but the live apply is
    // suppressed — the env override outranks project settings.
    server.RegisterHandler("set_taa_render_scale",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!ctx.params.contains("scale") || !ctx.params["scale"].is_number())
            return Editor::RefuseRequest("set_taa_render_scale requires number 'scale' in [0.5, 2.0]");
        const float requested = ctx.params["scale"].get<float>();
        if (!(requested >= 0.5f && requested <= 2.0f))
            return Editor::RefuseRequest("invalid 'scale' (expected [0.5, 2.0])");

        // Persist FIRST: the fan-out below re-applies from the settings file.
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        auto settings = Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
        settings.RenderScale = requested;
        // A non-native scale should take effect: Render Scale Mode Native pins
        // the scale to 1.0 on apply, so move it to Fixed — same behavior as the
        // settings slider. Dynamic keeps its mode.
        const bool nativeScale = requested > 0.995f && requested < 1.005f;
        if (!nativeScale && settings.ScaleMode == Engine::Renderer::DynamicResolutionMode::Off)
            settings.ScaleMode = Engine::Renderer::DynamicResolutionMode::Fixed;
        if (!Editor::SaveProjectAntiAliasingSettings(workspaceRoot, settings))
            return Editor::RefuseRequest("Failed to save project settings");

        app.ApplyProjectRenderSettingsToAllWindows();
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        return json{{"ok", true},
                    {"applied", rs ? rs->GetDefaultRenderScale() : requested}}; });

    // set_dynamic_resolution — mode + target for DRS, live, for same-session
    // A/B. mode: "off" | "fixed" | "dynamic". targetFps / targetMs optionally
    // retune the controller. Dynamic mode arms the render-graph GPU profiler
    // (its per-pass timestamps are the cost signal) and drives the same render
    // scale set_taa_render_scale writes, so the two are mutually exclusive by
    // construction. Returns the live controller state so a harness can watch
    // the loop converge without a screenshot.
    //
    // Fans out live rather than through the settings file like its three
    // siblings: this knob does not persist (targetMs, minScale and maxScale
    // have no project key at all), so the file cannot carry the state, and
    // under GE_TAA_RENDER_SCALE the file-backed re-apply skips the whole DRS
    // block — which would make this handler a no-op in exactly the A/B runs it
    // exists for.
    server.RegisterHandler("set_dynamic_resolution",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        if (rs == nullptr)
            return Editor::RefuseRequest("no RenderServices");

        using GameEngine::Engine::Renderer::DynamicResolutionMode;
        if (ctx.params.contains("mode"))
        {
            if (!ctx.params["mode"].is_string())
                return Editor::RefuseRequest("'mode' must be one of off|fixed|dynamic");
            const std::string mode = ctx.params["mode"].get<std::string>();
            if (mode == "off")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Off);
            else if (mode == "fixed")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Fixed);
            else if (mode == "dynamic")
                rs->SetDynamicResolutionMode(DynamicResolutionMode::Dynamic);
            else
                return Editor::RefuseRequest("'mode' must be one of off|fixed|dynamic");
        }

        auto config = rs->GetDynamicResolutionConfig();
        bool retune = false;
        if (ctx.params.contains("targetFps") && ctx.params["targetFps"].is_number())
        {
            const float fps = ctx.params["targetFps"].get<float>();
            if (!(fps > 0.0f))
                return Editor::RefuseRequest("'targetFps' must be > 0");
            config.TargetGpuMs = 1000.0f / fps;
            retune = true;
        }
        if (ctx.params.contains("targetMs") && ctx.params["targetMs"].is_number())
        {
            const float ms = ctx.params["targetMs"].get<float>();
            if (!(ms > 0.0f))
                return Editor::RefuseRequest("'targetMs' must be > 0");
            config.TargetGpuMs = ms;
            retune = true;
        }
        if (ctx.params.contains("minScale") && ctx.params["minScale"].is_number())
        {
            config.MinScale = ctx.params["minScale"].get<float>();
            retune = true;
        }
        if (ctx.params.contains("maxScale") && ctx.params["maxScale"].is_number())
        {
            config.MaxScale = ctx.params["maxScale"].get<float>();
            retune = true;
        }
        if (retune)
            rs->SetDynamicResolutionConfig(config);

        // Every floating window owns a private RenderServices, so the writes
        // above only moved the main one.
        app.ApplyDynamicResolutionFromMainWindowToAllWindows();

        const auto& applied = rs->GetDynamicResolutionConfig();
        const auto& stats = rs->GetDynamicResolutionStats();
        return json{{"ok", true},
                    {"mode", static_cast<uint32_t>(rs->GetDynamicResolutionMode())},
                    {"scale", rs->GetDefaultRenderScale()},
                    {"targetGpuMs", applied.TargetGpuMs},
                    {"minScale", applied.MinScale},
                    {"maxScale", applied.MaxScale},
                    // ticks/appFrameEpoch deltas between two polls must be ~1:
                    // the controller ticks once per app frame no matter how
                    // many windows declare (the multi-window dedup contract).
                    {"ticks", stats.Ticks},
                    {"appFrameEpoch", rs->Spine().WorldFrameEpoch()},
                    {"smoothedGpuMs", stats.SmoothedGpuMs},
                    {"acceptedDrops", stats.AcceptedDrops},
                    {"acceptedRises", stats.AcceptedRises},
                    {"saturatedLow", stats.SaturatedLow},
                    {"saturatedHigh", stats.SaturatedHigh},
                    {"responseFitValid", stats.ResponseFitValid},
                    {"scalingIneffective", stats.ScalingIneffective},
                    {"estimatedUnscalableMs", stats.EstimatedUnscalableMs},
                    {"estimatedFullRangeSavingMs", stats.EstimatedFullRangeSavingMs}}; });

    // debug_crash — deliberately kill the editor through a chosen crash path so
    // automation can gate the crash-evidence pipeline (SEH filter / terminate
    // handler / signal handler → minidump in every config + synchronous stderr
    // backtrace). This exists because a real editor death used to leave zero
    // evidence in DebugFast/Release, and got misattributed to whatever IPC call
    // happened nearby (the 2026-07-16 "set_component float crash" mirage).
    // kind: "av" (default) = access violation on the main thread,
    //       "terminate"    = std::terminate,
    //       "abort"        = std::abort (signal path).
    // The process dies inside the handler, so no response is ever sent — the
    // caller sees the connection drop, then verifies the dump + stderr output.
    server.RegisterHandler("debug_crash", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string kind = ctx.params.value("kind", "av");
        if (kind == "av")
        {
            volatile int* ptr = nullptr;
            *ptr = 42; // intentional AV → UnhandledSEHFilter
        }
        else if (kind == "terminate")
        {
            std::terminate(); // → TerminateHandler
        }
        else if (kind == "abort")
        {
            std::abort(); // → SIGABRT → SignalHandler
        }
        return Editor::RefuseRequest("Unknown crash kind: " + kind + " (expected av|terminate|abort)"); });
}

} // namespace GameEngine
