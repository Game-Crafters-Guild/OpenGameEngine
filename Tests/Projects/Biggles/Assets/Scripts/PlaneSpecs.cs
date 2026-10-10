namespace GameEngine.Scripts;
using System.Collections.Generic;


public class PlaneSpecs
{
    public struct FlightSpecs
    {
        public float CruiseSpeed;
        public float DiveSpeed;
        public float ClimbSpeed;
        public float ClimbAngle;
        public float StallSpeed;
        public float Acceleration;
        public float TurnSpeed;
    }

    public FlightSpecs Flight;
}

public static class PlaneLibrary
{
    public static PlaneSpecs AircoDH2 = new PlaneSpecs
    {
        Flight = new PlaneSpecs.FlightSpecs
        {
            DiveSpeed = 120,
            CruiseSpeed = 100,
            ClimbSpeed = 60,
            ClimbAngle = 45,
            StallSpeed = 55,
            Acceleration = 10,
            TurnSpeed = 60,
        }
    };

    public enum Model
    {
        AircoDH2 = 0,
    }

    public static PlaneSpecs[] Library =
    {
        AircoDH2,
    };
}