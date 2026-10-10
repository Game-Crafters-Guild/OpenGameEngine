#include "Logger/CallbackSink.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <type_traits>

static_assert(std::is_trivially_destructible_v<Logger::OwnedSourceLocation>);

#if defined(__APPLE__) || defined(__linux__)
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <cerrno>
#include <cstddef>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
TEST(SourceCStringTests, PreservesCrashHandlerAndAlternateStackState)
{
    struct sigaction beforeSegv{}, beforeBus{}, afterSegv{}, afterBus{};
    stack_t beforeStack{}, afterStack{};
    ASSERT_EQ(sigaction(SIGSEGV, nullptr, &beforeSegv), 0);
    ASSERT_EQ(sigaction(SIGBUS, nullptr, &beforeBus), 0);
    ASSERT_EQ(sigaltstack(nullptr, &beforeStack), 0);
    EXPECT_TRUE(Logger::CopySourceCString(reinterpret_cast<const char*>(uintptr_t{1})).empty());
    EXPECT_EQ(Logger::CopySourceCString("valid.cpp"), "valid.cpp");
    ASSERT_EQ(sigaction(SIGSEGV, nullptr, &afterSegv), 0);
    ASSERT_EQ(sigaction(SIGBUS, nullptr, &afterBus), 0);
    ASSERT_EQ(sigaltstack(nullptr, &afterStack), 0);
    EXPECT_EQ(beforeSegv.sa_sigaction, afterSegv.sa_sigaction);
    EXPECT_EQ(beforeBus.sa_sigaction, afterBus.sa_sigaction);
    EXPECT_EQ(beforeSegv.sa_flags, afterSegv.sa_flags);
    EXPECT_EQ(beforeBus.sa_flags, afterBus.sa_flags);
    for (int signal = 1; signal < NSIG; ++signal)
    {
        EXPECT_EQ(sigismember(&beforeSegv.sa_mask, signal), sigismember(&afterSegv.sa_mask, signal));
        EXPECT_EQ(sigismember(&beforeBus.sa_mask, signal), sigismember(&afterBus.sa_mask, signal));
    }
    EXPECT_EQ(beforeStack.ss_sp, afterStack.ss_sp);
    EXPECT_EQ(beforeStack.ss_size, afterStack.ss_size);
    EXPECT_EQ(beforeStack.ss_flags, afterStack.ss_flags);
}

TEST(SourceCStringTests, ProtectedBoundaryAndUnmappedPageDoNotCrash)
{
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* mapping = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    ASSERT_NE(mapping, MAP_FAILED);
    auto* bytes = static_cast<char*>(mapping);
    ASSERT_EQ(mprotect(bytes + pageSize, pageSize, PROT_NONE), 0);
    std::memset(bytes, 'X', pageSize);
    bytes[pageSize - 1] = '\0';
    EXPECT_EQ(Logger::CopySourceCString(bytes + pageSize - 16), std::string(15, 'X'));
    bytes[pageSize - 1] = 'X';
    EXPECT_TRUE(Logger::CopySourceCString(bytes + pageSize - 16).empty());
    EXPECT_TRUE(Logger::CopySourceCString(bytes).empty());
    EXPECT_TRUE(Logger::CopySourceCString(bytes + pageSize).empty());
    ASSERT_EQ(munmap(mapping, pageSize * 2), 0);
    EXPECT_TRUE(Logger::CopySourceCString(bytes).empty());
}

TEST(SourceCStringTests, ConcurrentValidAndInvalidSourcesRemainIndependent)
{
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 8; ++thread)
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 1000; ++iteration)
            {
                if (Logger::CopySourceCString("thread.cpp") != "thread.cpp"
                    || !Logger::CopySourceCString(reinterpret_cast<const char*>(uintptr_t{1})).empty())
                    ++failures;
            }
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST(SourceCStringTests, CopiesAcrossReadablePageBoundaryAndMultipleChunks)
{
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* mapping = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    ASSERT_NE(mapping, MAP_FAILED);
    auto* bytes = static_cast<char*>(mapping);
    // Every start offset in the last 300 bytes of the first page, against
    // lengths on both sides of the 128-byte read chunk and of the page
    // boundary. The copied length is measured from the start of the
    // destination rather than from the current chunk, so an off-by-a-chunk is
    // only observable once a string needs more than one read.
    std::string wrong;
    size_t cases = 0;
    for (size_t back = 1; back <= 300; ++back)
    {
        auto* start = bytes + pageSize - back;
        for (const size_t length : {size_t{0}, size_t{1}, size_t{127}, size_t{128}, size_t{129},
                                    size_t{255}, size_t{256}, size_t{257}, size_t{511}, size_t{1022}})
        {
            std::memset(start, 'X', length);
            start[length] = 0;
            ++cases;
            if (Logger::CopySourceCString(start) != std::string(length, 'X') && wrong.size() < 200)
                wrong += " -" + std::to_string(back) + "/+" + std::to_string(length);
        }
    }
    EXPECT_EQ(cases, 3000u);
    EXPECT_TRUE(wrong.empty()) << "offset before the page end / length that did not round-trip:" << wrong;
    ASSERT_EQ(munmap(mapping, pageSize * 2), 0);
}

