#include "Rendering/Common/Math.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::Rendering {

    // Math utilities for the rendering library
    void InitializeMath() {
        // Silenced: avoid noisy initialization logs
    }

	    // Convert a canonical column-major matrix into the layout expected by the
	    // active graphics API when uploading to shaders.
	    Matrix4x4 ConvertMatrixForShader(const Matrix4x4& matrix, GraphicsAPI api)
	    {
	        switch (api)
	        {
	        case GraphicsAPI::Vulkan:
	        case GraphicsAPI::Metal:
	        case GraphicsAPI::WebGPU:
	        case GraphicsAPI::Auto:
	            // Vulkan/GLSL expect column-major matrices by default and our
	            // Mathematics::Matrix4x4 is already column-major, so we can
	            // upload the matrix as-is.
	            return matrix;
	
	        case GraphicsAPI::DirectX12:
	            // The original implementation assumed row-major CPU matrices and
	            // uploaded them directly for DirectX 12 while transposing for
	            // Vulkan/OpenGL. Now that our canonical matrices are
	            // column-major, we transpose only for DirectX to preserve the
	            // same shader-visible layout.
	            return GameEngine::Mathematics::Transpose(matrix);
	        }
	
	        // Fallback: treat unknown APIs like Vulkan/GLSL and upload as-is.
	        return matrix;
	    }

	    // MatrixUtils implementations moved to individual files to avoid circular dependencies

	} // namespace GameEngine::Rendering
