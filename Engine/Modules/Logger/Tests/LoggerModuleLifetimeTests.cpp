// A separately linked producer DLL logging into a persistent logger DLL — the
// arrangement a generated native game module has with Engine.dll.
//
// The producer emits either borrowed source pointers or a call-site snapshot
// held in its own module storage; both must reach the sink after the image is
// gone. Each case parks the real drain thread on the sink mutex so the producer's
// records are still in the queue when FreeLibrary runs, proves the image is
// unmapped (GetModuleHandleExW by address), and only then exercises one queue
// operation against the retired image: dispatch, vector relocation, destruction,
// logging from DLL_PROCESS_DETACH, or concurrent producers. DrainFirst and Plain
// are controls — they pass whether or not the queue owns its records.
//
// Every case runs in its own process (EXPECT_EXIT), because the failure under
// test is an access violation on the drain thread: the vectored handler prints
// the faulting instruction pointer against the retired image range and
// terminates with a distinct exit code rather than taking the suite down.
//
// The probe (LoggerModuleProbe.cpp) counts live module-defined arguments and
// formatter invocations and overwrites its own source-metadata buffers before
// returning, so "the producer still owned something" is observable rather than
// inferred.

#include <gtest/gtest.h>
#include "Logger/Logger.h"
#include "LoggerModuleProbe.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace
{
using namespace Logger;
enum class QueueUse
{
    Dispatch,
    OwnedSource,
    Move,
    Destroy,
    Detach,
    DrainFirst,
    Plain,
    Concurrent
};
std::uintptr_t s_RetiredBase = 0, s_RetiredSize = 0;
bool s_Unmapped = false;

LONG WINAPI OnFault(EXCEPTION_POINTERS* exception)
{
    if (s_Unmapped && exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        const auto address =
            reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
        std::fprintf(stderr, "AV after unload: ip=0x%llx retired=[0x%llx,0x%llx)\n",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(s_RetiredBase),
                     static_cast<unsigned long long>(s_RetiredBase + s_RetiredSize));
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 88);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 17);
    }
}

class CaptureSink : public LogSink
{
  public:
    std::vector<LogMessage> Messages;
    void Write(const LogMessage& message) override { Messages.push_back(message); }
    void Flush() override {}
    bool ShouldLog(LogLevel) const override { return true; }
    String GetName() const override { return "ModuleCapture"; }
};

