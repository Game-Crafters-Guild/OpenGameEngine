#pragma once

#include <filesystem>

#if LOGGER_ENABLE_FILE_LOGGING

namespace Logger
{
class DeferredSink;
}

namespace GameEngine
{

/// Starts holding every log record. The Player's log file lives in the per-user
/// logs directory of the game the configuration names, so it cannot open until
/// the configuration is read, and reading it logs its own problems. Returns the
/// holding sink, which the Logger owns for the rest of the process.
Logger::DeferredSink& StartPlayerLog();

/// Opens this process's log file in logsDirectory, writes the held records into
/// it and sends every later record there. The file name carries the process id,
/// so two running copies of a game each keep their own log. A file that cannot be
/// opened is logged as an error, and the held records go to no file.
void OpenPlayerLogFile(Logger::DeferredSink& heldRecords, const std::filesystem::path& logsDirectory);

} // namespace GameEngine

#endif // LOGGER_ENABLE_FILE_LOGGING
