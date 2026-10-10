#pragma once

#include "Rendering/Common/Math.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Rendering {

    /**
     * @brief Matrix utilities for graphics API conversion
     * 
     * Provides helper functions for converting matrices between different
     * graphics API conventions (row-major vs column-major).
     */
    namespace MatrixUtils {
        
        /**
         * @brief Convert matrix for shader upload based on graphics API
         * 
         * @param matrix The matrix to convert
         * @param api The graphics API being used
         * @return Matrix in the correct format for the specified API
         */
	        inline Matrix4x4 ToShader(const Matrix4x4& matrix, GraphicsAPI api) noexcept {
	            // Delegate to the central conversion helper so all matrix uploads
	            // go through the same column-major -> API-specific path.
	            return ConvertMatrixForShader(matrix, api);
	        }
        
        /**
         * @brief Get the appropriate matrix conversion method name for an API
         * 
         * @param api The graphics API
         * @return String describing the conversion method
         */
	        inline const char* GetConversionMethodName(GraphicsAPI api) noexcept {
	            switch (api) {
	                case GraphicsAPI::Vulkan:   return "ConvertMatrixForShader(matrix, GraphicsAPI::Vulkan)";
	                case GraphicsAPI::DirectX12:return "ConvertMatrixForShader(matrix, GraphicsAPI::DirectX12)";
	                default:                    return "ConvertMatrixForShader(matrix, api)";
	            }
	        }
        
        /**
         * @brief Check if an API requires matrix transposition
         *
         * @param api The graphics API
         * @return True if the API requires transposition (column-major)
         */
	        inline bool RequiresTransposition(GraphicsAPI api) noexcept {
	            // With Mathematics::Matrix4x4 as the canonical column-major
	            // representation, we now transpose only for DirectX-style row-
	            // major expectations.
	            switch (api) {
	                case GraphicsAPI::Vulkan:   return false; // Column-major, no transpose needed
	                case GraphicsAPI::DirectX12:return true;  // Row-major, transpose from canonical
	                default:                    return false; // Treat unknown like Vulkan
	            }
	        }

        // Compute the normal matrix = transpose(inverse(upper3x3(m))) from a
        // column-major 4x4 model matrix, written into three vec4 columns with
        // the matrix data in .xyz (w = 0). Matches GPUInstance::normalMatrixCol*
        // layout and the per-vertex expansion in instance_io.glsl. Returns the
        // identity-columns fallback when the upper 3x3 is singular.
        //
        // When outMirrored is non-null it receives whether the upper-3x3
        // determinant is negative — an odd number of mirror/negative-scale axes,
        // which reverses rasterized triangle winding. Extraction stamps this as
        // GPUInstance.flags bit 4 so the scatter path can split mirrored
        // instances into a flipped-front-face batch. Reusing the determinant
        // already computed here avoids a second per-instance det evaluation. A
        // singular matrix reports false (winding is undefined; the identity
        // fallback keeps normals sane).
        //
        // Pulled out of its two prior inline copies (RenderServices.cpp,
        // RenderExtractionSystem.cpp) to prevent silent drift.
        inline void ComputeNormalMatrixColumns(const float m[16],
                                               Mathematics::Vector4& outN0,
                                               Mathematics::Vector4& outN1,
                                               Mathematics::Vector4& outN2,
                                               bool* outMirrored = nullptr) noexcept
        {
            const float a00 = m[0],  a01 = m[4],  a02 = m[8];
            const float a10 = m[1],  a11 = m[5],  a12 = m[9];
            const float a20 = m[2],  a21 = m[6],  a22 = m[10];

            const float c00 =  (a11 * a22 - a12 * a21);
            const float c01 = -(a10 * a22 - a12 * a20);
            const float c02 =  (a10 * a21 - a11 * a20);
            const float det = a00 * c00 + a01 * c01 + a02 * c02;
            if (det > -1e-18f && det < 1e-18f)
            {
                if (outMirrored)
                    *outMirrored = false;
                outN0 = Mathematics::Vector4(1.0f, 0.0f, 0.0f, 0.0f);
                outN1 = Mathematics::Vector4(0.0f, 1.0f, 0.0f, 0.0f);
                outN2 = Mathematics::Vector4(0.0f, 0.0f, 1.0f, 0.0f);
                return;
            }
            if (outMirrored)
                *outMirrored = det < 0.0f;
            const float invDet = 1.0f / det;
            outN0 = Mathematics::Vector4(
                c00 * invDet, -(a01 * a22 - a02 * a21) * invDet,  (a01 * a12 - a02 * a11) * invDet, 0.0f);
            outN1 = Mathematics::Vector4(
                c01 * invDet,  (a00 * a22 - a02 * a20) * invDet, -(a00 * a12 - a02 * a10) * invDet, 0.0f);
            outN2 = Mathematics::Vector4(
                c02 * invDet, -(a00 * a21 - a01 * a20) * invDet,  (a00 * a11 - a01 * a10) * invDet, 0.0f);
        }

        // The largest singular value of a column-major 4x4's upper 3x3: the
        // greatest factor by which the transform can lengthen ANY direction.
        // Scaling a mesh's local bounding radius by this is what makes the
        // world-space bounding sphere actually contain the transformed mesh.
        //
        // The obvious max(|col0|, |col1|, |col2|) is that factor only while the
        // columns are mutually PERPENDICULAR. A column norm is the stretch
        // applied to one local axis; with a perpendicular basis no direction
        // between the axes can stretch further, but a SHEARED one lengthens the
        // diagonal between two columns past either of them. The sphere then
        // fails to contain the mesh it bounds, and the object is culled while
        // part of it is still on screen — a pop at the frustum edge, which no
        // still frame can show.
        //
        // Two producers of sheared instance transforms ship, both in spline
        // placement: the seam shear that closes a tile wedge at a bend
        // (Placement/SeamShear.h) and SplineSpanGrade::Sheared, which slants a
        // fence span onto a grade while its up axis stays with its stations.
        //
        // Cost, measured rather than argued — both formulas at /O2 on x64, best
        // of 4000 reps over an L1-resident batch, so it is the ARITHMETIC and
        // therefore an upper bound on the delta inside a loop that also streams
        // its matrices. The perpendicular path runs 3.52 -> 4.49 ns per
        // instance, +0.98 ns: trading two of the three square roots for three
        // dot products does not quite pay for them. Streamed from a 4 MB
        // working set, where memory rather than arithmetic sets the pace, the
        // same delta measures +0.07 ns. The sheared path costs 6.6 -> 24.6 ns,
        // and only a sheared instance reaches it. Over a 3628-entity scene
        // those bound the change at 3.6 us/frame with nothing sheared and about
        // 5 us/frame with every runtime-built piece sheared — either way under
        // 0.05% of a 16.7 ms frame.
        //
        // The result never falls BELOW max(|col_i|): a singular value bounds
        // every column norm from above, so this can only grow a sphere a
        // previous build published, never shrink one.
        [[nodiscard]] inline float LargestAxisScale(const float m[16]) noexcept
        {
            const float sq0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
            const float sq1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6];
            const float sq2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
            const float largestSq = std::max({sq0, sq1, sq2});

            const float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
            const float d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
            const float d12 = m[4] * m[8] + m[5] * m[9] + m[6] * m[10];

            // p1 is the sum of the squared off-diagonals of S = M^T M. It is
            // BOTH the orthogonality test and the first term of the solve.
            const float p1 = d01 * d01 + d02 * d02 + d12 * d12;

            // Relative, because the dots grow with the SQUARE of the column
            // lengths and an absolute epsilon would call a merely large
            // orthogonal matrix sheared. Loose on purpose: a shear small enough
            // to pass here moves the answer by about half its own relative
            // size, so admitting 1e-3 of shear admits ~0.05% of radius — a
            // fraction of a millimetre on a 1.5 m panel — while keeping every
            // rounding-noise-orthogonal transform on the cheap path, and a
            // composed parent-local chain accumulates far more than one ULP.
            // Erring the other way would buy correctness that is already there
            // and pay a per-instance transcendental for the whole scene.
            constexpr float kShearRelativeTolerance = 1.0e-3f;
            constexpr float kOrthogonalP1Tolerance =
                kShearRelativeTolerance * kShearRelativeTolerance;
            if (p1 <= kOrthogonalP1Tolerance * largestSq * largestSq)
                return std::sqrt(largestSq); // == max(|col_i|); sqrt is monotonic

            // Sheared. The singular values of M are the square roots of the
            // eigenvalues of the symmetric S = M^T M, and a symmetric 3x3 has a
            // closed form for its eigenvalues, so nothing has to iterate.
            const float q = (sq0 + sq1 + sq2) * (1.0f / 3.0f);
            const float e0 = sq0 - q;
            const float e1 = sq1 - q;
            const float e2 = sq2 - q;
            const float p2 = e0 * e0 + e1 * e1 + e2 * e2 + 2.0f * p1;
            const float p = std::sqrt(p2 * (1.0f / 6.0f));
            if (!(p > 0.0f))
                return std::sqrt(largestSq); // S is a multiple of the identity

            const float invP = 1.0f / p;
            const float b00 = e0 * invP, b11 = e1 * invP, b22 = e2 * invP;
            const float b01 = d01 * invP, b02 = d02 * invP, b12 = d12 * invP;
            const float detB = b00 * (b11 * b22 - b12 * b12) -
                               b01 * (b01 * b22 - b12 * b02) +
                               b02 * (b01 * b12 - b11 * b02);
            const float r = std::clamp(detB * 0.5f, -1.0f, 1.0f);
            const float phi = std::acos(r) * (1.0f / 3.0f);
            const float largestEigenvalue = q + 2.0f * p * std::cos(phi);
            // Never report less than a column already measures: the clamp costs
            // nothing and stops a rounding wobble from shrinking a sphere.
            return std::sqrt(std::max(largestEigenvalue, largestSq));
        }

        // Note: GPU data structure conversion functions are implemented
        // directly in the files that use them to avoid circular dependencies

        /**
         * @brief Validate that matrices are properly converted for GPU upload
         *
         * This function helps catch common matrix conversion errors during development.
         * It should be called before uploading matrix data to GPU buffers.
         *
         * @param matrixName Name of the matrix for error reporting
         * @param api The graphics API being used
         * @return True if validation passes, false otherwise
         */
        [[nodiscard]] bool ValidateMatrixConversion(const char* matrixName, GraphicsAPI api);

	        /**
	         * @brief Macro to help ensure matrix conversion is not forgotten
	         *
	         * Use this macro when uploading matrices to remind about conversion:
	         * ENSURE_MATRIX_CONVERTED(ConvertMatrixForShader(myMatrix, api), api);
	         */
	        #define ENSURE_MATRIX_CONVERTED(convertedMatrix, api) \
	            do { \
	                static_assert(true, "Matrix conversion reminder: Use ConvertMatrixForShader(matrix, " #api ") or MatrixUtils::ToShader(matrix, " #api ")"); \
	            } while(0)
    }

} // namespace Rendering
} // namespace GameEngine
