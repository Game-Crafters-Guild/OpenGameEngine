#pragma once

// Portable environment-variable control for tests.
//
// An empty value means "remove the variable". That matches _putenv_s, where
// passing "" deletes the entry; setenv(name, "", 1) would instead leave the
// variable defined but empty, which any getenv-presence check reads as set.

#include <cstdlib>

namespace GameEngine::Testing
{
inline void SetEnvVar(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value && *value)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}
} // namespace GameEngine::Testing
