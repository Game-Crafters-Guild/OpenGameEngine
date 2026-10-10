#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace GameEngine
{
/// Sets or clears an environment variable for one test and restores it after.
class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const char* name, const char* value) : m_Name(name)
    {
        if (const char* previous = std::getenv(name))
            m_Previous = previous;
        Set(value);
    }
    ~ScopedEnvironmentVariable() { Set(m_Previous ? m_Previous->c_str() : nullptr); }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s(m_Name.c_str(), value ? value : "");
#else
        if (value)
            setenv(m_Name.c_str(), value, 1);
        else
            unsetenv(m_Name.c_str());
#endif
    }

    std::string m_Name;
    std::optional<std::string> m_Previous;
};
} // namespace GameEngine
