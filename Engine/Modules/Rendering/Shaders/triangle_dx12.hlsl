// DirectX 12 HLSL Shaders for Triangle Rendering

// Vertex Output / Pixel Input Structure
struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

// Vertex Shader - Hardcoded triangle using only SV_VertexID
VertexOutput VSMain(uint vertexID : SV_VertexID)
{
    VertexOutput output;

    // Create a hardcoded triangle using vertex ID, completely bypassing vertex input
    float2 positions[3] = {
        float2( 0.0f,  0.8f), // Top center
        float2(-0.8f, -0.8f), // Bottom left
        float2( 0.8f, -0.8f)  // Bottom right
    };

    output.position = float4(positions[vertexID], 0.0f, 1.0f);
    output.color = float3(0.0f, 1.0f, 0.0f); // Bright green
    return output;
}

// Pixel Shader
float4 PSMain(VertexOutput input) : SV_TARGET
{
    // Test: Output solid bright green to verify triangle is being rendered
    return float4(0.0f, 1.0f, 0.0f, 1.0f); // Bright green
    // Original: return float4(input.color, 1.0f);
}
