#pragma once

// PostProcessEffectRegistry — the EffectDescriptor registry.
//
// One registration per post-process effect replaces the per-effect edits that
// were scattered across the scene schemas, inspector chrome, and settings
// plumbing:
//   - Scene layer: per-field serialization metadata — exactly the four
//     reflection-path gaps that blocked the Phase 0 plain-schema collapse
//     (float[3] encodings, load-time clamps, enum-as-int on-disk contracts,
//     legacy key renames) — so a migrated effect keeps its on-disk format and
//     load-clamp behavior while its hand-written schema is deleted.
//   - Inspector chrome: display name, tooltip, icon class, package gate — the
//     InspectorPanel per-effect tables read the descriptor instead.
//   - Settings-write: the shader-name -> resolved-PostProcessSettings-member
//     tables (successor of the PP_SHARED_FIELDS X-macro) that drive
//     TryWriteField / TryReadField / ReadableFieldNames.
//   - Skip gates: the computed read-only "<effect>Active" names a render-graph
//     skipWhen reads, each with its predicate, consumed by TryReadField and
//     ReadableFieldNames.
//   - Blend + extraction (Phase 2): each SettingsFields entry carries a blend
//     rule and contributes-group, and BlendPostProcessSettings /
//     CopySettingsGroup fold over the tables; the Neutralize/Extract hooks
//     fold each effect component into the extracted volume settings.

#include "ECS/ComponentFieldRegistry.h" // ECS::ComponentTypeId

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
} // namespace GameEngine::ECS

namespace GameEngine::Engine::Renderer
{
struct PostProcessExtractContext;
struct PostProcessExtractedVolume;
struct PostProcessSettings;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Rendering
{

// Scene-IO metadata for ONE reflected field of an effect component. Only
// fields that need something beyond plain reflection are listed; unlisted
// fields serialize through the generic reflection path untouched.
struct EffectFieldIO
{
    std::string_view FieldName;       // reflected member name (FieldInfo::Name)
    std::string_view SerializedKey{}; // on-disk key when it differs from FieldName (e.g. "longitudinal")

    // Load-time clamp applied after parse: mirrors what the hand schemas and
    // render extraction enforce, so a hand-edited scene cannot feed the shader
    // out-of-range values. float bounds are exact for the small integer/enum
    // ranges they also serve; FLT_MAX as ClampMax means "no upper bound".
    bool HasClamp{false};
    float ClampMin{0.0f};
    float ClampMax{0.0f};

    // float[3] encodings (mutually exclusive):
    bool SplitRgb{false};  // three scalar lines keyed <key>R / <key>G / <key>B
    bool TupleVec3{false}; // one "(x, y, z)" line

    // Serialize the enum's underlying integer (the shipped on-disk contract)
    // instead of the enumerator name the generic enum codec writes.
    bool EnumAsInt{false};
    // Out-of-clamp-range integers snap to this value instead of clamping —
    // e.g. a future ShadowSettings mode falls back to Cascades.
    bool HasEnumFallback{false};
    std::int64_t EnumFallbackValue{0};

    // Legacy load-only field: accepted from file, never written on save.
    bool SkipSerialize{false};
};

// How BlendPostProcessSettings folds one settings member across two volumes
// (PP-ARCH Phase 2). Float members default to Lerp, int members to Dominant;
// the exceptions are declared per entry.
enum class SettingsBlendRule : std::uint8_t
{
    Lerp,     // a + (b - a) * w — continuous controls
    Dominant, // w >= 0.5 ? b : a — discrete enums/toggles (highest priority wins)
    Log2Lerp, // stop-space (geometric) lerp — exposure
    Skip,     // never blended: derived per view or stamped after blending
};

// Contributes-gate for grouped members: when the incoming volume does not
// author the group (no such effect component), the blend keeps a's values for
// every member of the group — an unrelated grading volume must not fade fog
// toward its defaults. FogGlow is shared by both fog effects and gates on
// either one contributing.
enum class SettingsBlendGroup : std::uint8_t
{
    Always,
    ColorGrade,
    HeightFog,
    VolumetricFog,
    AtmosphericCloud,
    VolumetricClouds,
    FogGlow,
};

// One member of the resolved Engine::Renderer::PostProcessSettings that this
// effect authors. Named entries are shader-visible: TryWriteField /
// TryReadField / ReadableFieldNames walk them, so an effect's push-constant +
// skipWhen names live in its registration instead of a central X-macro.
// Entries with an EMPTY ShaderName are blend-only — members the volume blend
// folds but no shader reads by name (fog interiors, AO, exposure modifiers).
// Offsets are into the resolved settings struct (the blended output), NOT
// into the effect component.
struct EffectSettingsField
{
    enum class Kind : std::uint8_t { Float32, Int32 };

