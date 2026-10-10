using System;
using GameEngine.Scripting;

namespace GameScripts
{
		public static class SampleInit
		{
			[InitializeOnLoad]
			public static void Boot()
			{
				string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "<null>";
				Console.WriteLine($"[Test] InitializeOnLoad Boot() called v9 (GE_HOTRELOAD_IOL_TRACE_FILE='{tracePath}')");
			}
		}
}

