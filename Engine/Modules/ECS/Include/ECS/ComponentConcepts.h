#pragma once

#include "ECS/ECS.h"  // For the base Component concept
#include "ECS/Components.h"  // For built-in component types
#include <type_traits>
#include <concepts>

namespace GameEngine::ECS {

    // Qualifier wrappers (semantic-only initially)
    template<typename T> struct Read { using Type = T; };
    template<typename T> struct Write { using Type = T; };
    template<typename T> struct Optional { using Type = T; };

    // Trait to unwrap qualifiers
    template<typename T> struct UnderlyingType { using type = T; };
    template<typename T> struct UnderlyingType<Read<T>> { using type = T; };
    template<typename T> struct UnderlyingType<Write<T>> { using type = T; };
    template<typename T> struct UnderlyingType<Optional<T>> { using type = T; };

    template<typename T>
    using Underlying = typename UnderlyingType<T>::type;

    // Qualifier detection
    template<typename T> struct IsRead : std::false_type {};
    template<typename T> struct IsRead<Read<T>> : std::true_type {};
    template<typename T> struct IsWrite : std::false_type {};
    template<typename T> struct IsWrite<Write<T>> : std::true_type {};
    template<typename T> struct IsOptional : std::false_type {};
    template<typename T> struct IsOptional<Optional<T>> : std::true_type {};

    // Qualified component concept: underlying type must satisfy Component
    template<typename T>
    concept QualifiedComponent = Component<Underlying<T>>;

} // namespace GameEngine::ECS
