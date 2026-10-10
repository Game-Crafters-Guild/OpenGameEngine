#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace GameEngine
{

// Typed error code for asset operations. Used by every asset Load/Resolve/
// Register API that can fail. Replaces the historical pattern of a nullable
// Asset pointer plus a `std::string error` parameter — the typed enum gives
// callers something they can switch on without parsing strings, and removes
// the silent "did the caller forget to check?" pitfall of nullable returns.
enum class AssetError
{
    Missing,           // Registry has no entry for this GUID/path, or the entry is tombstoned.
    ImportFailed,      // File exists but the importer rejected it (bad magic, parse error, etc.).
    MountUnavailable,  // The mount that owns this asset is unregistered, read-only, or shutting down.
    Cancelled,         // The load was cancelled by the caller, or the engine is shutting down.
};

// Cheap stable string for logs and diagnostics.
const char* ToString(AssetError e) noexcept;

// Result<T, E> — holds either a successful value of type T or an error of
// type E. No heap allocation beyond what T or E themselves require. Designed
// for hot-path uses where exceptions are unaffordable; the asset Load API
// in particular is called thousands of times per frame.
//
// Construction:
//   Result<AssetHandle> r1 = std::move(handle);   // success
//   Result<AssetHandle> r2 = AssetError::Missing; // failure
//
// Inspection:
//   if (r) { ... use r.Value() ... }
//   else { Logger::Warning("load failed: {}", ToString(r.Error())); }
//
// Move-friendly: T and E may be move-only types (e.g. AssetHandle holding
// a unique resource). Copy is supported only when both T and E are copyable.
template<typename T, typename E = AssetError>
class Result
{
    static_assert(!std::is_reference_v<T>, "Result<T,E>: T cannot be a reference");
    static_assert(!std::is_reference_v<E>, "Result<T,E>: E cannot be a reference");
    static_assert(!std::is_same_v<T, E>, "Result<T,E>: T and E must be distinct types");

public:
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : m_Storage(std::in_place_index<0>, std::move(value)) {}

    Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : m_Storage(std::in_place_index<1>, std::move(error)) {}

    // No default ctor: a Result must explicitly be constructed Ok or Err.

    bool IsOk() const noexcept { return m_Storage.index() == 0; }
    bool IsErr() const noexcept { return m_Storage.index() == 1; }
    explicit operator bool() const noexcept { return IsOk(); }

    T& Value() & { return std::get<0>(m_Storage); }
    const T& Value() const& { return std::get<0>(m_Storage); }
    T&& Value() && { return std::move(std::get<0>(m_Storage)); }

    E& Error() & { return std::get<1>(m_Storage); }
    const E& Error() const& { return std::get<1>(m_Storage); }
    E&& Error() && { return std::move(std::get<1>(m_Storage)); }

    // Returns the contained value, or the supplied default if this is an error.
    // The default is evaluated lazily only when needed.
    template<typename U>
    T ValueOr(U&& fallback) const&
    {
        return IsOk() ? Value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> m_Storage;
};

} // namespace GameEngine
