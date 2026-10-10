#pragma once

/**
 * @file DiscreteGpuPreference.h
 * @brief Asks the NVIDIA and AMD drivers on Windows laptops with switchable
 * graphics to run the process on the discrete GPU.
 *
 * The drivers read these two symbols from the export table of the process's
 * executable when the process starts; an export from a DLL is ignored. Include
 * this header in exactly one source file of each executable (its main.cpp) and
 * nowhere else: it defines the variables, so a second inclusion in the same
 * executable fails to link. On a machine with a single GPU the symbols are
 * never read.
 */

#if defined(_WIN32)
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
