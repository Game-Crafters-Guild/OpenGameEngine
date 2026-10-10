#pragma once

#include "Logger/LogSink.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include <atomic>
#include <fstream>
#include <mutex>

namespace Logger
{

/// Environment variable naming the level a file sink records at, for one run.
inline constexpr const char* kFileLogLevelEnvVar = "GE_LOG_FILE_LEVEL";

/// What GE_LOG_FILE_LEVEL resolved to for a file sink.
struct FileLogLevel
{
	LogLevel level = LogLevel::Info;
	// The GE_LOG_FILE_LEVEL value that named no level, empty otherwise. A host
	// names it when it reports the level, so a typo does not read as "the
	// variable did nothing".
	String unrecognizedValue;
};

/**
 * @brief Level a file sink records at: Info, or the level named by
 *        GE_LOG_FILE_LEVEL (debug, info, warn, error, any capitalization),
 *        never below the global level in force when this is called.
 *
 * The file is the artifact read after the run, so it stays at Info while the
 * global level — and with it the console and the in-memory ring sink debug tools
 * read — can sit at Debug. The global level bounds it from the other side: the
 * call site gates on that level before any sink is consulted, so a file level
 * under it records nothing extra. A value that names no level resolves to Info
 * and comes back in unrecognizedValue.
 *
 * The editor is the host that resolves its file sink this way, and it reports
 * the level it got in the line announcing the log file, so a typo is visible
 * there. Every other file sink in the tree is built at a level its host names
 * directly.
 */
FileLogLevel ResolveFileLogLevel();

/**
 * @brief File output sink - writes log messages to a file with optional rotation.
 */
class FileSink : public LogSink
{
public:
	struct Config
	{
		String filename;                       // Output filename
		bool append = true;                    // Append to existing file if true
		LogLevel minLevel = LogLevel::Debug;   // Minimum level to output
		bool includeTimestamp = true;          // Include timestamps in output
		bool includeSource = true;             // Include source location if available
		size_t maxFileSize = 10 * 1024 * 1024; // Max file size (10 MB) before rotation
		size_t maxBackupFiles = 5;             // Number of rotated backup files to keep
		bool autoFlush = false;                // Flush after each write
	};

	explicit FileSink(const Config& config);
	~FileSink() override;

	void Write(const LogMessage& message) override;
	void Flush() override;
	bool ShouldLog(LogLevel level) const override;
	String GetName() const override;
	bool IsAvailable() const override;

private:
	Config m_Config;
	String m_CurrentFilename;
	std::ofstream m_File;
	size_t m_CurrentSize;
	mutable std::mutex m_Mutex;
	// Config::append decides only whether the FIRST open inherits a previous run's
	// file. Every later open — a lazy reopen after a write failure, or the fresh
	// file after a rotation — appends, or it would erase this session's own log.
	bool m_HasOpened = false;
	// Set when a rotation could not rename the current file (another process holds
	// it open). Retrying on every write is the failure this guards; see RotateFile.
	bool m_RotationDisabled = false;
	// Set when the file could not be opened. Write() reopens lazily while the file
	// is closed, so without this an unopenable path costs a directory creation, an
	// open and a stderr line per log line; see OpenFile. Atomic because
	// IsAvailable reads it without m_Mutex, from whichever thread dispatches.
	std::atomic<bool> m_OpenFailed{false};

	void OpenFile();
	void CloseFile();
	void RotateIfNeeded();
	void RotateFile();
};

} // namespace Logger

#endif // LOGGER_ENABLE_FILE_LOGGING
