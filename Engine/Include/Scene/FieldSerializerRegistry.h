#pragma once

// FieldSerializerRegistry — the text<->bytes codec lookup for reflected component fields.
//
// The reflection-driven scene serializer (ReflectionSceneSchema) walks a component's
// ComponentFieldRegistry field table and, for each field, asks this registry for an
// IFieldSerializer to convert the field's raw bytes to/from the scene's "value" text.
//
// Dispatch is a flat, file-scope table indexed by FieldTypeId over stateless static serializer
// instances (no heap, no map, no mutex). An enum field is detected by a non-empty FieldInfo::EnumNames
// (the scanner-emitted constexpr name table) and routed to the shared name codec — no per-field
// registration. There is intentionally no runtime registration API: a new kind of field is a new
// static serializer + a table entry, decided at compile time.

#include "ECS/Reflection.h" // FieldInfo, FieldTypeId

#include <cstddef>
#include <string>

namespace GameEngine::Scene
{

struct SceneSaveContext;
struct SceneLoadContext;

// Converts ONE reflected field's bytes <-> scene "value" text (the right-hand side of
// "Component.field = value", with no prefix and no trailing newline). The field bytes are the
// span [FieldInfo::Offset, +FieldInfo::Size) inside a captured component byte buffer.
class IFieldSerializer
{
  public:
    virtual ~IFieldSerializer() = default;

    // Format the field at `fieldBytes` as scene-value text.
    virtual std::string Write(const std::byte* fieldBytes,
                              const ECS::FieldInfo& info,
                              const SceneSaveContext& ctx) const = 0;

    // Parse `text` into the field at `fieldBytes`. Return false + set *err on malformed input.
    virtual bool Read(std::string_view text,
                      std::byte* fieldBytes,
                      const ECS::FieldInfo& info,
                      const SceneLoadContext& ctx,
                      std::string* err) const = 0;
};

class FieldSerializerRegistry
{
  public:
    // Resolve the codec for a field: the enum name codec when FieldInfo::EnumNames is set, else the
    // flat default keyed by FieldInfo::Type. Null when the type has no serializer (Mat4 / Bytes) —
    // the caller skips the field with a warning.
    static const IFieldSerializer* Resolve(const ECS::FieldInfo& info);
};

} // namespace GameEngine::Scene
