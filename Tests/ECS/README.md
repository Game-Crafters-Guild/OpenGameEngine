# ECS Test Suite

This directory contains comprehensive tests for the ECS (Entity Component System) library.

## Test Organization

### Google Test Framework Tests
These tests are integrated with the CMake build system and CTest:

- **StandaloneECSTests** (`StandaloneECSTest.cpp`) - Core ECS functionality tests
- **AutoRegistrationTests** (`AutoRegistrationTests.cpp`) - Auto-registration system tests  
- **ECSIntegrationTests** (`IntegrationTest.cpp`) - Cross-library component integration tests
- **MinimalECSTests** (`MinimalECSTest.cpp`) - Basic ECS functionality tests
- **AdditionalECSTests** - Combined tests from:
  - `NewBatchCreationTest.cpp` - Batch entity creation tests
  - `PublicAPITest.cpp` - Public API validation tests
  - `UnifiedComponentSystemTest.cpp` - Unified component system tests

### Standalone Files (Not Built by Default)

- **UnifiedSystemDemo.cpp** - Interactive demonstration of the unified component system
  - Purpose: Shows how the ECS system works in practice
  - Usage: Can be built manually for educational purposes
  - Not included in automated tests as it's meant for human interaction

- **UnifiedSystemVerification.cpp** - Compile-time verification of component concepts
  - Purpose: Ensures component concepts work correctly at compile time
  - Usage: Included by other test files for static assertions
  - Not a runnable test, but provides compile-time validation

## Running Tests

### All ECS Tests
```bash
ctest --test-dir build -C Debug -R "ECS|StandaloneECS|AutoRegistration|Integration|Minimal|Additional"
```

### Specific Test Suites
```bash
# Core ECS functionality
./build/bin/Debug/StandaloneECSTests.exe

# Auto-registration system
./build/bin/Debug/AutoRegistrationTests.exe

# Cross-library integration
./build/bin/Debug/ECSIntegrationTests.exe

# Basic functionality
./build/bin/Debug/MinimalECSTests.exe

# Additional features
./build/bin/Debug/AdditionalECSTests.exe
```

## Test Coverage

The test suite covers:

- ✅ Entity creation and destruction
- ✅ Component addition, retrieval, and removal
- ✅ Component existence checking
- ✅ Basic and advanced querying
- ✅ Archetype management
- ✅ World clearing and management
- ✅ Component registry functionality
- ✅ C++20 auto-registration system
- ✅ Thread safety and stress testing
- ✅ Large-scale component handling
- ✅ Error handling and validation
- ✅ Serialization (Entity, World, JSON)
- ✅ Performance benchmarking
- ✅ Cross-library component integration
- ✅ Multi-component entity support

## Architecture Validation

The tests validate the modular ECS architecture:

1. **ECS Core** - Minimal and generic, only contains essential components
2. **Engine Library** - Contains transform components with auto-registration
3. **Test Library** - Contains test-specific components with auto-registration
4. **Cross-Library Integration** - Components from different libraries work seamlessly

All tests pass with zero compilation warnings and demonstrate production-ready quality.
