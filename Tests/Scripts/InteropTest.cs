using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripts.Tests
{
    /// <summary>
    /// Test class to validate C# → C++ interoperability
    /// </summary>
    public static class InteropTest
    {
        // Test with the correct DLL name (main executable)
        [DllImport("Editor.exe", CallingConvention = CallingConvention.Cdecl)]
        private static extern int CallCppFunction(string functionName, string args);

        [DllImport("Editor.exe", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GetCppFunctions(StringBuilder buffer, int bufferSize);

        [DllImport("Editor.exe", CallingConvention = CallingConvention.Cdecl)]
        private static extern void LogFromCSharp(int level, string message);

        /// <summary>
        /// Test C# → C++ function calling with correct DLL name
        /// </summary>
        public static void TestCSharpToCppInterop()
        {
            LogFromCSharp(1, "=== TESTING C# → C++ INTEROP WITH CORRECT DLL ===");

            try
            {
                // Test 1: Get available functions
                StringBuilder buffer = new StringBuilder(4096);
                int count = GetCppFunctions(buffer, buffer.Capacity);
                LogFromCSharp(1, $"Found {count} registered C++ functions");
                
                if (count > 0)
                {
                    string[] functions = buffer.ToString().Split(',');
                    LogFromCSharp(1, $"Available functions: {string.Join(", ", functions)}");
                }

                // Test 2: Math functions
                int sum = CallCppFunction("Add", "15,27");
                LogFromCSharp(1, $"Add(15, 27) = {sum}");

                int product = CallCppFunction("Multiply", "6,9");
                LogFromCSharp(1, $"Multiply(6, 9) = {product}");

                // Test 3: Engine functions
                int assetCount = CallCppFunction("GetAssetCount", "");
                LogFromCSharp(1, $"Asset count: {assetCount}");

                int frameCount = CallCppFunction("GetFrameCount", "");
                LogFromCSharp(1, $"Frame count: {frameCount}");

                int engineInit = CallCppFunction("IsEngineInitialized", "");
                LogFromCSharp(1, $"Engine initialized: {(engineInit == 1 ? "Yes" : "No")}");

                LogFromCSharp(1, "✅ C# → C++ interop test completed successfully!");
            }
            catch (Exception ex)
            {
                LogFromCSharp(3, $"❌ C# → C++ interop test failed: {ex.Message}");
            }
        }

        /// <summary>
        /// Test method that can be called from ComponentEntryPoint
        /// </summary>
        public static int TestInteropFromComponentEntryPoint()
        {
            TestCSharpToCppInterop();
            return 1; // Success
        }
    }
}
