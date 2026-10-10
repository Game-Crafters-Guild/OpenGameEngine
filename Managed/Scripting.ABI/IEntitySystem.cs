namespace GameEngine.Scripting
{
    /// <summary>
    /// Implement on a <c>partial struct</c> to define a per-entity ECS system with
    /// source-generated chunk iteration. The source generator emits the boilerplate
    /// that iterates matching entities via raw memory spans with zero per-entity
    /// ABI transitions.
    ///
    /// <para><b>Usage:</b> Declare an <c>Execute</c> method whose parameters define
    /// which components the system processes:</para>
    ///
    /// <code>
    /// [StructLayout(LayoutKind.Sequential)]
    /// public struct Velocity : IComponent { public float X, Y, Z; }
    ///
    /// public partial struct MovementSystem : IEntitySystem
    /// {
    ///     void Execute(ref Position pos, in Velocity vel, float deltaTime)
    ///     {
    ///         pos.X += vel.X * deltaTime;
    ///         pos.Y += vel.Y * deltaTime;
    ///         pos.Z += vel.Z * deltaTime;
    ///     }
    /// }
    /// </code>
    ///
    /// <para><b>Parameter conventions:</b></para>
    /// <list type="bullet">
    ///   <item><c>ref T</c> — read-write access to component T (required on entity)</item>
    ///   <item><c>in T</c> — read-only access to component T (required on entity)</item>
    ///   <item><c>float deltaTime</c> — injected frame delta time</item>
    ///   <item><c>uint entityId</c> — injected entity ID for the current entity</item>
    /// </list>
    ///
    /// <para>All component types must be <c>unmanaged</c> structs implementing
    /// <see cref="IComponent"/> with <c>[StructLayout(LayoutKind.Sequential)]</c>.</para>
    ///
    /// <para>Use <c>[Without(typeof(Static))]</c> on the struct to exclude entities
    /// with specific components from the query.</para>
    ///
    /// <para><b>Important:</b> Do not perform structural changes (entity create/destroy,
    /// component add/remove) inside <c>Execute</c>. Structural changes are deferred and
    /// flushed after all systems complete for the frame.</para>
    /// </summary>
    /// <remarks>
    /// The source generator emits a companion partial struct with:
    /// <list type="bullet">
    ///   <item><c>__RegisterSystem()</c> — registers component types and the system with GameSystemRunner</item>
    ///   <item><c>__Execute(ulong, float)</c> — chunk iteration loop</item>
    ///   <item><c>__Destroy()</c> — cleanup cached query handles</item>
    /// </list>
    /// </remarks>
    public interface IEntitySystem
    {
        /// <summary>Execution order. Lower runs first. Default 0.</summary>
        int Order => 0;

        /// <summary>Whether this system is currently enabled. Can be toggled via
        /// <c>GameSystemRunner.SetSystemEnabled(name, enabled)</c>.</summary>
        bool Enabled => true;
    }
}
