using System.Numerics;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Mirror of C++ GameEngine::Components::Transform (64 bytes, column-major).
    ///
    /// Memory layout matches C++ GameEngine::Components::Transform.
    /// Columns: [0..3] = X axis, [4..7] = Y axis, [8..11] = Z axis, [12..15] = translation.
    ///
    /// Note: System.Numerics.Matrix4x4 is row-major. Use ToMatrix4x4() / FromMatrix4x4()
    /// which transpose on conversion so the math comes out correct.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    [BuiltInComponent("GameEngine::Components::Transform")]
    public struct Transform : IComponent
    {
        // 4x4 matrix in column-major order, matching the C++ layout.
        public unsafe fixed float M[16];

        // ------------------------------------------------------------------ //
        // Construction
        // ------------------------------------------------------------------ //

        /// <summary>Identity transform.</summary>
        public Transform()
        {
            unsafe
            {
                M[0] = 1f; M[5] = 1f; M[10] = 1f; M[15] = 1f;
            }
        }

        /// <summary>
        /// LH TRS: matrix * v == rot.Rotate(v). +Y yaw sends +Z toward +X.
        /// Matches C++ Transform::FromTRS.
        /// </summary>
        public static Transform FromTRS(float px, float py, float pz,
                                        float qx, float qy, float qz, float qw,
                                        float sx, float sy, float sz)
            => MakeTRS(px, py, pz, qx, qy, qz, qw, sx, sy, sz, leftHanded: true);

        /// <summary>
        /// RH TRS: transpose of FromTRS. +Y yaw sends +Z toward -X.
        /// Matches C++ Transform::FromTRSRH.
        /// </summary>
        public static Transform FromTRSRH(float px, float py, float pz,
                                          float qx, float qy, float qz, float qw,
                                          float sx, float sy, float sz)
            => MakeTRS(px, py, pz, qx, qy, qz, qw, sx, sy, sz, leftHanded: false);

        /// <summary>Build a transform from position, rotation and scale using System.Numerics types.</summary>
        public static Transform FromTRSRH(Vector3 position, Quaternion rotation, Vector3 scale)
            => FromTRSRH(position.X, position.Y, position.Z,
                         rotation.X, rotation.Y, rotation.Z, rotation.W,
                         scale.X, scale.Y, scale.Z);

        static Transform MakeTRS(float px, float py, float pz,
                                 float qx, float qy, float qz, float qw,
                                 float sx, float sy, float sz, bool leftHanded)
        {
            float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
            float xx = qx * x2, xy = qx * y2, xz = qx * z2;
            float yy = qy * y2, yz = qy * z2, zz = qz * z2;
            float wx = qw * x2, wy = qw * y2, wz = qw * z2;

            float r00 = 1f - (yy + zz);
            float r11 = 1f - (xx + zz);
            float r22 = 1f - (xx + yy);
            float r01 = leftHanded ? (xy - wz) : (xy + wz);
            float r02 = leftHanded ? (xz + wy) : (xz - wy);
            float r10 = leftHanded ? (xy + wz) : (xy - wz);
            float r12 = leftHanded ? (yz - wx) : (yz + wx);
            float r20 = leftHanded ? (xz - wy) : (xz + wy);
            float r21 = leftHanded ? (yz + wx) : (yz - wx);

            var t = new Transform();
            unsafe
            {
                t.M[0] = r00 * sx; t.M[1] = r10 * sx; t.M[2] = r20 * sx; t.M[3] = 0f;
                t.M[4] = r01 * sy; t.M[5] = r11 * sy; t.M[6] = r21 * sy; t.M[7] = 0f;
                t.M[8] = r02 * sz; t.M[9] = r12 * sz; t.M[10] = r22 * sz; t.M[11] = 0f;
                t.M[12] = px; t.M[13] = py; t.M[14] = pz; t.M[15] = 1f;
            }
            return t;
        }

        /// <summary>Build a transform from position, rotation and scale using System.Numerics types.</summary>
        public static Transform FromTRS(Vector3 position, Quaternion rotation, Vector3 scale)
            => FromTRS(position.X, position.Y, position.Z,
                       rotation.X, rotation.Y, rotation.Z, rotation.W,
                       scale.X, scale.Y, scale.Z);

        // ------------------------------------------------------------------ //
        // Accessors
        // ------------------------------------------------------------------ //

        /// <summary>Extract the translation (column 3).</summary>
        public readonly Vector3 GetPosition()
        {
            unsafe { return new Vector3(M[12], M[13], M[14]); }
        }

        public readonly float X { get { unsafe { return M[12]; } } }
        public readonly float Y { get { unsafe { return M[13]; } } }
        public readonly float Z { get { unsafe { return M[14]; } } }

        /// <summary>
        /// Extract the rotation as a quaternion.
        /// Matches C++ Transform::GetRotation — removes scale from column lengths first.
        /// </summary>
        public readonly Quaternion GetRotation()
        {
            unsafe
            {
                float sx = MathF.Sqrt(M[0]*M[0] + M[1]*M[1] + M[2]*M[2]);
                float sy = MathF.Sqrt(M[4]*M[4] + M[5]*M[5] + M[6]*M[6]);
                float sz = MathF.Sqrt(M[8]*M[8] + M[9]*M[9] + M[10]*M[10]);

                if (sx <= 0f || sy <= 0f || sz <= 0f)
                    return Quaternion.Identity;

                float invSx = 1f / sx, invSy = 1f / sy, invSz = 1f / sz;

                float r00 = M[0] * invSx, r10 = M[1] * invSx, r20 = M[2] * invSx;
                float r01 = M[4] * invSy, r11 = M[5] * invSy, r21 = M[6] * invSy;
                float r02 = M[8] * invSz, r12 = M[9] * invSz, r22 = M[10] * invSz;

                float trace = r00 + r11 + r22;
                float qw, qx, qy, qz;

                if (trace > 0f)
                {
                    float s = MathF.Sqrt(trace + 1f) * 2f;
                    qw = 0.25f * s;
                    qx = (r21 - r12) / s;
                    qy = (r02 - r20) / s;
                    qz = (r10 - r01) / s;
                }
                else if (r00 > r11 && r00 > r22)
                {
                    float s = MathF.Sqrt(1f + r00 - r11 - r22) * 2f;
                    qw = (r21 - r12) / s;
                    qx = 0.25f * s;
                    qy = (r01 + r10) / s;
                    qz = (r02 + r20) / s;
                }
                else if (r11 > r22)
                {
                    float s = MathF.Sqrt(1f + r11 - r00 - r22) * 2f;
                    qw = (r02 - r20) / s;
                    qx = (r01 + r10) / s;
                    qy = 0.25f * s;
                    qz = (r12 + r21) / s;
                }
                else
                {
                    float s = MathF.Sqrt(1f + r22 - r00 - r11) * 2f;
                    qw = (r10 - r01) / s;
                    qx = (r02 + r20) / s;
                    qy = (r12 + r21) / s;
                    qz = 0.25f * s;
                }

                // Quaternion whose Rotate() matches this matrix (FromTRS convention).
                return new Quaternion(qx, qy, qz, qw);
            }
        }

        /// <summary>Extract per-axis scale from column lengths.</summary>
        public readonly Vector3 GetScale()
        {
            unsafe
            {
                return new Vector3(
                    MathF.Sqrt(M[0]*M[0] + M[1]*M[1] + M[2]*M[2]),
                    MathF.Sqrt(M[4]*M[4] + M[5]*M[5] + M[6]*M[6]),
                    MathF.Sqrt(M[8]*M[8] + M[9]*M[9] + M[10]*M[10]));
            }
        }

        // ------------------------------------------------------------------ //
        // Direction vectors (normalised, Y+ up, Z+ forward, left-handed)
        // ------------------------------------------------------------------ //

        /// <summary>Local +X axis (normalised).</summary>
        public readonly Vector3 Right
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[0], M[1], M[2])); } }
        }

        /// <summary>Local -X axis (normalised).</summary>
        public readonly Vector3 Left
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[0], -M[1], -M[2])); } }
        }

        /// <summary>Local +Y axis (normalised).</summary>
        public readonly Vector3 Up
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[4], M[5], M[6])); } }
        }

        /// <summary>Local -Y axis (normalised).</summary>
        public readonly Vector3 Down
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[4], -M[5], -M[6])); } }
        }

        /// <summary>Local +Z axis (normalised).</summary>
        public readonly Vector3 Forward
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[8], M[9], M[10])); } }
        }

        /// <summary>Local -Z axis (normalised).</summary>
        public readonly Vector3 Back
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[8], -M[9], -M[10])); } }
        }

        // ------------------------------------------------------------------ //
        // Mutation
        // ------------------------------------------------------------------ //

        /// <summary>Reset to identity.</summary>
        public void SetIdentity()
        {
            unsafe
            {
                for (int i = 0; i < 16; i++) M[i] = 0f;
                M[0] = M[5] = M[10] = M[15] = 1f;
            }
        }

        /// <summary>Add an offset to the translation column.</summary>
        public void Translate(float x, float y, float z)
        {
            unsafe { M[12] += x; M[13] += y; M[14] += z; }
        }

        /// <summary>Add an offset to the translation column.</summary>
        public void Translate(Vector3 offset) => Translate(offset.X, offset.Y, offset.Z);

        /// <summary>
        /// Overwrite the rotation via FromTRS, keeping position and scale.
        /// Matches C++ Transform::Rotate (xyzw args, wxyz Quaternion).
        /// </summary>
        public void Rotate(float x, float y, float z, float w)
        {
            var p = GetPosition();
            var s = GetScale();
            this = FromTRS(p.X, p.Y, p.Z, x, y, z, w, s.X, s.Y, s.Z);
        }

        /// <summary>Overwrite the rotation part of the matrix from a quaternion (scale is reset to 1).</summary>
        public void Rotate(Quaternion q) => Rotate(q.X, q.Y, q.Z, q.W);

        /// <summary>
        /// Set the diagonal scale values directly (replaces existing scale, does not multiply).
        /// Matches C++ Transform::SetScale.
        /// </summary>
        public void SetScale(float x, float y, float z)
        {
            unsafe { M[0] = x; M[5] = y; M[10] = z; }
        }

        /// <summary>Set the diagonal scale values directly.</summary>
        public void SetScale(Vector3 scale) => SetScale(scale.X, scale.Y, scale.Z);

        /// <summary>Overwrite the translation column.</summary>
        public void SetPosition(float x, float y, float z)
        {
            unsafe { M[12] = x; M[13] = y; M[14] = z; }
        }

        /// <summary>Overwrite the translation column.</summary>
        public void SetPosition(Vector3 position) => SetPosition(position.X, position.Y, position.Z);

        // ------------------------------------------------------------------ //
        // System.Numerics interop
        // ------------------------------------------------------------------ //

        /// <summary>
        /// Convert to System.Numerics.Matrix4x4 (row-major).
        /// Transposes on the way out so Numerics math (transforms, decompose, etc.) is correct.
        /// </summary>
        public readonly Matrix4x4 ToMatrix4x4()
        {
            unsafe
            {
                // C++ column-major -> Numerics row-major: swap row/column indices.
                return new Matrix4x4(
                    M[0],  M[4],  M[8],  M[12],
                    M[1],  M[5],  M[9],  M[13],
                    M[2],  M[6],  M[10], M[14],
                    M[3],  M[7],  M[11], M[15]);
            }
        }

        /// <summary>
        /// Overwrite this transform from a System.Numerics.Matrix4x4 (row-major).
        /// Transposes on the way in.
        /// </summary>
        public void FromMatrix4x4(Matrix4x4 m)
        {
            unsafe
            {
                M[0]  = m.M11; M[1]  = m.M21; M[2]  = m.M31; M[3]  = m.M41;
                M[4]  = m.M12; M[5]  = m.M22; M[6]  = m.M32; M[7]  = m.M42;
                M[8]  = m.M13; M[9]  = m.M23; M[10] = m.M33; M[11] = m.M43;
                M[12] = m.M14; M[13] = m.M24; M[14] = m.M34; M[15] = m.M44;
            }
        }

        public static explicit operator Matrix4x4(Transform t) => t.ToMatrix4x4();

        public override readonly string ToString()
        {
            var p = GetPosition();
            return $"Transform(pos=({p.X:F3}, {p.Y:F3}, {p.Z:F3}))";
        }
    }

    /// <summary>
    /// Mirror of C++ GameEngine::Components::WorldTransform (68 bytes, column-major + version).
    /// Read-only for gameplay scripts — written by TransformHierarchySystem.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    [BuiltInComponent("GameEngine::Components::WorldTransform")]
    public struct WorldTransform : IComponent
    {
        public unsafe fixed float M[16];
        public uint Version;

        /// <summary>Extract world-space position.</summary>
        public readonly Vector3 GetPosition()
        {
            unsafe { return new Vector3(M[12], M[13], M[14]); }
        }

        public readonly float X { get { unsafe { return M[12]; } } }
        public readonly float Y { get { unsafe { return M[13]; } } }
        public readonly float Z { get { unsafe { return M[14]; } } }

        /// <summary>Convert to System.Numerics.Matrix4x4 (transposes column-major -> row-major).</summary>
        public readonly Matrix4x4 ToMatrix4x4()
        {
            unsafe
            {
                return new Matrix4x4(
                    M[0],  M[4],  M[8],  M[12],
                    M[1],  M[5],  M[9],  M[13],
                    M[2],  M[6],  M[10], M[14],
                    M[3],  M[7],  M[11], M[15]);
            }
        }

        public static explicit operator Matrix4x4(WorldTransform t) => t.ToMatrix4x4();

        /// <summary>World-space +X axis (normalised).</summary>
        public readonly Vector3 Right
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[0], M[1], M[2])); } }
        }

        /// <summary>World-space -X axis (normalised).</summary>
        public readonly Vector3 Left
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[0], -M[1], -M[2])); } }
        }

        /// <summary>World-space +Y axis (normalised).</summary>
        public readonly Vector3 Up
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[4], M[5], M[6])); } }
        }

        /// <summary>World-space -Y axis (normalised).</summary>
        public readonly Vector3 Down
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[4], -M[5], -M[6])); } }
        }

        /// <summary>World-space +Z axis (normalised).</summary>
        public readonly Vector3 Forward
        {
            get { unsafe { return Vector3.Normalize(new Vector3(M[8], M[9], M[10])); } }
        }

        /// <summary>World-space -Z axis (normalised).</summary>
        public readonly Vector3 Back
        {
            get { unsafe { return Vector3.Normalize(new Vector3(-M[8], -M[9], -M[10])); } }
        }

        public override readonly string ToString()
        {
            var p = GetPosition();
            return $"WorldTransform(pos=({p.X:F3}, {p.Y:F3}, {p.Z:F3}), v={Version})";
        }
    }
}
