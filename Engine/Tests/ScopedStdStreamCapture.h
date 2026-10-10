#pragma once

// Capture of the process's stdout and stderr, for tests that assert on what a code path
// wrote outside the logger — most often that it wrote nothing.
//
// Descriptor level, not stream level: std::cout, std::cerr, printf and fputs all land in
// the same file, so "nothing was written" is a claim about the process rather than about
// one of its stream objects. gtest has the same mechanism behind CaptureStdout(), but
// only in testing::internal; this reaches it through public APIs instead.
//
// Text() may be called once. Reading rewinds nothing, but the capture stops at the
// destructor, which puts the real descriptors back on every exit path.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace GameEngine::TestLog
{

class ScopedStdStreamCapture
{
  public:
    ScopedStdStreamCapture()
    {
        m_Path = std::filesystem::temp_directory_path() /
                 ("ge-stream-capture-" + std::to_string(ProcessId()) + ".txt");

        std::fflush(stdout);
        std::fflush(stderr);
        m_SavedStdout = Duplicate(Descriptor(stdout));
        m_SavedStderr = Duplicate(Descriptor(stderr));

        m_File = std::fopen(m_Path.string().c_str(), "w+");
        if (m_File == nullptr)
        {
            return;
        }
        Redirect(Descriptor(m_File), Descriptor(stdout));
        Redirect(Descriptor(m_File), Descriptor(stderr));
    }

    ~ScopedStdStreamCapture()
    {
        Restore();
        if (m_File != nullptr)
        {
            std::fclose(m_File);
            m_File = nullptr;
        }
        std::error_code ignored;
        std::filesystem::remove(m_Path, ignored);
    }

    ScopedStdStreamCapture(const ScopedStdStreamCapture&) = delete;
    ScopedStdStreamCapture& operator=(const ScopedStdStreamCapture&) = delete;

    /// Everything the process wrote to stdout or stderr since construction.
    std::string Text()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        Restore();

        std::ifstream in(m_Path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

  private:
    static int Descriptor(std::FILE* file)
    {
#if defined(_WIN32)
        return _fileno(file);
#else
        return fileno(file);
#endif
    }

    static int Duplicate(int descriptor)
    {
#if defined(_WIN32)
        return _dup(descriptor);
#else
        return dup(descriptor);
#endif
    }

    static void Redirect(int from, int to)
    {
#if defined(_WIN32)
        _dup2(from, to);
#else
        dup2(from, to);
#endif
    }

    static void Close(int descriptor)
    {
#if defined(_WIN32)
        _close(descriptor);
#else
        close(descriptor);
#endif
    }

    static int ProcessId()
    {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

    void Restore()
    {
        if (m_SavedStdout >= 0)
        {
            std::fflush(stdout);
            Redirect(m_SavedStdout, Descriptor(stdout));
            Close(m_SavedStdout);
            m_SavedStdout = -1;
        }
        if (m_SavedStderr >= 0)
        {
            std::fflush(stderr);
            Redirect(m_SavedStderr, Descriptor(stderr));
            Close(m_SavedStderr);
            m_SavedStderr = -1;
        }
    }

    std::filesystem::path m_Path;
    std::FILE* m_File = nullptr;
    int m_SavedStdout = -1;
    int m_SavedStderr = -1;
};

} // namespace GameEngine::TestLog
