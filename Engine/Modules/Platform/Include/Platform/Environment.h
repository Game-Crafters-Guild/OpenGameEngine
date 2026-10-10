#pragma once

#include "Types/StringUtils.h"

#include <cstdlib>
#include <initializer_list>
#include <string_view>

namespace GameEngine::Platform
{

/// Read a developer switch from the process environment.
///
/// Unset or empty returns `valueWhenUnset`. "0", "false", "off" and "no"
/// (case-insensitive) are off; any other value is on. The web build
/// reaches the same values through the query string
/// (Platform/WebEnvironment.h), so a switch works the same way on both.
inline bool EnvironmentSwitchEnabled(const char* name, bool valueWhenUnset)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return valueWhenUnset;
    for (const std::string_view off : {"0", "false", "off", "no"})
    {
        if (EqualsIgnoreCase(value, off))
            return false;
    }
    return true;
}

} // namespace GameEngine::Platform
