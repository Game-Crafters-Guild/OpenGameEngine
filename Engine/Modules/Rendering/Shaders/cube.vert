#version 450

// Vertex input from vertex buffer
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// Push constants for transformation matrices and culling control
layout(push_constant) uniform PushConstants {
    mat4 viewProjMatrix;
    mat4 modelMatrix;
    uint cullingEnabled;   // 0 = no culling, 1 = culling enabled
    uint visibleCount;     // Number of visible instances
    uint pad0;             // ends the block on mat4's 16-byte alignment, as MSL does
    uint pad1;
} pc;

// Visible instance transform data (when culling is enabled)
struct VisibleInstanceTransform {
    mat4 transform;
    vec3 position;      // Extracted position for easy access
    float scale;        // Uniform scale factor
    vec3 color;         // Instance color for visualization
    uint originalIndex; // Original instance index for debugging
};

// Storage buffer for visible instance transforms (only used when culling is enabled)
layout(set = 0, binding = 0, std430) restrict readonly buffer VisibleTransformsBuffer {
    VisibleInstanceTransform visibleTransforms[];
};

// Output to fragment shader
layout(location = 0) out vec3 fragColor;

// Simple hash function for deterministic random values (must match C++ std::mt19937)
float hash(uint x) {
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) / 4294967295.0;
}

void main() {
    vec4 worldPos;
    vec3 instanceColor;

    if (pc.cullingEnabled != 0u) {
        // CULLING ENABLED: Use compact visible instance transforms buffer
        // gl_InstanceIndex now maps directly to visible instances (0 to visibleCount-1)
        uint visibleInstanceIndex = gl_InstanceIndex;

        // Bounds check for visible transforms buffer
        if (visibleInstanceIndex >= pc.visibleCount) {
            // Invalid instance index, render at origin as fallback
            worldPos = vec4(inPosition, 1.0);
            instanceColor = vec3(1.0, 0.0, 0.0); // Red for error
        } else {
            // Get the visible instance transform data from compact array
            VisibleInstanceTransform visibleInstance = visibleTransforms[visibleInstanceIndex];

            // Apply the full transform matrix from the visible instance
            vec4 localPos = vec4(inPosition, 1.0);
            worldPos = visibleInstance.transform * localPos;

            // FIXED: Use color based on originalIndex for consistency with mode 3
            // This ensures colors match between culled and unculled modes
            uint colorIndex = visibleInstance.originalIndex % 8u;
            if (colorIndex == 0u) instanceColor = vec3(1.0, 0.0, 0.0);      // Red
            else if (colorIndex == 1u) instanceColor = vec3(0.0, 1.0, 0.0); // Green
            else if (colorIndex == 2u) instanceColor = vec3(1.0, 1.0, 1.0); // White
            else if (colorIndex == 3u) instanceColor = vec3(1.0, 1.0, 0.0); // Yellow
            else if (colorIndex == 4u) instanceColor = vec3(1.0, 0.0, 1.0); // Magenta
            else if (colorIndex == 5u) instanceColor = vec3(0.0, 1.0, 1.0); // Cyan
            else if (colorIndex == 6u) instanceColor = vec3(1.0, 0.5, 0.0); // Orange
            else instanceColor = vec3(0.5, 0.0, 1.0);                       // Purple
        }
    } else {
        // CULLING DISABLED: Use visible transforms buffer populated with all instances
        // When culling is disabled, the visible transforms buffer should contain all instances in order
        uint actualInstanceIndex = gl_InstanceIndex;

        // Bounds check for visible transforms buffer
        if (actualInstanceIndex >= pc.visibleCount) {
            // Invalid instance index, render at origin as fallback
            worldPos = vec4(inPosition, 1.0);
            instanceColor = vec3(1.0, 0.0, 0.0); // Red for error
        } else {
            // Get the visible instance transform data (should be all instances when culling disabled)
            VisibleInstanceTransform visibleInstance = visibleTransforms[actualInstanceIndex];

            // Apply the full transform matrix from the visible instance
            vec4 localPos = vec4(inPosition, 1.0);
            worldPos = visibleInstance.transform * localPos;

            // Use the color from the visible instance data
            instanceColor = visibleInstance.color;
        }
    }

    // Apply view-projection transformation
    gl_Position = pc.viewProjMatrix * worldPos;

    // Output final color
    fragColor = instanceColor;
}
