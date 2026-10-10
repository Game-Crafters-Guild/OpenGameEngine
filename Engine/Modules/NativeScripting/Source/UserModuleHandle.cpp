#include "NativeScripting/UserModuleHandle.h"

#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace GameEngine
{
namespace NativeScripting
{

#if defined(_WIN32)
namespace
{
std::string FormatLastWin32Error()
{
    const DWORD code = ::GetLastError();
    if (code == 0)
        return {};
    LPSTR buffer = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = (len && buffer) ? std::string(buffer, len) : "unknown error";
    if (buffer)
        ::LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
        message.pop_back();
    return message;
}
} // namespace
#endif

UserModuleHandle::UserModuleHandle(const std::filesystem::path& path)
    : m_Path(path)
{
#if defined(_WIN32)
    m_Handle = ::LoadLibraryW(path.c_str());
    if (!m_Handle)
        m_LastError = FormatLastWin32Error();
#else
    m_Handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_Handle)
    {
        const char* err = ::dlerror();
        m_LastError = err ? err : "dlopen failed";
    }
#endif
}

UserModuleHandle::~UserModuleHandle()
{
    Reset();
}

UserModuleHandle::UserModuleHandle(UserModuleHandle&& other) noexcept
    : m_Handle(other.m_Handle)
    , m_Path(std::move(other.m_Path))
    , m_LastError(std::move(other.m_LastError))
{
    other.m_Handle = nullptr;
}

UserModuleHandle& UserModuleHandle::operator=(UserModuleHandle&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        m_Handle = other.m_Handle;
        m_Path = std::move(other.m_Path);
        m_LastError = std::move(other.m_LastError);
        other.m_Handle = nullptr;
    }
    return *this;
}

void* UserModuleHandle::GetSymbol(const char* name) const
{
    if (!m_Handle || !name)
        return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m_Handle), name));
#else
    return ::dlsym(m_Handle, name);
#endif
}

std::uint64_t UserModuleHandle::ImageBase() const
{
#if defined(_WIN32)
    // An HMODULE IS the image base on Windows.
    return reinterpret_cast<std::uint64_t>(m_Handle);
#else
    // A dlopen handle is not the base; Dl_info on any symbol of the module is.
    if (!m_Handle)
        return 0;
    Dl_info info{};
    void* anySymbol = ::dlsym(m_Handle, "GE_UserModule_AbiVersion_v1");
    if (!anySymbol || !::dladdr(anySymbol, &info))
        return 0;
    return reinterpret_cast<std::uint64_t>(info.dli_fbase);
#endif
}

std::uint64_t UserModuleHandle::ImageSize() const
{
#if defined(_WIN32)
    if (!m_Handle)
        return 0;
    const auto* base = static_cast<const unsigned char*>(m_Handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt->OptionalHeader.SizeOfImage;
#else
    // No portable span query. Native module hot-swapping is a Windows developer
    // loop today (the editor's build/reload path); elsewhere a zero span
    // publishes no range, so nothing is ever stamped and behaviour is unchanged.
    return 0;
#endif
}

void UserModuleHandle::Reset()
{
    if (!m_Handle)
        return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(m_Handle));
#else
    ::dlclose(m_Handle);
#endif
    m_Handle = nullptr;
}

} // namespace NativeScripting
} // namespace GameEngine
