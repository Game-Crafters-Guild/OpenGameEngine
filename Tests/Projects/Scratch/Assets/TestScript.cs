using System;
using GameEngine;

namespace TestAssets
{
    /// <summary>
    /// Test C# script for AssetBrowser validation
    /// </summary>
    public class TestScript : MonoBehaviour
    {
        public float testValue = 1.0f;
        public string testString = "Hello World";
        
        public void Start()
        {
            Console.WriteLine("TestScript started!");
        }
        
        public void Update()
        {
            // Test update logic
        }
    }
}
