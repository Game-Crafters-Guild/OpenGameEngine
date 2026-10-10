#include "PlayerLog.h"

#if LOGGER_ENABLE_FILE_LOGGING

#include "Logger/DeferredSink.h"
#include "Logger/FileSink.h"
#include "Logger/Logger.h"
#include "Platform/Process.h"

#include <string>
#include <utility>

namespace GameEngine
{

Logger::DeferredSink& StartPlayerLog()
{
    auto sink = Logger::MakeUnique<Logger::DeferredSink>();
    Logger::DeferredSink& heldRecords = *sink;
    Logger::Log::AddSink(std::move(sink));
    return heldRecords;
}

void OpenPlayerLogFile(Logger::DeferredSink& heldRecords, const std::filesystem::path& logsDirectory)
{
    const std::filesystem::path logFile =
        logsDirectory / ("game-" + std::to_string(Platform::GetCurrentProcessId()) + ".log");

    Logger::FileSink::Config fileConfig;
    fileConfig.filename = logFile.generic_string();
    // This process owns the file: a reused process id starts it clean rather than
    // continuing an unrelated earlier session.
    fileConfig.append = false;
    fileConfig.minLevel = Logger::LogLevel::Info;
    fileConfig.includeTimestamp = true;
    fileConfig.includeSource = false;
    // The file is read after a crash; losing its tail to buffering defeats it.
    fileConfig.autoFlush = true;
    auto fileSink = Logger::MakeUnique<Logger::FileSink>(fileConfig);
    const bool fileOpened = fileSink->IsAvailable();
    heldRecords.Attach(std::move(fileSink));
    if (fileOpened)
        Logger::Log::Info("Player: log file '{}'", fileConfig.filename);
    else
        Logger::Log::Error("Player: cannot open the log file '{}'; this run keeps no log file. "
                           "Make the directory writable to restore it.",
                           fileConfig.filename);
}

} // namespace GameEngine

#endif // LOGGER_ENABLE_FILE_LOGGING
