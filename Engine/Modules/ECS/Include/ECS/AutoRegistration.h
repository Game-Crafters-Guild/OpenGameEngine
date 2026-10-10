#pragma once

#include "ComponentConcepts.h"
#include "ComponentRegistry.h"
#include "ComponentTypeName.h"
#include <string>
#include <string_view>

namespace GameEngine::ECS {

/**
 * @brief Universal automatic component registrar
 *
 * This class uses static initialization to automatically register ALL components
 * when they are first used. This provides a completely unified experience with
 * zero manual registration required for any component type.
 *
 * @tparam T The component type to register
 */
template<Component T>
class AutoComponentRegistrar {
private:
    // The registry name is the consteval, cross-compiler-stable type name
    // (ComponentTypeName.h), never typeid(T).name(): that is the MANGLED name on
    // the Itanium ABI (clang/gcc), so a macOS or Linux build would register
    // "N10GameEngine10Components9TransformE" and every name-keyed lookup — scene
    // load, ComponentFieldRegistry::FindByName, the debug server's component
    // vocabulary — would miss it.
    static constexpr std::string_view kTypeName = ComponentTypeName<T>();

    // Static initialization ensures registration happens once per component type
    static inline bool registered = []() {
        ComponentRegistry::RegisterComponent<T>(std::string(kTypeName));
        return true;
    }();

public:
    /**
     * @brief Ensure the component is registered
     *
     * This method triggers the static initialization if it hasn't happened yet,
     * and re-registers the component if the registry was cleared between tests.
     */
    static void EnsureRegistered() {
        // Touch the static to trigger first-time registration
        (void)registered;

        // If the registry has been cleared after initial registration (e.g., in tests),
        // re-register the handler and metadata idempotently.
        const ComponentTypeId typeId = GetComponentTypeId<T>();
        if (ComponentRegistry::GetHandler(typeId) == nullptr) {
            ComponentRegistry::RegisterComponent<T>(std::string(kTypeName));
        }
    }

    /**
     * @brief Check if the component has been registered
     *
     * @return true if the component is registered, false otherwise
     */
    static bool IsRegistered() {
        return registered;
    }
};

} // namespace GameEngine::ECS
