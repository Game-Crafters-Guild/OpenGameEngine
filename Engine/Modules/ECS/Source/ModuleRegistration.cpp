#include "ECS/ModuleRegistration.h"

namespace GameEngine {
namespace ECS {

namespace {
// Function-local static: registrars run during LoadLibrary static init, so the
// stamp must be construct-on-first-use like the registry maps it feeds.
ModuleRegistrationStamp& ActiveStamp()
{
    static ModuleRegistrationStamp s_Stamp;
    return s_Stamp;
}
} // namespace

void SetActiveRegistrationModule(std::string_view moduleId, std::uint64_t generation)
{
    ModuleRegistrationStamp& stamp = ActiveStamp();
    stamp.ModuleId.assign(moduleId);
    stamp.Generation = generation;
}

void ClearActiveRegistrationModule()
{
    ModuleRegistrationStamp& stamp = ActiveStamp();
    stamp.ModuleId.clear();
    stamp.Generation = 0;
}

const ModuleRegistrationStamp& GetActiveRegistrationModule()
{
    return ActiveStamp();
}

} // namespace ECS
} // namespace GameEngine
