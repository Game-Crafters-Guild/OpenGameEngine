#pragma once

// Teaches GE_REFLECT about the engine's Mathematics vector/quaternion types.
//
// FieldTypeIdOf<T>() consults FieldTypeTraits<T> before the scalar/array ladder,
// so specializing it here maps a reflected Vector3 member to FieldTypeId::Vec3
// (one 12-byte composite) instead of falling through to Unknown. ColorLinear and
// EntityHandle already have their traits in ECS/Reflection.h; these are the
// Mathematics counterparts.
//
// This is a deliberate BRIDGE header in the Components layer, not in ECS or in
// Mathematics: ECS must not depend on Mathematics (ECS/Reflection.h stays
// Mathematics-free), and Mathematics is a foundation module that must not depend
// on ECS. Components already includes both, so the specializations live here and
// are pulled in by ComponentRegistration.h — making them visible at every
// GE_REGISTER_COMPONENT site (including the generated reflection TU) before the
// macro expands and evaluates FieldTypeIdOf for each field.

#include "ECS/Reflection.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Mathematics/Quaternion.h"

namespace GameEngine {
namespace ECS {

template <>
struct FieldTypeTraits<Mathematics::Vector2>
{
    static constexpr FieldTypeId kType = FieldTypeId::Vec2;
};

template <>
struct FieldTypeTraits<Mathematics::Vector3>
{
    static constexpr FieldTypeId kType = FieldTypeId::Vec3;
};

template <>
struct FieldTypeTraits<Mathematics::Vector4>
{
    static constexpr FieldTypeId kType = FieldTypeId::Vec4;
};

template <>
struct FieldTypeTraits<Mathematics::Quaternion>
{
    static constexpr FieldTypeId kType = FieldTypeId::Quat;
};

// A composite's hardcoded FieldElementSize must equal the mapped C++ type's byte
// layout, or array-count derivation (count = Size / FieldElementSize) silently
// truncates. Pin each here, where both the size table and the Math types are
// visible (ECS/Reflection.h cannot see Mathematics).
static_assert(FieldElementSize(FieldTypeId::Vec2) == sizeof(Mathematics::Vector2),
              "FieldElementSize(Vec2) must equal sizeof(Mathematics::Vector2)");
static_assert(FieldElementSize(FieldTypeId::Vec3) == sizeof(Mathematics::Vector3),
              "FieldElementSize(Vec3) must equal sizeof(Mathematics::Vector3)");
static_assert(FieldElementSize(FieldTypeId::Vec4) == sizeof(Mathematics::Vector4),
              "FieldElementSize(Vec4) must equal sizeof(Mathematics::Vector4)");
static_assert(FieldElementSize(FieldTypeId::Quat) == sizeof(Mathematics::Quaternion),
              "FieldElementSize(Quat) must equal sizeof(Mathematics::Quaternion)");

} // namespace ECS
} // namespace GameEngine
