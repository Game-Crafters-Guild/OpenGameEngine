#pragma once

// UserModuleHandle — RAII wrapper over an OS dynamic-library handle.
//
// Loads a built user-script DLL (LoadLibraryW / dlopen), resolves its exported
// symbols (GetProcAddress / dlsym), and frees it on destruction (FreeLibrary /
// dlclose). Move-only. Engine-internal — NOT part of the user-facing ABI.
//
// C9 defines it; it is first used when C10 compiles and loads a user DLL and
// resolves the four NativeScriptingABI exports. The native OS handle is held as an
// opaque void* so this header stays free of <windows.h>.

#include "Types/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

class UserModuleHandle
{
public:
    UserModuleHandle() = default;
    explicit UserModuleHandle(const std::filesystem::path& path);
    ~UserModuleHandle();

    UserModuleHandle(const UserModuleHandle&) = delete;
    UserModuleHandle& operator=(const UserModuleHandle&) = delete;
    UserModuleHandle(UserModuleHandle&& other) noexcept;
    UserModuleHandle& operator=(UserModuleHandle&& other) noexcept;

    bool IsValid() const { return m_Handle != nullptr; }
    const std::filesystem::path& Path() const { return m_Path; }

    // Empty on success; the OS error string when the last load failed.
    const std::string& LastError() const { return m_LastError; }

    // Resolve an exported symbol by name; null if the module is invalid or the
    // symbol is absent.
    void* GetSymbol(const char* name) const;

    template <class Fn>
    Fn GetFunction(const char* name) const
    {
        return reinterpret_cast<Fn>(GetSymbol(name));
    }

    // The mapped image's address range, {0, 0} when the module is not loaded (or
    // when the platform cannot report a span). Everything the image owns —
    // code, vtables, type_info, constant data — lies inside it, so it is the
    // exact test for "does this pointer die when this module is unmapped".
    //
    // Read straight out of the mapped headers rather than through the loader:
    // callers resolve addresses while a module is being mapped, which on Windows
    // means running inside DLL_PROCESS_ATTACH, where taking the loader lock
    // again is a deadlock waiting to happen.
    std::uint64_t ImageBase() const;
    std::uint64_t ImageSize() const;

    // Free the library now (also done by the destructor). IsValid() is false after.
    void Reset();

private:
    void* m_Handle = nullptr; // HMODULE (Windows) / dlopen handle (POSIX)
    std::filesystem::path m_Path;
    std::string m_LastError;
};

} // namespace NativeScripting
} // namespace GameEngine
