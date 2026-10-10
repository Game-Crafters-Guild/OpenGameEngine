#pragma once

/**
 * @file GenerationalVector.hpp
 * @brief Main header for the GenerationalVector library
 * 
 * This header includes all the necessary components for using the
 * GenerationalVector system in your project.
 */

#include "GenerationalHandle.h"
#include "GenerationalManager.h"
#include "GenerationalVector.h"

/**
 * @namespace GenerationalVector
 * @brief Contains all GenerationalVector library components
 * 
 * The GenerationalVector library provides a high-performance, memory-safe
 * way to manage resources using generational handles. This prevents
 * use-after-free bugs and provides O(1) access to resources.
 * 
 * Key components:
 * - Handle: A generational handle that safely references resources
 * - GenerationalManager: Manages handle generation and validation
 * - GenerationalVector<T>: A container that stores elements accessible via handles
 * 
 * Example usage:
 * @code
 * #include <GenerationalVector/GenerationalVector.hpp>
 * 
 * struct MyResource {
 *     int value;
 *     std::string name;
 * };
 * 
 * GenerationalVector::GenerationalVector<MyResource> resources;
 * 
 * // Create a resource
 * auto handle = resources.Create(42, "test");
 * 
 * // Access the resource
 * if (resources.IsValid(handle)) {
 *     MyResource& resource = resources[handle];
 *     resource.value = 100;
 * }
 * 
 * // Destroy the resource
 * resources.Destroy(handle);
 * 
 * // Handle is now invalid
 * assert(!resources.IsValid(handle));
 * @endcode
 */
