#include "Logger/LogSink.h"
#include <cstdio>
#include <cstring>
#include <iostream>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef FormatMessage // Avoid conflict with Windows API
#else
#include <unistd.h>
#endif

namespace Logger
{

String LogSink::FormatMessage(const LogMessage& message,
                              bool includeTimestamp,
                              bool includeLevel,
                              bool includeSource) const
{
    // Reuse a thread_local buffer to avoid per-call allocation.
    // The drain thread is the primary caller, so this buffer stabilizes quickly.
    thread_local String result;
    result.clear();

#if LOGGER_ENABLE_TIMESTAMPS
    if (includeTimestamp)
    {
        result += '[';
        result += FormatTimestamp(message.Timestamp);
        result += "] ";
    }
#endif

    if (includeLevel)
    {
        result += '[';
        result += LogLevelToString(message.Level);
        result += "] ";
    }

    if (message.ThreadId[0] != '\0')
    {
        result += "[T:";
        result.append(message.ThreadId, std::min(std::strlen(message.ThreadId), size_t(6)));
        result += "] ";
    }

#if LOGGER_ENABLE_SOURCE_LOCATION
    if (includeSource && !message.SourceFile.empty())
    {
        const char* path = message.SourceFile.c_str();
        const char* lastSlash = std::strrchr(path, '/');
        const char* lastBackslash = std::strrchr(path, '\\');
        const char* filename = path;
        if (lastSlash && lastSlash > filename)
            filename = lastSlash + 1;
        if (lastBackslash && lastBackslash > filename)
            filename = lastBackslash + 1;

        result += '[';
        result += filename;
        result += ':';
        char lineBuf[12];
        std::snprintf(lineBuf, sizeof(lineBuf), "%d", message.SourceLine);
        result += lineBuf;
        if (!message.Function.empty())
        {
            result += " in ";
            result += message.Function;
        }
        result += "] ";
    }
#endif

    result += message.Message;
    return result;
}

String LogSink::FormatTimestamp(const TimePoint& timestamp) const
{
    // Cache the localtime conversion — only recompute when the second changes.
    // This eliminates a syscall per message during sustained logging.
    thread_local time_t cachedTimeT = 0;
    thread_local std::tm cachedTm = {};

    auto timeT = std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(timestamp));

    if (timeT != cachedTimeT)
    {
        cachedTimeT = timeT;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&cachedTm, &timeT);
#else
        localtime_r(&timeT, &cachedTm);
#endif
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  timestamp.time_since_epoch()) %
              1000;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  cachedTm.tm_hour, cachedTm.tm_min, cachedTm.tm_sec, static_cast<int>(ms.count()));
    return String(buf);
}

// ========================================
// ConsoleSink Implementation
// ========================================

ConsoleSink::ConsoleSink(const Config& config)
    : m_Config(config)
{
#ifdef WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
    {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
        m_ColorSupported = true;
    }
    else
    {
        m_ColorSupported = false;
    }
#else
    m_ColorSupported = isatty(fileno(stdout));
#endif

    m_ColorSupported = m_ColorSupported && m_Config.UseColors;
}

void ConsoleSink::Write(const LogMessage& message)
{
    if (!ShouldLog(message.Level))
        return;

    String formattedMessage = FormatMessage(message,
                                            m_Config.IncludeTimestamp,
                                            true,
                                            m_Config.IncludeSource);

    std::ostream* output = &std::cout;
    if (m_Config.UseStderr && (message.Level >= LogLevel::Warning))
    {
        output = &std::cerr;
    }

    if (m_ColorSupported)
    {
        *output << LogLevelToColorCode(message.Level);
    }

    *output << formattedMessage;

    if (m_ColorSupported)
    {
        *output << GetColorReset();
    }

    *output << '\n';

    if (message.Level >= LogLevel::Error)
    {
        output->flush();
    }
}

void ConsoleSink::Flush()
{
    std::cout.flush();
    std::cerr.flush();
}

bool ConsoleSink::ShouldLog(LogLevel level) const
{
    return level >= m_Config.MinLevel;
}

bool ConsoleSink::IsAvailable() const
{
    return true;
}

} // namespace Logger
