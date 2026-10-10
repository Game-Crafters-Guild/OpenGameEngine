using System.Runtime.InteropServices;
using GameEngine.Scripting;

namespace GameEngine.Scripts;

[StructLayout( LayoutKind.Sequential)]
public struct PlaneFlightControls : IComponent
{
    public float Throttle;
    public float Elevator;
    public float Aileron;
}