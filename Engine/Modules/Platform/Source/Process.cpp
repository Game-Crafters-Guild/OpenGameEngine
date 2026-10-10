#include "Platform/Process.h"

#include <algorithm>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace GameEngine::Platform
{
namespace
{
bool IsValidName(std::string_view name)
{
    return !name.empty() && name.find('=') == std::string_view::npos;
}

bool AllNamesValid(std::span<const EnvironmentEdit> edits)
{
    return std::all_of(edits.begin(), edits.end(),
                       [](const EnvironmentEdit& edit) { return IsValidName(edit.Name); });
}
} // namespace

#if defined(_WIN32)
namespace
{
std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// The name part of an entry. A drive-directory entry ("=C:=C:\dir") starts with
// '=', so its name ends at the second one.
std::wstring_view EntryName(std::wstring_view entry)
{
    return entry.substr(0, entry.find(L'=', 1));
}

bool IsEntryFor(std::wstring_view entry, std::wstring_view name)
{
    return entry.size() > name.size() && entry[name.size()] == L'=' &&
           ::CompareStringOrdinal(entry.data(), static_cast<int>(name.size()), name.data(),
                                  static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

bool NameLess(const std::wstring& a, const std::wstring& b)
{
    const std::wstring_view nameA = EntryName(a);
    const std::wstring_view nameB = EntryName(b);
    return ::CompareStringOrdinal(nameA.data(), static_cast<int>(nameA.size()), nameB.data(),
                                  static_cast<int>(nameB.size()), TRUE) == CSTR_LESS_THAN;
}
} // namespace

std::optional<std::wstring> BuildChildEnvironmentBlock(std::span<const EnvironmentEdit> edits)
{
    if (!AllNamesValid(edits))
        return std::nullopt;
    if (edits.empty())
        return std::wstring{};

    std::vector<std::wstring> entries;
    if (LPWCH inherited = ::GetEnvironmentStringsW())
    {
        for (LPWCH entry = inherited; *entry != L'\0'; entry += ::wcslen(entry) + 1)
            entries.emplace_back(entry);
        ::FreeEnvironmentStringsW(inherited);
    }
    for (const EnvironmentEdit& edit : edits)
    {
        const std::wstring name = Utf8ToWide(edit.Name);
        std::erase_if(entries, [&name](const std::wstring& entry) { return IsEntryFor(entry, name); });
        if (edit.Value)
            entries.push_back(name + L"=" + Utf8ToWide(*edit.Value));
    }
    std::stable_sort(entries.begin(), entries.end(), NameLess);

    std::wstring block;
    for (const std::wstring& entry : entries)
    {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
#else
namespace
{
char** InheritedEnvironment()
{
#if defined(__APPLE__)
    // A dylib has no direct access to environ on macOS.
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

bool IsEntryFor(std::string_view entry, std::string_view name)
{
    return entry.size() > name.size() && entry[name.size()] == '=' && entry.starts_with(name);
}
} // namespace

std::optional<std::vector<std::string>> BuildChildEnvironment(std::span<const EnvironmentEdit> edits)
{
    if (!AllNamesValid(edits))
        return std::nullopt;

    std::vector<std::string> entries;
    for (char** entry = InheritedEnvironment(); *entry != nullptr; ++entry)
        entries.emplace_back(*entry);
    for (const EnvironmentEdit& edit : edits)
    {
        std::erase_if(entries, [&edit](const std::string& entry) { return IsEntryFor(entry, edit.Name); });
        if (edit.Value)
            entries.push_back(edit.Name + "=" + *edit.Value);
    }
    return entries;
}
#endif
} // namespace GameEngine::Platform
