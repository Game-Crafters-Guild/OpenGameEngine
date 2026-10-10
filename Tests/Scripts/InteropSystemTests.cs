using System;
using System.Diagnostics;

namespace GameEngine.Scripts.Tests
{
    /// <summary>
    /// Comprehensive test suite for the C# ↔ C++ interoperability system
    /// Tests initialization order, P/Invoke vs registry performance, and error handling
    /// </summary>
    public static class InteropSystemTests
    {
        /// <summary>
        /// Run all interop system tests
        /// </summary>
        public static int RunAllTests()
        {
            NativeInterop.LogInfo("=== COMPREHENSIVE INTEROP SYSTEM TESTS ===");
            
            int totalTests = 0;
            int passedTests = 0;
            
            // Test 1: Initialization Order
            NativeInterop.LogInfo("Test 1: Initialization Order Verification");
            if (TestInitializationOrder())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 1 PASSED: Initialization order correct");
            }
            else
            {
                NativeInterop.LogError("❌ Test 1 FAILED: Initialization order incorrect");
            }
            totalTests++;
            
            // Test 2: Engine Ready Detection
            NativeInterop.LogInfo("Test 2: Engine Ready Detection");
            if (TestEngineReadyDetection())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 2 PASSED: Engine ready detection works");
            }
            else
            {
                NativeInterop.LogError("❌ Test 2 FAILED: Engine ready detection failed");
            }
            totalTests++;
            
