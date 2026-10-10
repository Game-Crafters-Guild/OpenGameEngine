// Child fixture for the argv CancellableShellProcess::Run tests: each mode writes a
// known shape to its streams, so what the runner delivered can be compared with
// what the child wrote.
//
//   streams              "to-stderr" on stderr, then "to-stdout" on stdout
//   line <n>             "before", a line of n 'x', "after" on stdout
//   stderr-flood <n>     n 'e' then "END" on stderr, then "done" on stdout
//   stdin                reads stdin to its end; prints "bytes=<count>" and "first=<first line>"
//   write-then-read <n>  a line of n 'o' on stdout, then reads stdin to its end and prints "bytes=<count>"
//   carriage-returns     "a<CR>b<CR><LF>c<LF>" on stdout, bytes exactly as written
//   report <names...>    prints "cwd=<working directory>", then [NAME=value] or [NAME unset] per name
//   sleep                prints "sleeping", then sleeps 30 s
//   holds <handle> <event>   Windows: "holds=1" when <handle> (a decimal handle value)
//                        names the event <event>, else "holds=0"
//   stdin-holder <ms>    Windows: starts a copy of itself in "linger <ms>" that inherits
//                        this process's stdin, then exits at once without reading stdin
//   linger <ms>          sleeps <ms> milliseconds

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {
int Streams()
{
    std::fputs("to-stderr\n", stderr);
    std::fflush(stderr);
    std::fputs("to-stdout\n", stdout);
    return 0;
}

int Line(std::size_t length)
{
    std::fputs("before\n", stdout);
    const std::string line(length, 'x');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputs("\nafter\n", stdout);
    return 0;
}

int StderrFlood(std::size_t length)
{
    const std::string flood(length, 'e');
    std::fwrite(flood.data(), 1, flood.size(), stderr);
    std::fputs("END", stderr);
    std::fflush(stderr);
    std::fputs("done\n", stdout);
    return 0;
}

int Stdin()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    std::printf("bytes=%zu\n", input.size());
    std::printf("first=%s\n", input.substr(0, input.find('\n')).c_str());
    return 0;
}

// Writes before it reads: with more than a pipe buffer on each side, a parent that
// wrote stdin on its reading thread would wait on this child forever.
int WriteThenRead(std::size_t length)
{
    const std::string line(length, 'o');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    std::printf("bytes=%zu\n", input.size());
    return 0;
}

int CarriageReturns()
{
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fputs("a\rb\r\nc\n", stdout);
    return 0;
}

int Report(int argc, char** argv)
{
    std::printf("cwd=%s\n", std::filesystem::current_path().generic_string().c_str());
    for (int i = 2; i < argc; ++i)
    {
        const char* value = std::getenv(argv[i]);
        if (value != nullptr)
            std::printf("[%s=%s]\n", argv[i], value);
        else
            std::printf("[%s unset]\n", argv[i]);
    }
    return 0;
}

int Sleep()
{
    std::fputs("sleeping\n", stdout);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}

int Linger(unsigned long long milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    return 0;
}

#if defined(_WIN32)
// An inherited handle keeps its value in the child, so the parent passes the value
// and the event's name; CompareObjectHandles tells whether both name one object.
int Holds(const char* handleValue, const char* eventName)
{
    using CompareObjectHandlesFn = BOOL(WINAPI*)(HANDLE, HANDLE);
    const auto compare = reinterpret_cast<CompareObjectHandlesFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
    HANDLE byName = ::OpenEventA(SYNCHRONIZE, FALSE, eventName);
    if (compare == nullptr || byName == nullptr)
        return 3;
    const auto value = static_cast<std::uintptr_t>(std::strtoull(handleValue, nullptr, 10));
    std::printf("holds=%d\n", compare(reinterpret_cast<HANDLE>(value), byName) ? 1 : 0);
    ::CloseHandle(byName);
    return 0;
}

// The descendant inherits this process's stdin and nothing else, and outlives it.
int StdinHolder(const char* milliseconds)
{
    wchar_t self[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0)
        return 3;
    std::wstring commandLine = L"\"" + std::wstring(self) + L"\" linger ";
    for (const char* digit = milliseconds; *digit != '\0'; ++digit)
        commandLine.push_back(static_cast<wchar_t>(*digit));

    HANDLE stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::string attrStorage(attrSize, '\0');
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize) ||
        !::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &stdinHandle,
                                     sizeof(stdinHandle), nullptr, nullptr))
        return 3;
    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdInput = stdinHandle;
    siex.lpAttributeList = attrList;
    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                          &siex.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList(attrList);
    if (!created)
        return 3;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return 0;
}
#endif
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    const std::string mode = argv[1];
    if (mode == "streams")
        return Streams();
    if (mode == "line" && argc == 3)
        return Line(std::strtoull(argv[2], nullptr, 10));
    if (mode == "stderr-flood" && argc == 3)
        return StderrFlood(std::strtoull(argv[2], nullptr, 10));
    if (mode == "stdin")
        return Stdin();
    if (mode == "write-then-read" && argc == 3)
        return WriteThenRead(std::strtoull(argv[2], nullptr, 10));
    if (mode == "carriage-returns")
        return CarriageReturns();
    if (mode == "report")
        return Report(argc, argv);
    if (mode == "sleep")
        return Sleep();
    if (mode == "linger" && argc == 3)
        return Linger(std::strtoull(argv[2], nullptr, 10));
#if defined(_WIN32)
    if (mode == "holds" && argc == 4)
        return Holds(argv[2], argv[3]);
    if (mode == "stdin-holder" && argc == 3)
        return StdinHolder(argv[2]);
#endif
    return 2;
}
