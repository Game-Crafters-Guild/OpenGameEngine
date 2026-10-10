# GenerationalVector Library

A high-performance, memory-safe generational handle system for C++20. This library provides O(1) access to resources while preventing use-after-free bugs through generational validation.

## Features

- **Memory Safety**: Handles become invalid when resources are destroyed, preventing use-after-free bugs
- **High Performance**: O(1) creation, access, and destruction operations
- **Generational Validation**: Automatic detection of stale handle usage
- **Header-Only**: Easy integration with minimal dependencies
- **Modern C++**: Requires C++20 for optimal performance and safety
- **Thread-Safe Design**: Can be made thread-safe with external synchronization

## Quick Start

```cpp
#include <GenerationalVector/GenerationalVector.hpp>

struct MyResource {
    int value;
    std::string name;
    
    MyResource(int v, const std::string& n) : value(v), name(n) {}
};

// Create a container
GenerationalVector::GenerationalVector<MyResource> resources;

// Create a resource
auto handle = resources.Create(42, "test");

// Access the resource safely
if (resources.IsValid(handle)) {
    MyResource& resource = resources[handle];
    resource.value = 100;
}

// Destroy the resource
resources.Destroy(handle);

// Handle is now invalid - safe to check
assert(!resources.IsValid(handle));
```

## Core Components

### Handle
A lightweight 64-bit handle that combines an index and generation:
- **Index**: Points to the slot in the container (32 bits)
- **Generation**: Incremented each time a slot is reused (32 bits)

### GenerationalManager
Manages handle generation and validation:
- Tracks which handles are alive
- Provides efficient slot allocation and reuse
- Validates handle generations

### GenerationalVector<T>
The main container that stores elements:
- O(1) element access via handles
- Automatic memory management
- Safe iteration over live elements

## Performance Characteristics

- **Creation**: O(1) amortized
- **Access**: O(1) 
- **Destruction**: O(1)
- **Validation**: O(1)
- **Memory Overhead**: ~8 bytes per slot + element size

## Integration

### CMake Integration

```cmake
# Add the library to your project
add_subdirectory(GenerationalVector)

# Link to your target
target_link_libraries(your_target PRIVATE GenerationalVector)
```

### Manual Integration

Simply copy the `Include/GenerationalVector/` directory to your project and include the main header:

```cpp
#include "GenerationalVector/GenerationalVector.hpp"
```

## Advanced Usage

### Iteration
```cpp
// Iterate over all live elements
resources.ForEach([](Handle handle, MyResource& resource) {
    std::cout << "Resource " << handle.Value() << ": " << resource.name << "\n";
});
```

### Safe Access
```cpp
// Safe pointer access (returns nullptr for invalid handles)
if (auto* resource = resources.Get(handle)) {
    resource->value = 200;
}
```

### Performance Optimization
```cpp
// Reserve space to avoid reallocations
resources.Reserve(10000);

// Check capacity
std::cout << "Capacity: " << resources.Capacity() << "\n";
std::cout << "Size: " << resources.Size() << "\n";
```

## Comparison with Alternatives

| Approach | Access Time | Memory Safety | Memory Overhead |
|----------|-------------|---------------|-----------------|
| Raw Pointers | O(1) | ❌ Unsafe | Minimal |
| std::shared_ptr | O(1) | ✅ Safe | High |
| std::weak_ptr | O(1) | ✅ Safe | High |
| GenerationalVector | O(1) | ✅ Safe | Low |

## Building and Testing

```bash
# Configure with tests enabled
cmake -B build -DBUILD_TESTING=ON

# Build
cmake --build build

# Run tests
ctest --test-dir build
```

## Requirements

- C++20 compatible compiler
- CMake 3.20 or later (for building)
- Google Test (for running tests, optional)

## License

This library is part of the GameEngine project and follows the same licensing terms.
