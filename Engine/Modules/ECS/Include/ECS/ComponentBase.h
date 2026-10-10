#pragma once

// ECS::ComponentBase — an empty, OPTIONAL tag base for the no-boilerplate component
// path. Inheriting it (IN A HEADER) marks a struct as a component for the build-time
// scanner, which then auto-generates its reflection + registration (no GE_REFLECT / no
// GE_REGISTER_COMPONENT, no hand-listed fields):
//
//     struct Health : ECS::ComponentBase { float Current = 100.0f; float Max = 100.0f; };
//
// Must be in a header (.h/.hpp): the scanner reflects headers (the generated TU #includes
// them); a struct inheriting this in a .cpp is diagnosed, not registered.
//
// It is NOT what *makes* a type a component — the `Component` concept (any
// trivially-copyable, standard-layout POD) is the real definition; this is purely a
// detection hint. Components declared with the explicit macros inherit nothing.
//
// The base is empty, so via empty-base optimization it costs zero bytes and preserves
// the trivially-copyable + standard-layout properties the `Component` concept requires
// (the component's type id, a hash of its type name, is unaffected by the base).

namespace GameEngine
{
namespace ECS
{
struct ComponentBase
{
};
} // namespace ECS
} // namespace GameEngine
