#pragma once

#include "ECS/Reflection.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::WebLibrary
{

/// A reflected component as a page sees it: what ge_reflection_json lists and what the
/// component and field calls accept. Editor-only components are not part of a game, and hidden,
/// transient and untyped fields have no page-facing form, so all are left out (the same rule as
/// the component scanner's --emit-json).
struct PageComponent
{
    std::uint64_t TypeId = 0;
    /// The unqualified name (`Light`).
    std::string_view Name;
    /// In declaration order; a field's index here is the field id the ABI takes.
    std::vector<const ECS::FieldInfo*> Fields;
};

/// The component `typeId` names, or null when it is not a page component. Built on a type's first
/// lookup and kept, so the per-frame field calls allocate nothing; rebuilt when the registry
/// re-points the type's field table. Valid until the next call. Main thread only, as every ABI call.
const PageComponent* FindPageComponent(std::uint64_t typeId);

/// Every page component, sorted by name.
std::vector<PageComponent> ListPageComponents();

/// The bytes one element of a field of `type` takes; 1 for the byte-run kinds (String, Bytes).
std::uint32_t ElementSize(ECS::FieldTypeId type);

/// The FieldTypeId name (ECS/Reflection.h), as abi.ts's FieldKind spells it.
std::string_view KindName(ECS::FieldTypeId type);

/// The page components as abi.ts's ReflectionJson document.
std::string BuildReflectionJson();

} // namespace GameEngine::WebLibrary
