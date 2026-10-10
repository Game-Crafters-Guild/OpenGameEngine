#include "Placement/RecipeValidation.h"

#include "Logger/Logger.h"

namespace GameEngine::Editor
{

bool ReportRecipeValidation(const std::vector<std::string>& validation,
                            std::string_view entityName, uint32 entityId,
                            std::vector<std::string>& loggedValidation)
{
    if (validation == loggedValidation)
        return false;
    loggedValidation = validation;
    for (const std::string& message : validation)
    {
        if (entityName.empty())
            Logger::Log::Warning("{} (entity {})", message, entityId);
        else
            Logger::Log::Warning("{} ('{}', entity {})", message, entityName, entityId);
    }
    return !validation.empty();
}

} // namespace GameEngine::Editor
