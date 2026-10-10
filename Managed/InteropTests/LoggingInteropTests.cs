using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using GameEngine;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Smoke validation around logging through managed while using the façade.
    /// </summary>
    public class LoggingInteropTests
    {
        /// <summary>
        /// Writes to console and queries the world; intended as a lightweight smoke test.
        /// </summary>
        [Test]
        public void Facade_Smoke_LogAndWorldQuery_DoesNotThrow()
        {
            // Use the facade; prefer not to fail when ECS is unavailable in headless
            // environments, since logging coverage is validated elsewhere.
            try
            {
                var world = EcsTestHelpers.RequirePrimaryWorldOrInconclusive("LoggingInterop.Facade_Smoke_LogAndWorldQuery_DoesNotThrow");
                Console.WriteLine($"[LoggingInteropTests] world.EntityCount={world.EntityCount}");
            }
            finally
            {
                Console.WriteLine("[LoggingInteropTests] basic facade logging smoke completed");
            }
        }
    }
}