    std::string_view ShaderName; // push-constant / skipWhen name; empty = blend-only
    std::uint16_t Offset{0};     // offsetof into PostProcessSettings
    Kind Type{Kind::Float32};
    SettingsBlendRule Blend{SettingsBlendRule::Lerp};
    SettingsBlendGroup Group{SettingsBlendGroup::Always};
};

// One computed, read-only skipWhen gate: TryReadField answers ShaderName with
// IsActive(settings) as 1.0 / 0.0, so a pass skips when its effect cannot
// contribute to the image rather than when one raw member is zero.
struct EffectSettingsGate
{
    using PredicateFn = bool (*)(const Engine::Renderer::PostProcessSettings& settings);

    std::string_view ShaderName;
    PredicateFn IsActive = nullptr;
};

// One post-process effect's registration.
struct PostProcessEffectDescriptor
{
    ECS::ComponentTypeId Type{};
    std::string_view ComponentName; // unqualified scene-schema name, e.g. "VignetteEffect"

    // Inspector chrome. DisplayName doubles as the Add-Effect menu label and
    // the "Add <name>" undo caption; Description is the section-header tooltip;
    // IconPath is an asset path for the section-header and Add-Effect menu icon,
    // resolved like a CSS background-image (editor mount when unprefixed);
    // RequiredPackage (when set) drives the "Package Not Installed" header badge.
    std::string_view DisplayName;
    std::string_view Description{};
    std::string_view IconPath{};
    std::string_view RequiredPackage{};

    // Scene-IO metadata. Effects that keep a hand-written schema register with
    // an empty FieldIO: hand schemas win name resolution in SceneSchemaRegistry,
    // so a descriptor is scene-IO-inert for them — it exists for the chrome and
    // settings-write consumers above.
    std::span<const EffectFieldIO> FieldIO{};
    // Retired keys older scenes may still carry: parsed and dropped without a
    // warning (e.g. ColorGrade brightness/gamma/hue).
    std::span<const std::string_view> DroppedLegacyKeys{};

    std::span<const EffectSettingsField> SettingsFields{};

    // Gates this effect alone decides. A gate that combines several effects
    // (bloom chain, fog glow, the HDR and LDR stacks) lives in the core gate
    // table in PostProcessSettings.cpp.
    std::span<const EffectSettingsGate> Gates{};

    // Extraction (PP-ARCH Phase 2). Extract reads this effect's component off
    // the volume entity (present + Enabled gating included — Deband's
    // present-but-disabled fold is a per-effect policy) and folds it into the
    // extracted volume; the volume-core spatial fields are already stamped.
    // NeutralizeSettings runs for EVERY volume before the folds: effects whose
    // struct defaults are not "authored off" (volumetric fog, clouds) baseline
    // them here so a volume without the effect doesn't carry them.
    // RenderExtractionSystem invokes hooks in registration order — order-
    // sensitive folds (the shared fog-glow merge) rely on it.
    using ExtractFn = void (*)(ECS::World& world, const ECS::EntityHandle& entity,
                               const Engine::Renderer::PostProcessExtractContext& ctx,
                               Engine::Renderer::PostProcessExtractedVolume& out);
    using NeutralizeFn = void (*)(Engine::Renderer::PostProcessSettings& settings);
    ExtractFn Extract = nullptr;
    NeutralizeFn NeutralizeSettings = nullptr;
};

class PostProcessEffectRegistry
{
  public:
    // Register a descriptor. Non-owning: the descriptor's spans must point at
    // static storage. Idempotent per Type (last write wins).
    static void Register(const PostProcessEffectDescriptor& descriptor);

    // Descriptor for a component type, or null for non-effect components.
    // Ensures the built-in descriptors are registered first (linker-strip
    // proof), so any consumer may call it without an init-order dance.
    static const PostProcessEffectDescriptor* Find(ECS::ComponentTypeId type);

    // Field metadata by reflected field name, or null when the field has none.
    static const EffectFieldIO* FindFieldIO(const PostProcessEffectDescriptor& descriptor,
                                            std::string_view fieldName);

    // Visit every registered descriptor in registration order (built-ins are
    // registered first, in their RegisterBuiltInPostProcessEffectDescriptors
    // order). Descriptor references stay valid for the process lifetime; the
    // callback may call Find/FindFieldIO but must not Register.
    static void ForEach(const std::function<void(const PostProcessEffectDescriptor&)>& fn);

    // Settings-write lookup by shader-side name across every registration, or
    // null when no effect authors that name. Allocation-free: this sits on the
    // per-frame push-constant / skipWhen resolve path.
    static const EffectSettingsField* FindSettingsField(std::string_view shaderName);

    // Gate lookup by shader-side name across every registration, or null.
    // Allocation-free, like FindSettingsField: skipWhen resolves it per frame.
    static const EffectSettingsGate* FindGate(std::string_view shaderName);

    // Every settings field of every registration (named + blend-only), flat and
    // cached: BlendPostProcessSettings folds over this per volume per frame.
    // The span stays valid for the process lifetime (Register retires, never
    // frees, the cached table).
    static std::span<const EffectSettingsField> AllSettingsFields();
};

} // namespace GameEngine::Rendering
