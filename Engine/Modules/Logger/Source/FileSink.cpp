#include "Logger/FileSink.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include "Logger/LogLevel.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace Logger
{

FileLogLevel ResolveFileLogLevel()
{
	FileLogLevel resolved;

	const char* configured = std::getenv(kFileLogLevelEnvVar);
	if (configured != nullptr && configured[0] != '\0')
	{
		String spelling(configured);
		std::transform(spelling.begin(), spelling.end(), spelling.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		// StringToLogLevel answers Info for a spelling it does not know, so "info"
		// is the one value that has to be excluded to tell a request from a typo.
		const LogLevel named = StringToLogLevel(spelling);
		if (named != LogLevel::Info || spelling == "info")
		{
			resolved.level = named;
		}
		else
		{
			resolved.unrecognizedValue = configured;
		}
	}

	// The global level gates at the call site, before any sink is consulted, so a
	// file level below it admits nothing extra — it would only make the level the
	// host reports untrue. Off is what that gate reads while the logger holds no
	// sink rather than a level a host chose, and attaching this sink restores the
	// global level, so it bounds nothing.
	const LogLevel globalLevel = Log::GetLogLevel();
	if (globalLevel != LogLevel::Off)
	{
		resolved.level = std::max(resolved.level, globalLevel);
	}
	return resolved;
}

FileSink::FileSink(const Config& config)
	: m_Config(config)
	, m_CurrentFilename(config.filename)
	, m_CurrentSize(0)
{
	OpenFile();
}

FileSink::~FileSink()
{
	CloseFile();
}

void FileSink::Write(const LogMessage& message)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (!m_File.is_open())
	{
		OpenFile();
		if (!m_File.is_open())
		{
			return;
		}
	}

	RotateIfNeeded();

	// The level is always written: a log file whose lines carry no severity cannot
	// be triaged, and FileSink::Config deliberately has no switch for it.
	String formatted =
	    FormatMessage(message, m_Config.includeTimestamp, /*includeLevel=*/true, m_Config.includeSource);
	m_File << formatted << '\n';
	m_CurrentSize += formatted.size() + 1; // account for newline

	if (m_Config.autoFlush)
	{
		m_File.flush();
	}
}

void FileSink::Flush()
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (m_File.is_open())
	{
		m_File.flush();
	}
}

bool FileSink::ShouldLog(LogLevel level) const
{
	return level >= m_Config.minLevel;
}

String FileSink::GetName() const
{
	return "FileSink";
}

bool FileSink::IsAvailable() const
{
	// A failed open is final for the session (see OpenFile), so the sink says so.
	return !m_CurrentFilename.empty() && !m_OpenFailed.load(std::memory_order_relaxed);
}

void FileSink::OpenFile()
{
	if (m_CurrentFilename.empty() || m_OpenFailed.load(std::memory_order_relaxed))
	{
		return;
	}

		// Ensure parent directory exists (if any). This keeps the sink robust when
		// callers pass paths like "Logs/Editor.txt" without pre-creating the folder.
		std::error_code ec;
		std::filesystem::path path(m_CurrentFilename);
		std::filesystem::path parent = path.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				// Use stderr directly — going through Logger here would
				// recurse back into us via any FileSinks already attached.
				std::fprintf(stderr, "FileSink: failed to create directory '%s': %s\n",
				             parent.string().c_str(), ec.message().c_str());
			}
		}

		using std::ios;
		ios::openmode mode = ios::out;
		// Truncate only on the very first open, and only if configured to: a reopen
		// mid-session must never discard what this session has already written.
		mode |= (m_Config.append || m_HasOpened) ? ios::app : ios::trunc;

		m_File.open(m_CurrentFilename.c_str(), mode);
	if (!m_File.is_open())
	{
		// Write() reopens on every line while the file is closed, so retrying is
		// a storm: a read-only directory or a full disk is not transient, and
		// each retry costs a create_directories, an open and this message. One
		// answer for the session; the sink stays inert until it is replaced.
		m_OpenFailed.store(true, std::memory_order_relaxed);
		// stderr, not Logger: this can run under m_Mutex from inside Write().
		std::fprintf(stderr,
		             "FileSink: cannot open '%s' for writing - no further attempts will be made. "
		             "Point the log at a writable path to restore it.\n",
		             m_CurrentFilename.c_str());
		m_CurrentSize = 0;
		return;
	}
	m_HasOpened = true;

	// Compute current file size for rotation bookkeeping.
		ec.clear();
	auto fileSize = std::filesystem::file_size(path, ec);
	if (!ec)
	{
		m_CurrentSize = static_cast<size_t>(fileSize);
	}
	else
	{
		m_CurrentSize = 0;
	}
}

void FileSink::CloseFile()
{
	if (m_File.is_open())
	{
		m_File.flush();
		m_File.close();
	}
}

void FileSink::RotateIfNeeded()
{
	if (m_Config.maxFileSize == 0 || m_Config.maxBackupFiles == 0 || m_RotationDisabled)
	{
		return;
	}

	if (m_CurrentSize < m_Config.maxFileSize)
	{
		return;
	}

	RotateFile();
}

void FileSink::RotateFile()
{
	if (m_CurrentFilename.empty())
	{
		return;
	}

	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path basePath(m_CurrentFilename);
	fs::path directory = basePath.parent_path();
	fs::path stem = basePath.stem();
	fs::path extension = basePath.extension();

	// Delete the oldest backup if it exists
	if (m_Config.maxBackupFiles > 0)
	{
		fs::path oldest = directory / (stem.string() + "." + std::to_string(m_Config.maxBackupFiles) + extension.string());
		fs::remove(oldest, ec);
	}

	// Shift existing backups up (file.(n-1) -> file.n)
	for (size_t i = m_Config.maxBackupFiles; i-- > 1;)
	{
		fs::path src = directory / (stem.string() + "." + std::to_string(i) + extension.string());
		fs::path dst = directory / (stem.string() + "." + std::to_string(i + 1) + extension.string());
		if (fs::exists(src, ec))
		{
			fs::rename(src, dst, ec);
		}
	}

	// Current log becomes first backup
	fs::path firstBackup = directory / (stem.string() + ".1" + extension.string());
	CloseFile();
	if (fs::exists(basePath, ec))
	{
		std::error_code renameEc;
		fs::rename(basePath, firstBackup, renameEc);
		if (renameEc)
		{
			// The rename is the rotation; if it failed, nothing rotated. Windows
			// denies it while any other process holds the file open, and that is
			// not transient. Reporting and retrying next write would be a loop:
			// OpenFile re-stats the size straight back to the cap, so every
			// subsequent Write would close, fail to rename, and reopen, forever.
			// Keep appending instead — an oversized log is recoverable, a sink
			// that thrashes instead of writing is not.
			m_RotationDisabled = true;
			// stderr, not Logger: this runs under m_Mutex from inside Write().
			std::fprintf(stderr,
			             "FileSink: cannot rotate '%s' (%s) - continuing without rotation. "
			             "Give each process its own log path to restore it.\n",
			             m_CurrentFilename.c_str(), renameEc.message().c_str());
			// Reopens in append mode (m_HasOpened), preserving the file we could
			// not rotate, and restores m_CurrentSize from it.
			OpenFile();
			return;
		}
	}

	// Re-open a fresh log file
	m_CurrentSize = 0;
	OpenFile();
}

} // namespace Logger

#endif // LOGGER_ENABLE_FILE_LOGGING
