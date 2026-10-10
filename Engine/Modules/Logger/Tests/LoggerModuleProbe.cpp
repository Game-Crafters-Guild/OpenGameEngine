#include "LoggerModuleProbe.h"
#include "Logger/Logger.h"
#include <cstring>

namespace
{
LoggerModuleProbeStats* s_Stats = nullptr;
bool s_LogOnDetach = false;

struct Argument
{
    unsigned Value;
    explicit Argument(unsigned value) : Value(value) { ++s_Stats->LiveArguments; }
    Argument(const Argument& value) : Argument(value.Value) {}
    Argument(Argument&& value) noexcept : Argument(value.Value) {}
    ~Argument() { --s_Stats->LiveArguments; }
};
struct DetachLog
{
    ~DetachLog()
    {
        if (s_LogOnDetach)
            Logger::Log::Info("detach log {}", 71);
    }
} s_Detach;
} // namespace

template <> struct std::formatter<Argument> : std::formatter<unsigned>
{
    auto format(const Argument& argument, std::format_context& context) const
    {
        ++s_Stats->Formatted;
        return std::formatter<unsigned>::format(argument.Value, context);
    }
};

extern "C" __declspec(dllexport) void ConfigureProbe(LoggerModuleProbeStats* stats,
                                                     bool logOnDetach)
{
    s_Stats = stats;
    s_LogOnDetach = logOnDetach;
}

extern "C" __declspec(dllexport) void EmitOwnedProbe(unsigned value, bool formatted)
{
    // Module storage the probe overwrites after every emission. A snapshot that
    // borrowed these buffers would report the overwritten bytes from the second
    // call onwards, and would name a retired image after the module unloads.
    static char file[] = "fixture.cpp";
    static char function[] = "EmitOwnedProbe";
    static const Logger::OwnedSourceLocation source(file, function);
    if (formatted)
        Logger::Log::LogWithSource(Logger::LogLevel::Info, source, 91, "probe value {}",
                                   Argument(value));
    else
        Logger::Log::LogWithSource(Logger::LogLevel::Info, source, 91,
                                   std::string_view("probe plain text"));
    std::memset(file, 'x', sizeof(file) - 1);
    std::memset(function, 'x', sizeof(function) - 1);
}

extern "C" __declspec(dllexport) void EmitProbe(unsigned value, bool formatted)
{
    char file[] = "fixture.cpp";
    char function[] = "EmitProbe";
    if (formatted)
        Logger::Log::LogWithSource(Logger::LogLevel::Info, file, 91, function, "probe value {}",
                                   Argument(value));
    else
        Logger::Log::LogWithSource(Logger::LogLevel::Info, file, 91, function,
                                   std::string_view("probe plain text"));
    std::memset(file, 'x', sizeof(file) - 1);
    std::memset(function, 'x', sizeof(function) - 1);
}