TEST(SourceCStringTests, OwnedSourceSurvivesUnmappingWithoutCachingBorrowedAddress)
{
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* mapping = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    ASSERT_NE(mapping, MAP_FAILED);
    auto* text = static_cast<char*>(mapping);
    std::memcpy(text, "module.cpp", 11);
    const Logger::OwnedSourceLocation source(text, "function");
    ASSERT_EQ(munmap(mapping, pageSize), 0);
    EXPECT_EQ(source.File(), "module.cpp");
    EXPECT_TRUE(Logger::CopySourceCString(text).empty());
}
#endif

using namespace Logger;

#if defined(__linux__)
TEST(SourceCStringTests, DeniedProbeReportsUnavailableInsteadOfUnreadablePointer)
{
    if (prctl(PR_GET_SECCOMP) < 0)
        GTEST_SKIP() << "the kernel does not support seccomp filtering";
    EXPECT_EXIT(([] {
        Log::Initialize({});
        Log::ClearSinks();
        static std::mutex blameMutex;
        static size_t blamedPointers = 0;
        auto sink = MakeUnique<CallbackSink>();
        sink->RegisterCallback([](const LogMessage& message) {
            std::lock_guard lock(blameMutex);
            if (message.Message.find("dropped unreadable source-location pointer") != String::npos)
                ++blamedPointers;
        });
        Log::AddSink(std::move(sink));
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_process_vm_readv, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
            || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
            _exit(77);
        const bool first = CopySourceCString("valid.cpp").empty();
        const bool second = CopySourceCString("still-valid.cpp").empty();
        // Both pointers are readable: a denied probe must be reported as the
        // probe being unavailable and must blame neither of them.
        Log::Flush();
        std::lock_guard lock(blameMutex);
        _exit(first && second && blamedPointers == 0 ? 0 : 1);
    }()), ::testing::ExitedWithCode(0), "source-location probing unavailable");
}
#endif

TEST(SourceCStringTests, NullAndEmptyBecomeEmpty)
{
    EXPECT_TRUE(CopySourceCString(nullptr).empty());
    EXPECT_TRUE(CopySourceCString("").empty());
}

TEST(SourceCStringTests, CopiesValidLiteral)
{
    EXPECT_EQ(CopySourceCString("Foo.cpp"), "Foo.cpp");
    EXPECT_EQ(CopySourceCString("Bar"), "Bar");
}

TEST(SourceCStringTests, SetSourceLocationCopiesOwnedStrings)
{
    LogMessage msg;
    msg.SetSourceLocation("Foo.cpp", 42, "Bar");
    EXPECT_EQ(msg.SourceFile, "Foo.cpp");
    EXPECT_EQ(msg.SourceLine, 42);
    EXPECT_EQ(msg.Function, "Bar");
}

TEST(SourceCStringTests, UnmappedPointerDoesNotCrash)
{
    const char* unmapped = reinterpret_cast<const char*>(static_cast<uintptr_t>(0x1));
    EXPECT_TRUE(CopySourceCString(unmapped).empty());

    LogMessage msg;
    msg.SetSourceLocation(unmapped, 7, unmapped);
    EXPECT_TRUE(msg.SourceFile.empty());
    EXPECT_TRUE(msg.Function.empty());
    EXPECT_EQ(msg.SourceLine, 7);
}

#if defined(_WIN32) || defined(_WIN64)
TEST(SourceCStringTests, FreedPageDoesNotCrash)
{
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(page, nullptr);
    std::memcpy(page, "was-valid.cpp", 14);
    const char* ptr = static_cast<const char*>(page);
    ASSERT_TRUE(VirtualFree(page, 0, MEM_RELEASE));

    EXPECT_TRUE(CopySourceCString(ptr).empty());
}

TEST(SourceCStringTests, UnterminatedReadableBecomesEmpty)
{
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(page, nullptr);
    std::memset(page, 'X', 4096);

    EXPECT_TRUE(CopySourceCString(static_cast<const char*>(page)).empty());
    VirtualFree(page, 0, MEM_RELEASE);
}
#endif
