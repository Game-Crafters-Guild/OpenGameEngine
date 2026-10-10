#pragma once

#include <cstdlib>
#include <string>

namespace GameEngine::Rendering::Tests
{

inline void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

inline void UnsetEnvVar(const char* key)
{
#if defined(_WIN32)
    _putenv_s(key, ""); // an empty value removes the variable
#else
    unsetenv(key);
#endif
}

/// Sets an environment variable for the enclosing scope and restores the prior
/// state on every exit path.
///
/// The environment is process-wide, so a test that sets a fault-injection
/// variable and clears it at the end of its body still leaks that variable
/// whenever an assertion returns early — and every later test in the same
/// process then runs under an injection it never asked for.
class ScopedEnvVar
{
public:
    ScopedEnvVar(const char* key, const char* value) : m_Key(key)
    {
        const char* previous = std::getenv(key);
        m_HadValue = previous != nullptr;
        if (previous)
            m_Saved = previous;
        SetEnvVar(key, value);
    }

    ~ScopedEnvVar()
    {
        if (m_HadValue)
            SetEnvVar(m_Key, m_Saved.c_str());
        else
            UnsetEnvVar(m_Key);
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    const char* m_Key;
    std::string m_Saved;
    bool        m_HadValue;
};

} // namespace GameEngine::Rendering::Tests
