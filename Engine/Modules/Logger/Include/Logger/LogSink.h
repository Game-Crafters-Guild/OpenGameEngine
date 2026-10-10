#pragma once

#include "Logger/Types.h"
#include "Logger/LogLevel.h"
#include <chrono>
#include <cstring>
#include <array>
#include <string_view>

namespace Logger {

/**
 * @brief Structure containing all information about a log message
 */
struct BacktraceFrame;

// Borrowed C-string → owned string. Never calls unbounded strlen: a dangling
// __FILE__ / Function (unloaded module) or a non-terminated pointer becomes
// empty instead of taking the process down. Prefer SetSourceLocation over
// assigning a raw const char* into SourceFile / Function.
String CopySourceCString(const char* src);
inline constexpr size_t kMaxSourceCString = 1024;

// An owned snapshot for a stable logging call site. Keep this object at the
// call site, not in a pointer-keyed global cache: modules may unload or reuse
// addresses. Queued records copy its strings before returning to the caller.
class OwnedSourceLocation
{
public:
    OwnedSourceLocation(const char* file, const char* function)
    {
        const auto ownedFile = CopySourceCString(file);
        const auto ownedFunction = CopySourceCString(function);
        m_FileLength = ownedFile.size();
        m_FunctionLength = ownedFunction.size();
        std::memcpy(m_File.data(), ownedFile.data(), m_FileLength);
        std::memcpy(m_Function.data(), ownedFunction.data(), m_FunctionLength);
    }
    std::string_view File() const { return {m_File.data(), m_FileLength}; }
    std::string_view Function() const { return {m_Function.data(), m_FunctionLength}; }
private:
    // Bounded like CopySourceCString. Trivial destruction keeps static call
    // sites valid during shutdown; no heap-owning static string is torn down.
    std::array<char, kMaxSourceCString> m_File{};
    std::array<char, kMaxSourceCString> m_Function{};
    size_t m_FileLength = 0;
    size_t m_FunctionLength = 0;
};

struct LogMessage {
    LogLevel Level = LogLevel::Info;
    String Message;
    TimePoint Timestamp;
    char ThreadId[16] = {};
    String SourceFile;
    int32 SourceLine = 0;
    String Function;

    // Optional captured C++ callstack, auto-filled for Error/Critical.
    SharedPtr<const Vector<BacktraceFrame>> Backtrace;

    LogMessage() = default;
    LogMessage(LogLevel lvl, String msg)
        : Level(lvl), Message(std::move(msg)), Timestamp(std::chrono::system_clock::now()) {}

    void SetThreadId(const char* id)
    {
        std::strncpy(ThreadId, id, sizeof(ThreadId) - 1);
        ThreadId[sizeof(ThreadId) - 1] = '\0';
    }

    void SetSourceLocation(const char* file, int32 line, const char* function);
};

/**
 * @brief Abstract base class for log output destinations
 * 
 * LogSinks define where log messages are sent (console, file, network, etc.)
 * Multiple sinks can be registered with the Logger for simultaneous output.
 */
class LogSink {
public:
    virtual ~LogSink() = default;
    
    /**
     * @brief Write a log message to this sink
     * @param message Complete log message information
     */
    virtual void Write(const LogMessage& message) = 0;
    
    /**
     * @brief Flush any buffered output
     */
    virtual void Flush() = 0;
    
    /**
     * @brief Check if this sink should handle messages of the given level
     * @param level Log level to check
     * @return True if this sink should process the message
     */
    virtual bool ShouldLog(LogLevel level) const = 0;
    
    /**
     * @brief Get the name of this sink (for debugging/identification)
     * @return Human-readable name of the sink
     */
    virtual String GetName() const = 0;
    
    /**
     * @brief Check if this sink is currently available/working
     * @return True if the sink can accept log messages
     */
    virtual bool IsAvailable() const { return true; }
    
protected:
    /**
     * @brief Format a log message for output
     * @param message Log message to format
     * @param includeTimestamp Whether to include timestamp
     * @param includeLevel Whether to include log level
     * @param includeSource Whether to include source location
     * @return Formatted string ready for output
     */
    String FormatMessage(const LogMessage& message, 
                        bool includeTimestamp = true, 
                        bool includeLevel = true, 
                        bool includeSource = false) const;
    
    /**
     * @brief Format timestamp for output
     * @param timestamp Time point to format
     * @return Formatted timestamp string
     */
    String FormatTimestamp(const TimePoint& timestamp) const;
};

/**
 * @brief Console output sink - writes to stdout/stderr
 */
class ConsoleSink : public LogSink {
public:
    /**
     * @brief Configuration for console output
     */
    struct Config {
        bool UseColors = true;              // Use ANSI color codes
        bool UseStderr = true;              // Use stderr for warnings/errors
        LogLevel MinLevel = LogLevel::Info; // Minimum level to output
        bool IncludeTimestamp = true;       // Include timestamps
        bool IncludeSource = false;         // Include source location (debug only)
    };
    
    explicit ConsoleSink(const Config& config);
    
    void Write(const LogMessage& message) override;
    void Flush() override;
    bool ShouldLog(LogLevel level) const override;
    String GetName() const override { return "Console"; }
    bool IsAvailable() const override;
    
    /**
     * @brief Update configuration
     * @param config New configuration
     */
    void SetConfig(const Config& config) { m_Config = config; }
    
    /**
     * @brief Get current configuration
     * @return Current configuration
     */
    const Config& GetConfig() const { return m_Config; }
    
private:
    Config m_Config;
    bool m_ColorSupported;
};
} // namespace Logger
