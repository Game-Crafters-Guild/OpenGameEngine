#pragma once

#include "Handle.h"
#include <GenerationalVector/GenerationalVector.hpp>
#include <string>
#include <functional>

namespace GameEngine {
namespace Rendering {

    /**
     * @brief High-performance container for rendering resources
     * 
     * This template provides O(1) resource management using generational handles.
     * It replaces the previous std::unordered_map approach with significant
     * performance improvements.
     * 
     * @tparam T The resource type to store
     */
    template<typename T>
    class ResourceContainer {
    public:
        using ResourceType = T;
        using HandleType = Handle;
        using ContainerType = GenerationalVector::GenerationalVector<T>;

        ResourceContainer() = default;
        ~ResourceContainer() = default;

        // Non-copyable but movable
        ResourceContainer(const ResourceContainer&) = delete;
        ResourceContainer& operator=(const ResourceContainer&) = delete;
        ResourceContainer(ResourceContainer&&) = default;
        ResourceContainer& operator=(ResourceContainer&&) = default;

        /**
         * @brief Create a new resource
         * @tparam Args Constructor argument types
         * @param args Constructor arguments for the resource
         * @return Handle to the created resource
         */
        template<typename... Args>
        Handle Create(Args&&... args) {
            return m_Container.Create(std::forward<Args>(args)...);
        }

        /**
         * @brief Destroy a resource
         * @param handle Handle to the resource to destroy
         */
        void Destroy(Handle handle) {
            if (IsValid(handle)) {
                m_Container.Destroy(handle);
            }
        }

        /**
         * @brief Get a reference to a resource
         * @param handle Handle to the resource
         * @return Reference to the resource
         * @note Asserts if handle is invalid in debug builds
         */
        T& Get(Handle handle) {
            return m_Container[handle];
        }

        /**
         * @brief Get a const reference to a resource
         * @param handle Handle to the resource
         * @return Const reference to the resource
         * @note Asserts if handle is invalid in debug builds
         */
        const T& Get(Handle handle) const {
            return m_Container[handle];
        }

        /**
         * @brief Get a pointer to a resource if handle is valid
         * @param handle Handle to the resource
         * @return Pointer to the resource, or nullptr if handle is invalid
         */
        T* TryGet(Handle handle) {
            return m_Container.Get(handle);
        }

        /**
         * @brief Get a const pointer to a resource if handle is valid
         * @param handle Handle to the resource
         * @return Const pointer to the resource, or nullptr if handle is invalid
         */
        const T* TryGet(Handle handle) const {
            return m_Container.Get(handle);
        }

        /**
         * @brief Check if a handle is valid and points to a live resource
         * @param handle Handle to check
         * @return True if the handle is valid
         */
        bool IsValid(Handle handle) const {
            return m_Container.IsValid(handle);
        }

        /**
         * @brief Get the number of resources currently stored
         * @return Number of live resources
         */
        size_t Size() const {
            return m_Container.Size();
        }

        /**
         * @brief Check if the container is empty
         * @return True if no resources are stored
         */
        bool Empty() const {
            return m_Container.Empty();
        }

        /**
         * @brief Reserve space for a certain number of resources
         * @param capacity Number of resources to reserve space for
         */
        void Reserve(size_t capacity) {
            m_Container.Reserve(capacity);
        }

        /**
         * @brief Clear all resources
         */
        void Clear() {
            m_Container.Clear();
        }

        /**
         * @brief Iterate over all valid resources
         * @tparam Func Function type that takes (Handle, T&)
         * @param func Function to call for each resource
         */
        template<typename Func>
        void ForEach(Func&& func) {
            m_Container.ForEach(std::forward<Func>(func));
        }

        /**
         * @brief Iterate over all valid resources (const version)
         * @tparam Func Function type that takes (Handle, const T&)
         * @param func Function to call for each resource
         */
        template<typename Func>
        void ForEach(Func&& func) const {
            m_Container.ForEach(std::forward<Func>(func));
        }

        /**
         * @brief Find a resource by predicate
         * @tparam Predicate Function type that takes (const T&) and returns bool
         * @param pred Predicate function
         * @return Handle to the first matching resource, or INVALID_HANDLE if not found
         */
        template<typename Predicate>
        Handle FindIf(Predicate&& pred) const {
            Handle result = INVALID_HANDLE;
            ForEach([&](Handle handle, const T& resource) {
                if (result == INVALID_HANDLE && pred(resource)) {
                    result = handle;
                }
            });
            return result;
        }

        /**
         * @brief Count resources matching a predicate
         * @tparam Predicate Function type that takes (const T&) and returns bool
         * @param pred Predicate function
         * @return Number of matching resources
         */
        template<typename Predicate>
        size_t CountIf(Predicate&& pred) const {
            size_t count = 0;
            ForEach([&](Handle, const T& resource) {
                if (pred(resource)) {
                    ++count;
                }
            });
            return count;
        }

    private:
        ContainerType m_Container;
    };

    /**
     * @brief Convenience type aliases for common resource containers
     */
    template<typename T> using BufferContainer = ResourceContainer<T>;
    template<typename T> using TextureContainer = ResourceContainer<T>;
    template<typename T> using SamplerContainer = ResourceContainer<T>;
    template<typename T> using PipelineContainer = ResourceContainer<T>;

} // namespace Rendering
} // namespace GameEngine
