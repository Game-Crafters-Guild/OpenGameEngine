#pragma once

// Per-parameter read/write inference for Query callbacks (change-signaling
// design §4.3). The rule: parameter I is a READ iff the functor is invocable
// with parameter I's argument made const (others unchanged). Inferred and
// declared reads are then INVOKED as const — a body that actually writes
// fails to compile instead of silently skipping the stamp.
//
// Enforcement (all static_asserts here are evaluated for every slot before
// any invocation shape is selected — closing the dispatch-lattice hole, M4a):
//   - Read<T> + a parameter that cannot bind const  -> compile error
//     (contradiction: declared read, mutable access).
//   - generic/deduced parameter (auto&, auto&&, auto*, const auto&, ...) on
//     an UNQUALIFIED type -> compile error: a system declares its intent
//     through concrete parameter types or Read<T>/Write<T> qualifiers, and
//     an auto& parameter is never taken as a declared write.
//   - With the type qualified, deduced parameters bind per declared intent
//     (Read -> const, so a write in the body still fails loudly).
//
// Deduced-parameter detection: probe slot I with ProbeAlienTag (a type
// nothing converts to). Only a deduced parameter can bind it.

#include "ECS/ComponentConcepts.h"
#include "ECS/ECS.h"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace GameEngine::ECS::Detail
{

// Namespace-scope probe type (MSVC C5046: probe types in template machinery
// must not be local classes). Not constructible, converts to/from nothing.
struct ProbeAlienTag
{
    ProbeAlienTag() = delete;
};

template <std::size_t I, typename T0, typename... Rest>
struct NthTypeHelper
{
    using type = typename NthTypeHelper<I - 1, Rest...>::type;
};
template <typename T0, typename... Rest>
struct NthTypeHelper<0, T0, Rest...>
{
    using type = T0;
};
template <std::size_t I, typename... Ts>
using NthTypeOf = typename NthTypeHelper<I, Ts...>::type;

// Argument forms per qualified slot type Q.
// Reference shape (Each / Parallel): Optional slots pass pointers.
//   Plain — the permissive form every legal concrete parameter binds.
//   Const — the probe/invocation form for reads.
//   Alien — the deduced-parameter detector.
template <typename Q>
using PlainRefArg = std::conditional_t<IsOptional<Q>::value, Underlying<Q>*, Underlying<Q>&>;
template <typename Q>
using ConstRefArg =
    std::conditional_t<IsOptional<Q>::value, const Underlying<Q>*, const Underlying<Q>&>;
template <typename Q>
using AlienRefArg = std::conditional_t<IsOptional<Q>::value, ProbeAlienTag*, ProbeAlienTag&>;

// Pointer shape (BatchEach / ParallelBatchEach / ForEachChunk / ParallelChunks).
template <typename Q>
using PlainPtrArg = Underlying<Q>*;
template <typename Q>
using ConstPtrArg = const Underlying<Q>*;
template <typename Q>
using AlienPtrArg = ProbeAlienTag*;

// Effective invocation argument per slot, chosen by the computed read bit.
template <bool IsReadSlot, typename Q>
using EffRefArg = std::conditional_t<IsReadSlot, ConstRefArg<Q>, PlainRefArg<Q>>;
template <bool IsReadSlot, typename Q>
using EffPtrArg = std::conditional_t<IsReadSlot, ConstPtrArg<Q>, PlainPtrArg<Q>>;

template <bool IsReadSlot, typename UT>
constexpr auto EffPtrCast(UT* p)
{
    if constexpr (IsReadSlot)
        return static_cast<const UT*>(p);
    else
        return p;
}

// The callback shapes the iteration surface dispatches.
enum class CallShape
{
    EntityRef,      // func(EntityHandle, args...)              — Each / Parallel
    PlainRef,       // func(args...)                            — Each component-only
    PtrCount,       // func(ptrs..., size_t)                    — BatchEach / adapter with count
    EntityPtrCount, // func(const EntityHandle*, ptrs..., size_t) — BatchEach entity-aware
    Ptr             // func(ptrs...)                            — adapter without count
};

// Invocability of Func with slot I's argument substituted by SubstArg and all
// other slots in their permissive Plain form, under the given shape.
template <typename Func, CallShape Shape, template <typename> class SubstArg,
          template <typename> class OtherArg, std::size_t I, typename... Qs>
struct SlotProbe
{
    template <std::size_t J>
    using ArgAt = std::conditional_t<J == I, SubstArg<NthTypeOf<J, Qs...>>,
                                     OtherArg<NthTypeOf<J, Qs...>>>;

    template <std::size_t... Js>
    static constexpr bool Test(std::index_sequence<Js...>)
    {
        if constexpr (Shape == CallShape::EntityRef)
            return std::is_invocable_v<Func, EntityHandle, ArgAt<Js>...>;
        else if constexpr (Shape == CallShape::PtrCount)
            return std::is_invocable_v<Func, ArgAt<Js>..., std::size_t>;
        else if constexpr (Shape == CallShape::EntityPtrCount)
            return std::is_invocable_v<Func, const EntityHandle*, ArgAt<Js>..., std::size_t>;
        else
            return std::is_invocable_v<Func, ArgAt<Js>...>;
    }

    static constexpr bool value = Test(std::index_sequence_for<Qs...>{});
};

template <typename Func, CallShape Shape, typename... Qs>
struct AccessProbe
{
    static constexpr bool kPtrShape = (Shape == CallShape::PtrCount ||
                                       Shape == CallShape::EntityPtrCount || Shape == CallShape::Ptr);

    template <typename Q>
    using PlainA = std::conditional_t<kPtrShape, PlainPtrArg<Q>, PlainRefArg<Q>>;
    template <typename Q>
    using ConstA = std::conditional_t<kPtrShape, ConstPtrArg<Q>, ConstRefArg<Q>>;
    template <typename Q>
    using AlienA = std::conditional_t<kPtrShape, AlienPtrArg<Q>, AlienRefArg<Q>>;

    template <std::size_t I>
    static constexpr bool kDeduced =
        SlotProbe<Func, Shape, AlienA, PlainA, I, Qs...>::value;

    template <std::size_t I>
    static constexpr bool kConstBindable =
        SlotProbe<Func, Shape, ConstA, PlainA, I, Qs...>::value;

    // Per-slot classification. The static_asserts are the enforcement lattice
    // (M4a): they instantiate while computing kReadMask, i.e. before any
    // invocation shape is selected, so no fallback path can bypass them.
    template <std::size_t I>
    static constexpr bool SlotIsRead()
    {
        using Q = NthTypeOf<I, Qs...>;
        if constexpr (IsRead<Q>::value)
        {
            static_assert(kConstBindable<I>,
                          "ECS Query: Read<T>-declared component is taken mutably by the "
                          "callback. A declared read must bind const (const T& / const T*) -- "
                          "fix the parameter or declare Write<T>.");
            return true;
        }
        else if constexpr (IsWrite<Q>::value)
        {
            return false;
        }
        else
        {
            static_assert(!kDeduced<I>,
                          "ECS Query: generic/deduced callback parameter (auto&/auto*/...) on an "
                          "unqualified component type. Declare intent: use a concrete const T&/T& "
                          "(const T*/T* for Optional and batch shapes) parameter, or qualify the "
                          "query type with ECS::Read<T>/ECS::Write<T>.");
            return kConstBindable<I>;
        }
    }

    template <std::size_t... Is>
    static constexpr uint32_t ReadMaskImpl(std::index_sequence<Is...>)
    {
        uint32_t mask = 0;
        ((mask |= (SlotIsRead<Is>() ? (1u << Is) : 0u)), ...);
        return mask;
    }

    static_assert(sizeof...(Qs) <= 32, "AccessProbe masks are 32-bit");

    // Effective access sets (M13): the single source of truth for stamping.
    // The ctor-built qualifier sets remain only a declared upper bound.
    static constexpr uint32_t kReadMask = ReadMaskImpl(std::index_sequence_for<Qs...>{});
    static constexpr uint32_t kWriteMask =
        static_cast<uint32_t>(~kReadMask) &
        ((sizeof...(Qs) == 0) ? 0u : (0xFFFFFFFFu >> (32 - sizeof...(Qs))));
};

} // namespace GameEngine::ECS::Detail
