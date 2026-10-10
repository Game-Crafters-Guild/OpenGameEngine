#pragma once

#include <string>
#include <string_view>

// Which Field<T> value types reach a script, and how they encode into the event payload.
//
// This is the one place that answers "does this control raise a value event at all". A type
// with no specialisation below notifies its member callbacks and dispatches NOTHING, because an
// event whose Value is a meaningless zero cannot be told apart from a real zero — a subscriber
// would have no way to know it had been lied to.
//
// Its own header, rather than a block inside BaseField.h, because the set of encodable value
// types is a contract two unrelated places have to agree on, and a reader looking for that
// contract should not have to find it inside a 400-line control base class.
//
// ADDING A SPECIALISATION IS A TWO-FILE CHANGE. The scripting ABI's registration guard
// (Engine/Source/Scripting/UIElementABI.cpp, ElementDispatchesEvent) decides which element types
// may be subscribed to by dynamic_cast against this same set. Teaching one and not the other
// either refuses a subscription that would work, or mints one that can never fire.
//
// TWO PAYLOAD SHAPES, MUTUALLY EXCLUSIVE. A numeric type encodes into UIEvent::Value as a
// float (kNumericPayload with Encode). A string rides UIEvent::Text as UTF-8 bytes
// (kTextPayload with Text), behind the dispatch-window copy that keeps the bytes stable
// however a handler mutates the field (ScopedValueText — the lifetime design lives on that
// class). Exactly one flag is true per dispatching type, and each one's function exists only
// where its flag does: a type claiming both would write Value and Text in the same event, and
// a type claiming a shape it has no function for fails to compile at the dispatch site.
//
// Deliberately absent, with reasons, so the gaps read as decisions:
//   * int — int32 stops being exactly representable in a float above 2^24, so a value event
//     would silently report a different number than the control holds. It needs a properly
//     typed payload of its own.
//   * Vector3 and friends — no payload shape exists for them yet.

namespace GameEngine
{

template <typename T>
struct FieldEventValue
{
    static constexpr bool kNumericPayload = false;
    static constexpr bool kTextPayload = false;
};

template <>
struct FieldEventValue<float>
{
    static constexpr bool kNumericPayload = true;
    static constexpr bool kTextPayload = false;
    static float Encode(float v) { return v; }
};

template <>
struct FieldEventValue<bool>
{
    static constexpr bool kNumericPayload = true;
    static constexpr bool kTextPayload = false;
    static float Encode(bool v) { return v ? 1.0f : 0.0f; }
};

// TextField's text, and a Dropdown's selected option VALUE (its Field<std::string> value —
// the label is presentation, not the value).
template <>
struct FieldEventValue<std::string>
{
    static constexpr bool kNumericPayload = false;
    static constexpr bool kTextPayload = true;
    static std::string_view Text(const std::string& v) { return v; }
};

} // namespace GameEngine
