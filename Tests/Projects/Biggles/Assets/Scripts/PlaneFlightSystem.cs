using System;
using System.Numerics;
using GameEngine.Scripting;
using GameEngine.Scripts;

// Ported from 'Hunt for the Red Baron'
public partial struct PlaneFlightSystem : IEntitySystem
{
    const float Gravity = 9.81f;
    

    public void Execute(
        ref Transform xform,
        ref PlaneFlightState state,
        ref PlaneFlightControls controls,
        float deltaTime)
    {
        PlaneSpecs specs = PlaneLibrary.Library[(int)state.Model];
        
        float targetSpeed = state.Revs * specs.Flight.CruiseSpeed;
        float diff = Single.Abs(targetSpeed - state.Speed);
        float change = Single.Clamp(specs.Flight.Acceleration * deltaTime * (diff > 0 ? 1 : -1), -diff, diff);
        state.Speed += change;
        xform.Translate( xform.Forward * state.Speed * deltaTime);
        
        

    }
}
