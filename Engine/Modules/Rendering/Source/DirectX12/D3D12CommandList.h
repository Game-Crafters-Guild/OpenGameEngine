/**
 * @file D3D12CommandList.h
 * @brief DirectX 12 implementation of the CommandList interface
 */

#pragma once

#include "Rendering/Core/CommandList.h"
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace GameEngine {
namespace Rendering {

    // Forward declaration
    class D3D12Device;

    /**
     * @brief DirectX 12 implementation of the command list interface
     */
    class D3D12CommandList : public CommandList {
    public:
        explicit D3D12CommandList(D3D12Device* device);
        ~D3D12CommandList() override;

        // CommandList interface
        void Begin() override;
        void End() override;

        // Resource binding
        void SetVertexBuffer(BufferHandle buffer, uint32_t slot = 0) override;
        void SetIndexBuffer(BufferHandle buffer, IndexType indexType = IndexType::Uint16) override;
        void SetConstants(uint32_t slot, size_t size, const void* data) override;
        void SetPipeline(PipelineHandle pipeline) override;
        void BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline) override;



        // Drawing
        void Draw(uint32_t vertexCount, uint32_t instanceCount = 1) override;
        // Subrange overload
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance = 0) override;
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1) override;
        // Subrange overload
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance = 0) override;
        void DrawMeshTasks(uint32_t taskCount) override;

        // Indirect drawing (GPU-driven rendering)
        void DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
        void DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
        void DrawIndexedIndirectCount(BufferHandle commandBuffer,
                                      BufferHandle countBuffer,
                                      uint32_t     maxDrawCount,
                                      uint32_t     stride,
                                      size_t       commandBufferOffset = 0,
                                      size_t       countBufferOffset   = 0) override;

        // Compute
        void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) override;
        void DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes = 0) override;

        // Render passes
        void BeginRenderPass(const RenderPassDesc& desc) override;
        void EndRenderPass() override;

        // Viewport and scissor
        void SetViewport(float x, float y, float width, float height) override;
        void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

        // Resource operations
        void CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset = 0, size_t dstOffset = 0) override;
        void CopyTexture(TextureHandle src, TextureHandle dst) override;
        void FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value) override;


	        void CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer,
	                                 uint32_t width, uint32_t height, uint32_t SrcX = 0, uint32_t SrcY = 0,
	                                 size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0) override;


        // Readback helper for specific mip/layer (or 3D Z-range) — not implemented for D3D12 yet
        void CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
                                           BufferHandle dstBuffer,
                                           uint32_t width, uint32_t height,
                                           uint32_t SrcX = 0, uint32_t SrcY = 0,
                                           size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0,
                                           uint32_t depth = 1, size_t dstSlicePitchBytes = 0) override;

	        // Utility: clear a color image subresource directly (no-op on D3D12 for now)
	        void ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4]) override;


        // Synchronization
        void Barrier(const ResourceBarrier& barrier) override;

        // Debug events
        void BeginEvent(const char* name) override;
        void EndEvent() override;
        void SetMarker(const char* name) override;

        // DirectX 12-specific accessors
        // Query helpers
        void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) override;
        ID3D12GraphicsCommandList* GetD3D12CommandList() const { return m_CommandList.Get(); }

        void ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) override;

        void CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture,
                                            uint32_t mip, uint32_t layer,
                                            uint32_t width, uint32_t height,
                                            size_t srcOffsetBytes, size_t srcRowPitchBytes,
                                            uint32_t depth, size_t srcSlicePitchBytes,
                                            uint32_t dstX, uint32_t dstY,
                                            ResourceState currentState) override;

    private:
        D3D12Device* m_Device;
        ComPtr<ID3D12GraphicsCommandList> m_CommandList;
        ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
        bool m_IsRecording = false;
    };

} // namespace Rendering
} // namespace GameEngine
