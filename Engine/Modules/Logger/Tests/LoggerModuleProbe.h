#pragma once
#include <atomic>

struct LoggerModuleProbeStats
{
    std::atomic<unsigned> Formatted{0};
    std::atomic<unsigned> LiveArguments{0};
};

using ConfigureLoggerModuleProbe = void (*)(LoggerModuleProbeStats*, bool);
using EmitLoggerModuleProbe = void (*)(unsigned, bool);
