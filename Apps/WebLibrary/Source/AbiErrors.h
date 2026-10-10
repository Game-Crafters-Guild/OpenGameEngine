#pragma once

// The failure side of the WebLibrary C ABI (Apps/WebLibrary/ts/src/abi.ts): the return codes,
// the message ge_last_error hands back, the strings a call returns, and the one call-order rule
// the module enforces itself: no export runs while another is suspended.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace GameEngine::WebLibrary
{

/// What every call that returns an error code returns on success.
inline constexpr int32_t kOk = 0;
/// What every call that returns an error code returns on failure; ge_last_error says why.
inline constexpr int32_t kFailed = -1;
/// ge_load_asset's failure handle.
inline constexpr uint32_t kInvalidAsset = 0;

/// Records `message` as the last error; ge_last_error returns it until the next failure.
void SetLastError(std::string message);

template <class... Args>
void SetLastError(std::format_string<Args...> format, Args&&... args)
{
    SetLastError(std::format(format, std::forward<Args>(args)...));
}

/// One export's run, from entry to return. JavaScript is single-threaded, so another export can
/// only arrive while this one is suspended under ASYNCIFY (ge_create and ge_shutdown wait on the
/// browser; any call that reaches a GPU wait can), and an unwind does not destroy this object:
/// the mark stays set for the whole suspension. A call made in that window is refused, with
/// the last error naming both calls, because it would run on top of the suspended stack.
/// ge_last_error takes no scope: it is callable at any time.
class AbiCallScope
{
public:
    explicit AbiCallScope(std::string_view call);
    ~AbiCallScope();
    AbiCallScope(const AbiCallScope&) = delete;
    AbiCallScope& operator=(const AbiCallScope&) = delete;

    /// True when another export was in progress: the caller returns its failure value at once.
    bool Refused() const { return m_Refused; }

private:
    bool m_Refused = false;
};

/// Keeps `text` alive until the next ABI call that returns a string, and returns it as the
/// NUL-terminated pointer the call hands to JavaScript.
const char* ReturnString(std::string text);

} // namespace GameEngine::WebLibrary
