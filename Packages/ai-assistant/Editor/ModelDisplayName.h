#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{
/// A model id as its vendor names the model: "claude-opus-5-5" (a date or a "[1m]"
/// suffix after it is dropped) reads "Opus 5.5", the alias "opus" reads "Opus"; any
/// other id is returned as it is.
std::string ModelDisplayName(std::string_view model);
} // namespace GameEngine
