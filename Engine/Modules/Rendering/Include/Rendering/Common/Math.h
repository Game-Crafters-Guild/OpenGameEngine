/**
 * @file Math.h
 * @brief Math utilities for the rendering library
 * 
 * Provides vector, matrix, and utility math functions for 3D graphics.
 */

#pragma once

#include <cmath>
#include <cstring>

// Canonical engine-wide math (GLM-based) used as the underlying implementation
// for rendering math. Rendering keeps its own lightweight row-major types for
// public API and GPU conversion, but delegates construction to Mathematics.
#include "Mathematics/Types.h"
#include "Mathematics/MatrixOps.h"

namespace GameEngine {
namespace Rendering {
    // Forward declaration for GraphicsAPI enum
    enum class GraphicsAPI;

    	// Rendering now uses the canonical Mathematics vector types directly.
	    using Vector3 = GameEngine::Mathematics::Vector3;
	    using Vector4 = GameEngine::Mathematics::Vector4;

	    // Rendering matrices are now the canonical Mathematics matrices (column-major).
	    using Matrix4x4 = GameEngine::Mathematics::Matrix4x4;

	    // Helper to convert a matrix into the layout expected by the active graphics API
	    // when uploading to GPU buffers. Implemented in Math.cpp to avoid pulling
	    // in Device.h from every Math.h include.
	    Matrix4x4 ConvertMatrixForShader(const Matrix4x4& matrix, GraphicsAPI api);

    // Math utility functions
    namespace Math {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;
        constexpr float kHalfPi = 0.5f * kPi;

        constexpr float ToRadians(float degrees) noexcept {
            return degrees * (kPi / 180.0f);
        }

        constexpr float ToDegrees(float radians) noexcept {
            return radians * (180.0f / kPi);
        }

        constexpr float Clamp(float value, float min, float max) noexcept {
            return value < min ? min : (value > max ? max : value);
        }

        constexpr float Lerp(float a, float b, float t) noexcept {
            return a + t * (b - a);
        }

        inline Vector3 Lerp(const Vector3& a, const Vector3& b, float t) noexcept {
            return Vector3(
                Lerp(a.x, b.x, t),
                Lerp(a.y, b.y, t),
                Lerp(a.z, b.z, t)
            );
        }
    }

} // namespace Rendering
} // namespace GameEngine