void ExerciseUnloadedQueue(QueueUse use)
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ::AddVectoredExceptionHandler(1, OnFault);
    Log::Initialize({});
    Log::ClearSinks();
    auto sink = std::make_unique<CaptureSink>();
    auto* spy = sink.get();
    Log::AddSink(std::move(sink));
    Log::Flush();

    wchar_t executable[32768]{};
    Check(::GetModuleFileNameW(nullptr, executable, 32768) != 0, "missing executable path");
    const auto path = std::filesystem::path(executable).parent_path() / "LoggerModuleProbe.dll";
    HMODULE module = ::LoadLibraryW(path.c_str());
    Check(module != nullptr, "fixture DLL did not load");
    s_RetiredBase = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(s_RetiredBase + dos->e_lfanew);
    s_RetiredSize = nt->OptionalHeader.SizeOfImage;
    auto configure =
        reinterpret_cast<ConfigureLoggerModuleProbe>(::GetProcAddress(module, "ConfigureProbe"));
    auto emit = reinterpret_cast<EmitLoggerModuleProbe>(::GetProcAddress(
        module, use == QueueUse::OwnedSource ? "EmitOwnedProbe" : "EmitProbe"));
    Check(configure && emit, "missing fixture exports");
    // Deliberately rejected ABI fixture: logging happened before acceptance.
    Check(::GetProcAddress(module, "GE_UserModule_AbiVersion_v1") == nullptr,
          "fixture unexpectedly supplies the module acceptance export");
    LoggerModuleProbeStats stats;
    configure(&stats, use == QueueUse::Detach);

    auto& state = *Log::GetStateForSharing();
    std::unique_lock sinkLock(state.Mutex);
    Log::Info(std::string_view("host seed"));
    // Park the real drain on the sink mutex with its seed batch. All following
    // DLL records stay in Queue, so Move/Destroy cannot accidentally exercise
    // only an already-drained record. The timeout reports a failed precondition.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        bool empty;
        {
            std::lock_guard queueLock(state.QueueMutex);
            empty = state.Queue.empty();
        }
        if (empty)
            break;
        Check(std::chrono::steady_clock::now() < deadline, "drain did not take its seed batch");
        std::this_thread::yield();
    }

    unsigned count = 1;
    if (use == QueueUse::Concurrent)
    {
        count = 80;
        std::vector<std::thread> producers;
        for (unsigned thread = 0; thread < 4; ++thread)
            producers.emplace_back(
                [=]
                {
                    for (unsigned i = 0; i < 20; ++i)
                        emit(thread * 20 + i, true);
                });
        for (auto& producer : producers)
            producer.join();
    }
    else if (use == QueueUse::OwnedSource)
    {
        // Twice: the first call builds the call-site snapshot and then destroys
        // the buffers it was built from, so the second record can only be right
        // if the snapshot copied them.
        count = 2;
        emit(29, true);
        emit(30, true);
    }
    else
        emit(29, use != QueueUse::Plain);

    if (use == QueueUse::DrainFirst)
    {
        sinkLock.unlock();
        Log::Flush();
        sinkLock.lock();
    }
    const bool ownedBeforeUnload =
        stats.LiveArguments == 0 && stats.Formatted == (use == QueueUse::Plain ? 0u : count);
    if (use != QueueUse::DrainFirst)
    {
        std::lock_guard queueLock(state.QueueMutex);
        Check(state.Queue.size() == count, "fixture records were not queued before unmap");
    }
    Check(::FreeLibrary(module) != 0, "fixture FreeLibrary failed");
    HMODULE owner = nullptr;
    Check(!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(s_RetiredBase), &owner),
          "fixture remained mapped");
    s_Unmapped = true;

    if (use == QueueUse::Move)
    {
        size_t capacity;
        {
            std::lock_guard queueLock(state.QueueMutex);
            capacity = state.Queue.capacity();
        }
        for (size_t i = 0; i <= capacity; ++i)
            Log::Info(std::string_view("host growth"));
    }
    else if (use == QueueUse::Destroy)
    {
        std::lock_guard queueLock(state.QueueMutex);
        state.Queue.clear();
    }
    sinkLock.unlock();
    Log::Flush();
    Check(ownedBeforeUnload, "producer returned with pending formatter code or argument ownership");
    Check(stats.LiveArguments == 0, "queue retained module-defined argument destructors");

    const char* expectedFunction = use == QueueUse::OwnedSource ? "EmitOwnedProbe" : "EmitProbe";
    unsigned delivered = 0, detached = 0;
    for (const auto& message : spy->Messages)
    {
        if (message.Message.starts_with("probe "))
        {
            ++delivered;
            Check(message.SourceFile == "fixture.cpp" && message.SourceLine == 91 &&
                      message.Function == expectedFunction,
                  "source metadata did not survive the producer");
        }
        if (message.Message == "detach log 71")
            ++detached;
    }
    Check(delivered == (use == QueueUse::Destroy ? 0u : count), "wrong delivered record count");
    Check(detached == (use == QueueUse::Detach ? 1u : 0u), "detach logging was lost");
    Log::Shutdown();
    ::TerminateProcess(::GetCurrentProcess(), 0);
}

TEST(LoggerModuleLifetime, RejectedDllRecordsDispatchAfterUnmap)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Dispatch), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, OwnedSourceRecordsDispatchAfterUnmap)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::OwnedSource), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, QueuedRecordsMoveAfterUnmap)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Move), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, QueuedRecordsDestroyAfterUnmap)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Destroy), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, DllDetachCanPublishARecord)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Detach), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, DrainWhileMappedControl)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::DrainFirst), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, PlainOwnedTextControl)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Plain), ::testing::ExitedWithCode(0), "");
}
TEST(LoggerModuleLifetime, ConcurrentProducersFinishBeforeUnmap)
{
    EXPECT_EXIT(ExerciseUnloadedQueue(QueueUse::Concurrent), ::testing::ExitedWithCode(0), "");
}
} // namespace
