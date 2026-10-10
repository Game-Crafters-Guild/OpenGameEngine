using System.Runtime.InteropServices;
using GameEngine.Scripting;

namespace GameEngine.Scripts;

// [ComponentPreset("Planes", "Plane (Full)")]
// [PresetComponent(typeof(PlaneFlightState))]
// [PresetComponent(typeof(PlaneFlightControls))]
// public static class PlanePreset { }

[StructLayout(LayoutKind.Sequential)]
public struct PlaneFlightState : IComponent
{
    public enum PlaneState
    {
        Taxiing,
        Flying,
        Stalling,
        Rolling,
        Crashing,
        Coresample,
    }

    public float Speed;
    public float Revs; // 0..1
    public float PitchAngle; // accumulated degrees
    public float RollAngle;
    public float YawAngle;
    public PlaneLibrary.Model Model;
    public PlaneState State;
}
