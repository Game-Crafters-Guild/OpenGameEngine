// The allocation counter's hook for a test executable. cmake/AllocationHook.cmake adds
// this file to every test executable in every configuration. Production images compile
// ProductionAllocationHook.cpp instead, which carries the hook only under
// GE_DEBUG_INSTRUMENTATION. The replacements themselves are in Memory/AllocationHook.inl,
// which the native SDK's user-module entry source includes as well.

#include "Memory/AllocationHook.inl"