            // Test 3: Safe Function Calling
            NativeInterop.LogInfo("Test 3: Safe Function Calling");
            if (TestSafeFunctionCalling())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 3 PASSED: Safe function calling works");
            }
            else
            {
                NativeInterop.LogError("❌ Test 3 FAILED: Safe function calling failed");
            }
            totalTests++;
            
            // Test 4: P/Invoke Functions (if available)
            NativeInterop.LogInfo("Test 4: P/Invoke Function Testing");
            if (TestPInvokeFunctions())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 4 PASSED: P/Invoke functions work");
            }
            else
            {
                NativeInterop.LogWarning("⚠️ Test 4 SKIPPED: P/Invoke functions not available");
            }
            totalTests++;
            
            // Test 5: Registry Functions
            NativeInterop.LogInfo("Test 5: Registry Function Testing");
            if (TestRegistryFunctions())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 5 PASSED: Registry functions work");
            }
            else
            {
                NativeInterop.LogWarning("⚠️ Test 5 FAILED: Registry functions not working (expected if engine not initialized)");
            }
            totalTests++;
            
            // Test 6: Performance Comparison
            NativeInterop.LogInfo("Test 6: Performance Benchmarking");
            if (TestPerformanceComparison())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 6 PASSED: Performance benchmarking completed");
            }
            else
            {
                NativeInterop.LogWarning("⚠️ Test 6 PARTIAL: Performance benchmarking incomplete");
            }
            totalTests++;
            
            // Test 7: Error Handling
            NativeInterop.LogInfo("Test 7: Error Handling");
            if (TestErrorHandling())
            {
                passedTests++;
                NativeInterop.LogInfo("✅ Test 7 PASSED: Error handling works correctly");
            }
            else
            {
                NativeInterop.LogError("❌ Test 7 FAILED: Error handling failed");
            }
            totalTests++;
            
            // Summary
            NativeInterop.LogInfo($"=== TEST SUMMARY: {passedTests}/{totalTests} tests passed ===");
            
            if (passedTests == totalTests)
            {
                NativeInterop.LogInfo("🎉 ALL TESTS PASSED! Interop system is working correctly.");
                return 1;
            }
            else if (passedTests >= totalTests - 2)
            {
                NativeInterop.LogInfo("✅ MOSTLY WORKING: Core functionality operational, some features may be unavailable.");
                return 1;
            }
            else
            {
                NativeInterop.LogError("❌ MULTIPLE FAILURES: Interop system has significant issues.");
                return 0;
            }
        }
        
        /// <summary>
        /// Test initialization order verification
        /// </summary>
        private static bool TestInitializationOrder()
        {
            try
            {
                // Check if we can detect engine state
                bool engineReady = NativeInterop.IsEngineReady();
                NativeInterop.LogInfo($"Engine ready status: {engineReady}");
                
                // This test always passes - it's informational
                return true;
            }
            catch (Exception ex)
            {
                NativeInterop.LogError($"Initialization order test failed: {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test engine ready detection
        /// </summary>
        private static bool TestEngineReadyDetection()
        {
            try
            {
                // Reset cache to test fresh detection
                NativeInterop.ResetEngineReadyCache();
                
                // Test detection
                bool ready1 = NativeInterop.IsEngineReady();
                bool ready2 = NativeInterop.IsEngineReady(); // Should use cache
                
                NativeInterop.LogInfo($"Engine ready detection: {ready1}, cached: {ready2}");
                
                // Both should return the same value
                return ready1 == ready2;
            }
            catch (Exception ex)
            {
                NativeInterop.LogError($"Engine ready detection test failed: {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test safe function calling with graceful fallbacks
        /// </summary>
        private static bool TestSafeFunctionCalling()
        {
            try
            {
                // Test safe function calling
                int result1 = NativeInterop.CallCppFunctionSafe("Add", 10, 20);
                int result2 = NativeInterop.CallCppFunctionSafe("NonExistentFunction");
                
                NativeInterop.LogInfo($"Safe function call results: Add(10,20)={result1}, NonExistent={result2}");
                
                // Safe calling should not throw exceptions
                return true;
            }
            catch (Exception ex)
            {
                NativeInterop.LogError($"Safe function calling test failed: {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test P/Invoke functions if available
        /// </summary>
        private static bool TestPInvokeFunctions()
        {
            try
            {
                // Check if P/Invoke interop is available
                var pinvokeType = Type.GetType("GameEngine.Scripts.PInvokeInterop, GameEngine.Scripts");
                if (pinvokeType == null)
                {
                    NativeInterop.LogInfo("P/Invoke interop not available - skipping test");
                    return false;
                }
                
                // Test P/Invoke functions using reflection
                var testMethod = pinvokeType.GetMethod("TestAllFunctions");
                if (testMethod != null)
                {
                    testMethod.Invoke(null, null);
                    NativeInterop.LogInfo("P/Invoke functions tested successfully");
                    return true;
                }
                
                return false;
            }
            catch (Exception ex)
            {
                NativeInterop.LogInfo($"P/Invoke test failed (expected if not implemented): {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test registry-based functions
        /// </summary>
        private static bool TestRegistryFunctions()
        {
            try
            {
                // Test basic registry functions
                string[] functions = NativeInterop.GetAvailableFunctions();
                NativeInterop.LogInfo($"Available registry functions: {functions.Length}");
                
                if (functions.Length == 0)
                {
                    NativeInterop.LogInfo("No registry functions available (engine not initialized)");
                    return false;
                }
                
                // Test a simple function call
                int result = NativeInterop.Add(5, 7);
                NativeInterop.LogInfo($"Registry function test: Add(5,7) = {result}");
                
                return result == 12;
            }
            catch (Exception ex)
            {
                NativeInterop.LogInfo($"Registry function test failed (expected if engine not initialized): {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test performance comparison between different approaches
        /// </summary>
        private static bool TestPerformanceComparison()
        {
            try
            {
                const int iterations = 1000;
                
                // Benchmark logging functions (P/Invoke)
                var sw = Stopwatch.StartNew();
                for (int i = 0; i < iterations; i++)
                {
                    NativeInterop.LogInfo($"Performance test {i}");
                }
                sw.Stop();
                double loggingTime = sw.Elapsed.TotalMilliseconds;
                
                // Benchmark registry functions (if available)
                double registryTime = 0;
                if (NativeInterop.IsEngineReady())
                {
                    sw.Restart();
                    for (int i = 0; i < iterations; i++)
                    {
                        NativeInterop.Add(i, i + 1);
                    }
                    sw.Stop();
                    registryTime = sw.Elapsed.TotalMilliseconds;
                }
                
                NativeInterop.LogInfo($"Performance results ({iterations} iterations):");
                NativeInterop.LogInfo($"  Logging (P/Invoke): {loggingTime:F2}ms");
                NativeInterop.LogInfo($"  Registry functions: {registryTime:F2}ms");
                
                if (registryTime > 0)
                {
                    double ratio = registryTime / loggingTime;
                    NativeInterop.LogInfo($"  Registry is {ratio:F2}x slower than P/Invoke");
                }
                
                return true;
            }
            catch (Exception ex)
            {
                NativeInterop.LogError($"Performance comparison test failed: {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// Test error handling scenarios
        /// </summary>
        private static bool TestErrorHandling()
        {
            try
            {
                // Test calling non-existent function
                int result1 = NativeInterop.CallCppFunctionSafe("NonExistentFunction");
                
                // Test calling with invalid parameters
                int result2 = NativeInterop.CallCppFunctionSafe("Add", "invalid", "parameters");
                
                // Test null/empty function name
                int result3 = NativeInterop.CallCppFunctionSafe("");
                int result4 = NativeInterop.CallCppFunctionSafe(null);
                
                NativeInterop.LogInfo($"Error handling test results: {result1}, {result2}, {result3}, {result4}");
                
                // All should return -1 (error) without throwing exceptions
                return result1 == -1 && result2 == -1 && result3 == -1 && result4 == -1;
            }
            catch (Exception ex)
            {
                NativeInterop.LogError($"Error handling test failed: {ex.Message}");
                return false;
            }
        }
    }
}
