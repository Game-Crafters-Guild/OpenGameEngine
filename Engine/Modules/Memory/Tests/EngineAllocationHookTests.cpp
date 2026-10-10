// The allocation counter sees allocations made inside Engine from a test executable.
//
// On Windows every image has its own operator new: a replacement in this executable alone does
// not see Engine.dll's allocations, so Engine.dll carries its own hook (under
// GE_DEBUG_INSTRUMENTATION) counting into the same counter. This pin guards that Windows setup.
// On macOS and Linux the replacements coalesce process-wide (and libc++ builds the string inside
// its own library), so the pin passes there with or without Engine's hook. Registered only where
// Engine's allocations are counted (Memory/Tests/CMakeLists.txt).

#include "Memory/AllocationCountScope.h"
#include "Rendering/Materials/SurfaceShaderTemplate.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace
{

/// Receives each test-side string's buffer, so the compiler cannot elide the allocation the
/// reference window counts.
const char* volatile g_StringSink = nullptr;

} // namespace

// MakeSurfaceShaderTemplateSource is a non-template Engine function that builds one long
// std::string from a literal, made by Engine's code. The expected count is the same work done in
// this executable inside its own window: one heap buffer per string, plus whatever the standard
// library adds per string in this configuration (MSVC's Debug iterator debugging allocates a
// container proxy for each), so the comparison stays exact in every configuration and, on
// Windows, reads 0 against an Engine.dll without the hook.
TEST(EngineAllocationHookTests, AnAllocationInsideEngineIsCountedByTheTestProcess)
{
    constexpr int kCalls = 16;
    const std::size_t sourceLength = GameEngine::Rendering::MakeSurfaceShaderTemplateSource().size();
    ASSERT_GT(sourceLength, sizeof(std::string)) << "the source must not fit the small-string buffer";

    std::uint64_t testSideAllocations = 0;
    {
        const GameEngine::Memory::AllocationCountScope scope(GameEngine::Memory::CountWindow::ThisThread);
        for (int i = 0; i < kCalls; ++i)
        {
            const std::string sameLength(sourceLength, 'x');
            g_StringSink = sameLength.data();
        }
        testSideAllocations = scope.Count();
    }
    ASSERT_GE(testSideAllocations, static_cast<std::uint64_t>(kCalls))
        << "this executable's own allocations are not counted: its hook is missing (cmake/AllocationHook.cmake)";

    std::size_t totalLength = 0;
    std::uint64_t engineAllocations = 0;
    {
        const GameEngine::Memory::AllocationCountScope scope(GameEngine::Memory::CountWindow::ThisThread);
        for (int i = 0; i < kCalls; ++i)
            totalLength += GameEngine::Rendering::MakeSurfaceShaderTemplateSource().size();
        engineAllocations = scope.Count();
    }

    EXPECT_EQ(totalLength, sourceLength * kCalls);
    EXPECT_EQ(engineAllocations, testSideAllocations)
        << "Engine's allocations are not reaching the allocation counter: every image that allocates "
           "needs the hook (Engine/Modules/Memory/Include/Memory/AllocationHook.inl)";
}
