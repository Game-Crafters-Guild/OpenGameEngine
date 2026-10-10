using System;
using System.Runtime.CompilerServices;
using GameEngine.ECS;
using GameEngine.Scripting;

namespace WASDDemo
{
    /// <summary>
    /// IEntitySystem that reads WASD input and updates Position for all entities
    /// that have Position + Velocity. Velocity.X stores the movement speed.
    /// This is source-generated: the chunk iteration, query caching, and type ID
    /// resolution are all emitted by EntitySystemGenerator.
    /// </summary>
    public partial struct WASDMovementSystem : IEntitySystem
    {
        public int Order => 10;

        public void Execute(ref Position pos, in Velocity vel, float deltaTime)
        {
            float speed = vel.X * deltaTime;

            Input.IsKeyDown(KeyCode.W, out bool w);
            Input.IsKeyDown(KeyCode.S, out bool s);
            Input.IsKeyDown(KeyCode.A, out bool a);
            Input.IsKeyDown(KeyCode.D, out bool d);
            Input.IsKeyDown(KeyCode.Space, out bool space);
            Input.IsKeyDown(KeyCode.LeftControl, out bool ctrl);

            if (w) pos.Z += speed;
            if (s) pos.Z -= speed;
            if (a) pos.X -= speed;
            if (d) pos.X += speed;
            if (space) pos.Y += speed;
            if (ctrl) pos.Y -= speed;
        }
    }

    /// <summary>
    /// Play mode lifecycle: creates a player cube on enter, syncs Position to
    /// Transform each frame (so the renderer sees movement), cleans up on exit.
    /// </summary>
    public static class WASDDemoSetup
    {
        private static uint s_PlayerEntity;
        private static ulong s_PosTypeId;
        private static ulong s_VelTypeId;

        [PlayModeEnter]
        public static int OnEnter()
        {
            try
            {
                var world = Ecs.PrimaryWorld;
                s_PosTypeId = ComponentType<Position>.CachedId;
                s_VelTypeId = ComponentType<Velocity>.CachedId;

                // Create a cube as the player.
                s_PlayerEntity = world.CreatePrimitive(PrimitiveType.Cube, "Player");

                // Set initial Transform position.
                var matrix = world.GetTransformMatrix(s_PlayerEntity);
                matrix[12] = 0f; matrix[13] = 1f; matrix[14] = 0f;
                world.SetTransformMatrix(s_PlayerEntity, matrix);

                // Add Position component (used by WASDMovementSystem for chunk iteration).
                SetComponent(world, s_PlayerEntity, s_PosTypeId, new Position(0f, 1f, 0f));

                // Add Velocity component (X = movement speed in units/sec).
                SetComponent(world, s_PlayerEntity, s_VelTypeId, new Velocity(5f, 0f, 0f));

                Console.WriteLine("[WASDDemo] Player spawned. WASD=move, Space=up, LCtrl=down");
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[WASDDemo] OnEnter failed: {ex.Message}");
            }
            return 0;
        }

        [PlayModeTick]
        public static int OnTick(float deltaTime)
        {
            if (s_PlayerEntity == 0) return 0;

            try
            {
                // Sync Position → Transform so the renderer sees movement.
                var world = Ecs.PrimaryWorld;
                Span<byte> buf = stackalloc byte[Unsafe.SizeOf<Position>()];
                if (world.TryGetComponentBytesInto(s_PlayerEntity, s_PosTypeId, buf, out _))
                {
                    var pos = Unsafe.ReadUnaligned<Position>(ref buf[0]);
                    var m = world.GetTransformMatrix(s_PlayerEntity);
                    m[12] = pos.X;
                    m[13] = pos.Y;
                    m[14] = pos.Z;
                    world.SetTransformMatrix(s_PlayerEntity, m);
                }
            }
            catch { }
            return 0;
        }

        [PlayModeExit]
        public static int OnExit()
        {
            s_PlayerEntity = 0;
            return 0;
        }

        private static void SetComponent<T>(WorldHandle world, uint entityId, ulong typeId, T value)
            where T : unmanaged
        {
            var bytes = new byte[Unsafe.SizeOf<T>()];
            Unsafe.As<byte, T>(ref bytes[0]) = value;
            world.SetComponentBytes(entityId, typeId, bytes);
        }
    }
}
