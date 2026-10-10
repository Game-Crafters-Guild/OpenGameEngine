#pragma once

#include "Types/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Editor
{

// Log a spline recipe's validation lines against its entity when they differ
// from `loggedValidation`, the lines that entity last logged, and remember them.
// A rebuild that finds the same problems says nothing, fixing them clears the
// latch, and breaking them again reports them again, so a recipe that rebuilds
// on every settle cannot fill the log. The lines are the latch key: they name
// what is wrong and where, not measurements that move while the author drags.
// Returns whether it logged.
bool ReportRecipeValidation(const std::vector<std::string>& validation,
                            std::string_view entityName, uint32 entityId,
                            std::vector<std::string>& loggedValidation);

} // namespace GameEngine::Editor
