/**
 * @file D3D12CommandList.cpp
 * @brief DirectX 12 command list implementation
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D12CommandList.h"
#include "D3D12Device.h"
#include <algorithm>
#include <iostream>

namespace GameEngine {
namespace Rendering {

    namespace {
        D3D12_RESOURCE_STATES ToD3D12ResourceState(ResourceState state)
        {
            switch (state)
            {
            case ResourceState::Undefined:
            case ResourceState::Common:
                return D3D12_RESOURCE_STATE_COMMON;
            case ResourceState::VertexBuffer:
                return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case ResourceState::IndexBuffer:
                return D3D12_RESOURCE_STATE_INDEX_BUFFER;
            case ResourceState::ConstantBuffer:
                return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case ResourceState::ShaderResource:
            case ResourceState::DepthSampled:
                return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case ResourceState::UnorderedAccess:
                return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case ResourceState::RenderTarget:
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case ResourceState::DepthWrite:
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case ResourceState::DepthRead:
                return D3D12_RESOURCE_STATE_DEPTH_READ;
            case ResourceState::CopySource:
                return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case ResourceState::CopyDest:
                return D3D12_RESOURCE_STATE_COPY_DEST;
            default:
                return D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }

    D3D12CommandList::D3D12CommandList(D3D12Device* device)
        : m_Device(device) {
        std::cout << "D3D12CommandList: Constructor" << std::endl;

        // Create command allocator
        HRESULT hr = m_Device->GetD3D12Device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_CommandAllocator)
        );

        if (FAILED(hr)) {
            std::cerr << "D3D12CommandList: Failed to create command allocator. HRESULT: " << std::hex << hr << std::endl;
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                std::cerr << "   Device lost - this may be due to driver issues or debugging tools" << std::endl;
            }
            return;
        }

        // Create command list
        hr = m_Device->GetD3D12Device()->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_CommandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_CommandList)
        );

        if (FAILED(hr)) {
            std::cerr << "D3D12CommandList: Failed to create command list" << std::endl;
            return;
        }

        // Close the command list initially
        m_CommandList->Close();
    }

    D3D12CommandList::~D3D12CommandList() {
        std::cout << "D3D12CommandList: Destructor" << std::endl;
    }

    void D3D12CommandList::Begin() {
        std::cout << "D3D12CommandList: Begin recording" << std::endl;

        if (m_IsRecording) {
            std::cerr << "D3D12CommandList: Already recording!" << std::endl;
            return;
        }

        // Ensure GPU has finished with this command allocator before resetting
        // In a production implementation, this would use per-frame fence tracking
        // For now, we rely on the device-level synchronization from ExecuteCommandLists

        // Reset command allocator and command list
        // Note: This should only be called after GPU has finished executing previous commands
        HRESULT hr = m_CommandAllocator->Reset();
        if (FAILED(hr)) {
            std::cerr << "D3D12CommandList: Failed to reset command allocator. HRESULT: " << std::hex << hr << std::endl;
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                std::cerr << "   Device lost during command allocator reset" << std::endl;
            }
            return;
        }

        hr = m_CommandList->Reset(m_CommandAllocator.Get(), nullptr);
        if (FAILED(hr)) {
            std::cerr << "D3D12CommandList: Failed to reset command list. HRESULT: " << std::hex << hr << std::endl;
            return;
        }

        m_IsRecording = true;
        std::cout << "✅ D3D12CommandList: Command allocator and list reset successfully" << std::endl;
    }

    void D3D12CommandList::End() {
        std::cout << "D3D12CommandList: End recording" << std::endl;

        if (!m_IsRecording) {
            std::cerr << "D3D12CommandList: Not recording!" << std::endl;
            return;
        }

        m_CommandList->Close();
        m_IsRecording = false;
    }



    // Resource binding
    void D3D12CommandList::SetVertexBuffer(BufferHandle buffer, uint32_t slot) {
        std::cout << "D3D12CommandList: Set vertex buffer " << buffer << " slot " << slot << std::endl;

        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: Cannot set vertex buffer - command list not recording!" << std::endl;
            return;
        }

        // Get the D3D12 buffer from the device
        D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);
        ID3D12Resource* d3d12Buffer = d3d12Device->GetD3D12Buffer(buffer);

        if (!d3d12Buffer) {
            std::cerr << "D3D12CommandList: Invalid buffer handle " << buffer << std::endl;
            return;
        }

        // Create vertex buffer view
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = d3d12Buffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = static_cast<UINT>(d3d12Device->GetBufferSize(buffer));
        vbv.StrideInBytes = 24; // 3 floats position + 3 floats color = 6 * 4 = 24 bytes

        // Set vertex buffer
        m_CommandList->IASetVertexBuffers(slot, 1, &vbv);

        std::cout << "✅ D3D12CommandList: Vertex buffer " << buffer << " bound to slot " << slot << std::endl;
    }

    void D3D12CommandList::SetIndexBuffer(BufferHandle buffer, IndexType indexType) {
        std::cout << "D3D12CommandList: Set index buffer " << buffer << std::endl;

        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: Cannot set index buffer - command list not recording!" << std::endl;
            return;
        }

        // Get D3D12 buffer from device
        auto* d3d12Buffer = m_Device->GetD3D12Buffer(buffer);
        if (!d3d12Buffer) {
            std::cerr << "D3D12CommandList: Invalid index buffer handle " << buffer << std::endl;
            return;
        }

        // Create index buffer view
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        ibv.BufferLocation = d3d12Buffer->GetGPUVirtualAddress();
        ibv.SizeInBytes = static_cast<UINT>(m_Device->GetBufferSize(buffer));
        ibv.Format = (indexType == IndexType::Uint32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;

        // Set index buffer
        m_CommandList->IASetIndexBuffer(&ibv);

        std::cout << "✅ D3D12CommandList: Index buffer " << buffer << " bound" << std::endl;
    }

    void D3D12CommandList::SetConstants(uint32_t slot, size_t size, const void* data) {
        std::cout << "D3D12CommandList: Set constants slot " << slot << " size " << size << std::endl;

        if (!m_IsRecording || !m_CommandList || !data) {
            std::cerr << "D3D12CommandList: Cannot set constants - invalid state or data!" << std::endl;
            return;
        }

        // For slot 0, use root constants (32-bit values)
        if (slot == 0 && size <= 64) { // Max 16 32-bit values
            uint32_t num32BitValues = static_cast<uint32_t>(size / 4);
            m_CommandList->SetGraphicsRoot32BitConstants(slot, num32BitValues, data, 0);
            std::cout << "✅ D3D12CommandList: Root constants set for slot " << slot << " (" << num32BitValues << " values)" << std::endl;
        } else {
            std::cout << "D3D12CommandList: Constants slot " << slot << " not supported or size too large" << std::endl;
        }
    }

    void D3D12CommandList::SetPipeline(PipelineHandle pipeline) {
        std::cout << "D3D12CommandList: Set pipeline " << pipeline << std::endl;

        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: Cannot set pipeline - command list not recording!" << std::endl;
            return;
        }

        // Get the D3D12Pipeline from the device
        D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);
        const D3D12Pipeline* d3d12Pipeline = d3d12Device->GetD3D12Pipeline(pipeline);

        if (!d3d12Pipeline || !d3d12Pipeline->pipelineState) {
            std::cerr << "D3D12CommandList: Invalid pipeline handle " << pipeline << std::endl;
            return;
        }

        // Set pipeline state object and root signature
        m_CommandList->SetPipelineState(d3d12Pipeline->pipelineState.Get());
        m_CommandList->SetGraphicsRootSignature(d3d12Pipeline->rootSignature.Get());

        // Set primitive topology
        m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        std::cout << "✅ D3D12CommandList: Pipeline state and root signature set" << std::endl;
    }

    // Backend-agnostic overload (no-op for now; D3D12 backend is currently disabled in builds)
    void D3D12CommandList::BindDescriptorSet(uint32_t set, DescriptorSetHandle /*descriptorSet*/, PipelineHandle /*pipeline*/) {
        // Intentionally left minimal to satisfy interface; implement proper mapping when D3D12 path is enabled
        std::cout << "D3D12CommandList: BindDescriptorSet(handle) set " << set << " (stub)" << std::endl;
        return;
    }




	    void D3D12CommandList::CopyTextureToBuffer(TextureHandle, BufferHandle, uint32_t, uint32_t, uint32_t, uint32_t, size_t, size_t) {
	        // TODO: Implement for D3D12; stub to satisfy interface
	    }


    void D3D12CommandList::CopyTextureSubresourceToBuffer(TextureHandle, uint32_t, uint32_t, BufferHandle, uint32_t, uint32_t, uint32_t, uint32_t, size_t, size_t, uint32_t, size_t) {
        // TODO: Implement for D3D12; stub to satisfy interface
    }

    void D3D12CommandList::ClearColorImageSubresource(TextureHandle, uint32_t, uint32_t, const float[4]) {
        // No-op stub for now on D3D12; tests target Vulkan
    }

    void D3D12CommandList::CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture,
                                                          uint32_t mip, uint32_t layer,
                                                          uint32_t width, uint32_t height,
                                                          size_t srcOffsetBytes, size_t srcRowPitchBytes,
                                                          uint32_t depth, size_t srcSlicePitchBytes,
                                                          uint32_t dstX, uint32_t dstY,
                                                          ResourceState currentState) {
        // currentState drives the Vulkan content-preserving barrier; on D3D12 subresource
        // state transitions are issued by the render graph / resource tracker, not here.
        (void)currentState;
        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: CopyBufferToTextureSubresource called while not recording" << std::endl;
            return;
        }

        ID3D12Resource* srcRes = m_Device->GetD3D12Buffer(srcBuffer);
        ID3D12Resource* dstRes = m_Device->GetD3D12Resource(dstTexture);
        if (!srcRes || !dstRes) {
            std::cerr << "D3D12CommandList: Invalid handles for CopyBufferToTextureSubresource" << std::endl;
            return;
        }

        const D3D12_RESOURCE_DESC dstDesc = dstRes->GetDesc();
        const bool is3DTexture = dstDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        const uint32_t copyDepth = is3DTexture ? (depth ? depth : 1u) : 1u;
        const size_t rowPitch = srcRowPitchBytes ? srcRowPitchBytes : static_cast<size_t>(width) * 4u;
        const size_t compactSlicePitch = rowPitch * static_cast<size_t>(height);
        const size_t slicePitch = srcSlicePitchBytes ? srcSlicePitchBytes : compactSlicePitch;

        if (width == 0 || height == 0 || copyDepth == 0) {
            std::cerr << "D3D12CommandList: CopyBufferToTextureSubresource called with empty extent" << std::endl;
            return;
        }
        if (!is3DTexture && depth > 1) {
            std::cerr << "D3D12CommandList: 2D texture uploads must have depth 1" << std::endl;
            return;
        }
        if ((srcOffsetBytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != 0) {
            std::cerr << "D3D12CommandList: Source offset must be aligned to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT" << std::endl;
            return;
        }
        if ((rowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0) {
            std::cerr << "D3D12CommandList: Source row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT" << std::endl;
            return;
        }
        // D3D12 PLACED_FOOTPRINT requires tightly-packed Z slices; callers with gapped
        // slice pitch must repack into a contiguous staging buffer (UploadTexturesBatched
        // handles this transparently — see TextureUploadHelpers.h).
        if (slicePitch != compactSlicePitch) {
            std::cerr << "D3D12CommandList: Source slice pitch must be tightly packed for D3D12 texture copies" << std::endl;
            return;
        }
        if (mip >= dstDesc.MipLevels) {
            std::cerr << "D3D12CommandList: Mip " << mip << " out of range (MipLevels=" << dstDesc.MipLevels << ")" << std::endl;
            return;
        }
        if (is3DTexture) {
            // dstDesc.DepthOrArraySize is UINT16; the MipLevels guard above bounds mip < 16,
            // and the cast widens to uint32_t before the shift — both keep this well-defined.
            const uint32_t mipDepth = std::max(1u, static_cast<uint32_t>(dstDesc.DepthOrArraySize) >> std::min(mip, 31u));
            if (layer + copyDepth > mipDepth) {
                std::cerr << "D3D12CommandList: 3D texture copy range is out of bounds" << std::endl;
                return;
            }
        } else if (layer >= dstDesc.DepthOrArraySize) {
            std::cerr << "D3D12CommandList: 2D texture array layer is out of bounds" << std::endl;
            return;
        }

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcRes;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = srcOffsetBytes;
        srcLoc.PlacedFootprint.Footprint.Format = dstDesc.Format;
        srcLoc.PlacedFootprint.Footprint.Width = width;
        srcLoc.PlacedFootprint.Footprint.Height = height;
        srcLoc.PlacedFootprint.Footprint.Depth = copyDepth;
        srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstRes;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = is3DTexture
            ? mip
            : D3D12CalcSubresource(mip, layer, 0, dstDesc.MipLevels, dstDesc.DepthOrArraySize);

        D3D12_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = width;
        srcBox.bottom = height;
        srcBox.back = copyDepth;

        m_CommandList->CopyTextureRegion(&dstLoc, dstX, dstY, is3DTexture ? layer : 0, &srcLoc, &srcBox);
    }


    void D3D12CommandList::CopyTextureToBuffer(
        TextureHandle srcTexture,
        BufferHandle dstBuffer,
        uint32_t width,
        uint32_t height,
        uint32_t SrcX,
        uint32_t SrcY,
        size_t DstOffsetBytes,
        size_t dstRowPitchBytes)
    {
        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: CopyTextureToBuffer called while not recording" << std::endl;
            return;
        }

        ID3D12Resource* srcRes = m_Device->GetD3D12Resource(srcTexture);
        ID3D12Resource* dstRes = m_Device->GetD3D12Buffer(dstBuffer);
        if (!srcRes || !dstRes) {
            std::cerr << "D3D12CommandList: Invalid handles for CopyTextureToBuffer" << std::endl;
            return;
        }

        // Transition resources to copy states
        ResourceBarrier texToCopy{};
        texToCopy.type = ResourceBarrier::Texture;
        texToCopy.textureHandle = srcTexture;
        texToCopy.stateBefore = ResourceState::RenderTarget;
        texToCopy.stateAfter = ResourceState::CopySource;
        Barrier(texToCopy);

        ResourceBarrier bufToCopy{};
        bufToCopy.type = ResourceBarrier::Buffer;
        bufToCopy.bufferHandle = dstBuffer;
        bufToCopy.stateBefore = ResourceState::Common;
        bufToCopy.stateAfter = ResourceState::CopyDest;
        Barrier(bufToCopy);

        D3D12_RESOURCE_DESC desc = srcRes->GetDesc();

        auto AlignUp = [](size_t v, size_t align) { return (v + align - 1) & ~(align - 1); };
        const size_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        const size_t kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

        auto BytesPerPixelDXGI = [](DXGI_FORMAT fmt) -> uint32_t {
            switch (fmt) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_R11G11B10_FLOAT:
                case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
                case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
                case DXGI_FORMAT_R32G32_FLOAT: return 8;
                case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
                case DXGI_FORMAT_R16_FLOAT: return 2;
                case DXGI_FORMAT_R16G16_FLOAT: return 4;
                case DXGI_FORMAT_R8_UNORM: return 1;
                case DXGI_FORMAT_R8G8_UNORM: return 2;
                default: return 0;
            }
        };

        uint32_t bpp = BytesPerPixelDXGI(desc.Format);
        size_t rowPitch = dstRowPitchBytes ? dstRowPitchBytes : static_cast<size_t>(width) * (bpp ? bpp : 4);
        rowPitch = AlignUp(rowPitch, kPitchAlign);
        size_t offset = AlignUp(DstOffsetBytes, kPlacementAlign);

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcRes;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstRes;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = offset;
        dstLoc.PlacedFootprint.Footprint.Format = desc.Format;
        dstLoc.PlacedFootprint.Footprint.Width = width;
        dstLoc.PlacedFootprint.Footprint.Height = height;
        dstLoc.PlacedFootprint.Footprint.Depth = 1;
        dstLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        D3D12_BOX srcBox{};
        srcBox.left = SrcX;
        srcBox.top = SrcY;
        srcBox.right = SrcX + width;
        srcBox.bottom = SrcY + height;
        srcBox.front = 0;
        srcBox.back = 1;

        m_CommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);

        ResourceBarrier bufToCommon{};
        bufToCommon.type = ResourceBarrier::Buffer;
        bufToCommon.bufferHandle = dstBuffer;
        bufToCommon.stateBefore = ResourceState::CopyDest;
        bufToCommon.stateAfter = ResourceState::Common;
        Barrier(bufToCommon);

        ResourceBarrier texBack{};
        texBack.type = ResourceBarrier::Texture;
        texBack.textureHandle = srcTexture;
        texBack.stateBefore = ResourceState::CopySource;
        texBack.stateAfter = ResourceState::RenderTarget;
        Barrier(texBack);
    }



    // Drawing
    void D3D12CommandList::Draw(uint32_t vertexCount, uint32_t instanceCount) {
        std::cout << "D3D12CommandList: Draw " << vertexCount << " vertices, " << instanceCount << " instances" << std::endl;

        if (m_IsRecording && m_CommandList) {
            m_CommandList->DrawInstanced(vertexCount, instanceCount, 0, 0);
        }
    }

    void D3D12CommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount) {
        std::cout << "D3D12CommandList: Draw indexed " << indexCount << " indices, " << instanceCount << " instances" << std::endl;

        if (m_IsRecording && m_CommandList) {
            m_CommandList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
        }
    }

    void D3D12CommandList::Draw(uint32_t vertexCount, uint32_t instanceCount,
                                 uint32_t firstVertex, uint32_t firstInstance) {
        std::cout << "D3D12CommandList: Draw subrange vc=" << vertexCount
                  << " ic=" << instanceCount
                  << " firstVertex=" << firstVertex
                  << " firstInstance=" << firstInstance << std::endl;
        if (m_IsRecording && m_CommandList) {
            m_CommandList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        }
    }

    void D3D12CommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                       uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        std::cout << "D3D12CommandList: DrawIndexed subrange ic=" << indexCount
                  << " inst=" << instanceCount
                  << " firstIndex=" << firstIndex
                  << " vertexOffset=" << vertexOffset
                  << " firstInstance=" << firstInstance << std::endl;
        if (m_IsRecording && m_CommandList) {
            m_CommandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, static_cast<INT>(vertexOffset), firstInstance);
        }
    }


    void D3D12CommandList::DrawMeshTasks(uint32_t taskCount) {
        std::cout << "D3D12CommandList: Draw mesh tasks " << taskCount << std::endl;

        if (m_IsRecording && m_CommandList) {
            // Check if mesh shaders are supported (requires D3D12_FEATURE_LEVEL_12_1+)
            // For now, fall back to regular draw call
            std::cout << "D3D12CommandList: DrawMeshTasks fallback to regular draw" << std::endl;

            // In a real implementation with mesh shader support, this would use:
            // m_CommandList->DispatchMesh(taskCount, 0, 0);

            // For now, simulate with a regular draw call
            m_CommandList->DrawInstanced(taskCount * 3, 1, 0, 0); // Assume 3 vertices per task
            std::cout << "✅ D3D12CommandList: Mesh tasks simulated with regular draw" << std::endl;
        } else {
            std::cout << "D3D12CommandList: DrawMeshTasks (NOT RECORDING)" << std::endl;
        }
    }

    void D3D12CommandList::DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) {
        std::cout << "D3D12CommandList: DrawIndirect " << drawCount << " draws from buffer " << commandBuffer << std::endl;

        if (m_IsRecording && m_CommandList) {
            // In a real implementation, this would use ID3D12GraphicsCommandList::ExecuteIndirect
            // For now, just log the operation
            std::cout << "✅ D3D12CommandList: DrawIndirect prepared (not implemented)" << std::endl;
        } else {
            std::cout << "D3D12CommandList: DrawIndirect (NOT RECORDING)" << std::endl;
        }
    }

    void D3D12CommandList::DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) {
        std::cout << "D3D12CommandList: DrawIndexedIndirect " << drawCount << " draws from buffer " << commandBuffer << std::endl;

        if (m_IsRecording && m_CommandList) {
            // In a real implementation, this would use ID3D12GraphicsCommandList::ExecuteIndirect
            // For now, just log the operation
            std::cout << "✅ D3D12CommandList: DrawIndexedIndirect prepared (not implemented)" << std::endl;
        } else {
            std::cout << "D3D12CommandList: DrawIndexedIndirect (NOT RECORDING)" << std::endl;
        }
    }

    void D3D12CommandList::DrawIndexedIndirectCount(BufferHandle commandBuffer,
                                                    BufferHandle countBuffer,
                                                    uint32_t     maxDrawCount,
                                                    uint32_t     stride,
                                                    size_t       commandBufferOffset,
                                                    size_t       countBufferOffset) {
        (void)commandBuffer; (void)countBuffer; (void)maxDrawCount; (void)stride;
        (void)commandBufferOffset; (void)countBufferOffset;
        // D3D12 ExecuteIndirect accepts a count buffer; stub for now since
        // production runs on Vulkan. Mirrors DrawIndexedIndirect's status.
        std::cout << "D3D12CommandList: DrawIndexedIndirectCount (not implemented)" << std::endl;
    }

    void D3D12CommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        std::cout << "D3D12CommandList: Dispatch " << x << "x" << y << "x" << z << " groups" << std::endl;

        if (m_IsRecording && m_CommandList) {
            m_CommandList->Dispatch(x, y, z);
        }
    }

    void D3D12CommandList::DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes) {
        (void)argsBuffer; (void)argsOffsetBytes;
        // Real support needs a net-new ID3D12CommandSignature (D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)
        // + ExecuteIndirect, same plumbing the indirect-draw siblings still lack.
        // Out of scope while production runs on Vulkan; mirrors DrawIndirect's status.
        std::cout << "D3D12CommandList: DispatchIndirect (not implemented)" << std::endl;
    }

    // Render passes
    void D3D12CommandList::BeginRenderPass(const RenderPassDesc& desc) {
        std::cout << "D3D12CommandList: Begin render pass" << std::endl;

        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: Cannot begin render pass - command list not recording!" << std::endl;
            return;
        }

        // Get current swapchain RTV from device
        D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);
        uint32_t currentImageIndex = 0;
        if (!d3d12Device->AcquireNextImage(currentImageIndex)) {
            std::cerr << "D3D12CommandList: Failed to acquire swapchain image!" << std::endl;
            return;
        }

        // Get the RTV handle for current swapchain image
        auto swapchainRTVs = d3d12Device->GetSwapchainRTVs();
        if (currentImageIndex >= swapchainRTVs.size()) {
            std::cerr << "D3D12CommandList: Invalid swapchain image index!" << std::endl;
            return;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = swapchainRTVs[currentImageIndex];

        // Transition swapchain image to render target state
        auto swapchainBuffers = d3d12Device->GetSwapchainBuffers();
        if (currentImageIndex < swapchainBuffers.size()) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = swapchainBuffers[currentImageIndex].Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            m_CommandList->ResourceBarrier(1, &barrier);
        }

        // Set render target
        m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // Clear render target to background color
        float clearColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; // Bright red background for testing
        m_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        std::cout << "✅ D3D12CommandList: Render pass begun with RTV set and cleared" << std::endl;
    }

    void D3D12CommandList::EndRenderPass() {
        std::cout << "D3D12CommandList: End render pass" << std::endl;

        if (!m_IsRecording || !m_CommandList) {
            std::cerr << "D3D12CommandList: Cannot end render pass - command list not recording!" << std::endl;
            return;
        }

        // Get current swapchain image and transition back to present state
        D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);
        uint32_t currentImageIndex = 0;
        if (d3d12Device->AcquireNextImage(currentImageIndex)) {
            auto swapchainBuffers = d3d12Device->GetSwapchainBuffers();
            if (currentImageIndex < swapchainBuffers.size()) {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = swapchainBuffers[currentImageIndex].Get();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                m_CommandList->ResourceBarrier(1, &barrier);
            }
        }

        std::cout << "✅ D3D12CommandList: Render pass ended with swapchain transitioned to present" << std::endl;
    }

    // Viewport and scissor
    void D3D12CommandList::SetViewport(float x, float y, float width, float height) {
        std::cout << "D3D12CommandList: Set viewport " << width << "x" << height << std::endl;

        if (m_IsRecording && m_CommandList) {
            D3D12_VIEWPORT viewport = {};
            viewport.TopLeftX = x;
            viewport.TopLeftY = y;
            viewport.Width = width;
            viewport.Height = height;
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;

            m_CommandList->RSSetViewports(1, &viewport);
        }
    }

    void D3D12CommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        std::cout << "D3D12CommandList: Set scissor " << width << "x" << height << std::endl;

        if (m_IsRecording && m_CommandList) {
            D3D12_RECT scissorRect = {};
            scissorRect.left = x;
            scissorRect.top = y;
            scissorRect.right = x + width;
            scissorRect.bottom = y + height;

            m_CommandList->RSSetScissorRects(1, &scissorRect);
        }
    }

    // Resource operations
    void D3D12CommandList::CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset, size_t dstOffset) {
        if (m_CommandList && m_IsRecording) {
            // Get D3D12 resources from handles
            auto* srcResource = m_Device->GetD3D12Buffer(src);
            auto* dstResource = m_Device->GetD3D12Buffer(dst);

            if (srcResource && dstResource) {
                m_CommandList->CopyBufferRegion(dstResource, dstOffset, srcResource, srcOffset, size);
                std::cout << "D3D12CommandList: REAL Buffer copy executed " << src << " to " << dst << " size " << size << std::endl;
            } else {
                std::cout << "D3D12CommandList: Copy buffer failed - invalid buffer handles" << std::endl;
            }
        } else {
            std::cout << "D3D12CommandList: Copy buffer " << src << " to " << dst << " size " << size << " (NOT RECORDING)" << std::endl;
        }
    }

    void D3D12CommandList::FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value) {
        // D3D12 has no direct equivalent; would require a small CopyBufferRegion
        // from a staging upload buffer or a compute shader. Not implemented yet —
        // SDSM is currently a Vulkan-only feature path.
        (void)dst; (void)offset; (void)size; (void)value;
    }

    void D3D12CommandList::CopyTexture(TextureHandle src, TextureHandle dst) {
        std::cout << "D3D12CommandList: Copy texture " << src << " to " << dst << std::endl;

        if (m_CommandList && m_IsRecording) {
            // Get D3D12 resources from handles
            auto* srcResource = m_Device->GetD3D12Resource(src);
            auto* dstResource = m_Device->GetD3D12Resource(dst);

            if (srcResource && dstResource) {
                m_CommandList->CopyResource(dstResource, srcResource);
                std::cout << "D3D12CommandList: Real texture copy executed" << std::endl;
            } else {
                std::cout << "D3D12CommandList: Invalid texture handles for copy" << std::endl;
            }
        }
    }

    // Synchronization
    void D3D12CommandList::Barrier(const ResourceBarrier& barrier) {
        std::cout << "D3D12CommandList: Resource barrier" << std::endl;

        if (m_CommandList && m_IsRecording) {
            D3D12_RESOURCE_BARRIER d3d12Barrier = {};
            d3d12Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            d3d12Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

            // Convert barrier to D3D12 format based on resource type
            ID3D12Resource* resource = nullptr;
            if (barrier.type == ResourceBarrier::Texture) {
                resource = m_Device->GetD3D12Resource(barrier.textureHandle);

                if (!resource) {
                    D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);
                    uint32_t currentImageIndex = 0;
                    if (d3d12Device->AcquireNextImage(currentImageIndex)) {
                        auto swapchainBuffers = d3d12Device->GetSwapchainBuffers();
                        if (currentImageIndex < swapchainBuffers.size()) {
                            resource = swapchainBuffers[currentImageIndex].Get();
                            std::cout << "✅ D3D12CommandList: Using swapchain buffer " << currentImageIndex << " for barrier" << std::endl;
                        }
                    }
                }
            } else if (barrier.type == ResourceBarrier::Buffer) {
                resource = m_Device->GetD3D12Buffer(barrier.bufferHandle);
            }

            if (resource) {
                d3d12Barrier.Transition.pResource = resource;
                d3d12Barrier.Transition.StateBefore = ToD3D12ResourceState(barrier.stateBefore);
                d3d12Barrier.Transition.StateAfter = ToD3D12ResourceState(barrier.stateAfter);
                d3d12Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                m_CommandList->ResourceBarrier(1, &d3d12Barrier);
                std::cout << "✅ D3D12CommandList: Resource barrier executed (state " << static_cast<int>(barrier.stateBefore) << " -> " << static_cast<int>(barrier.stateAfter) << ")" << std::endl;
            } else {
                std::cerr << "⚠️  D3D12CommandList: Resource barrier skipped - resource not found" << std::endl;
            }
        }
    }

    // Debug events
    void D3D12CommandList::BeginEvent(const char* name) {
        std::cout << "D3D12CommandList: Begin event '" << name << "'" << std::endl;

        if (m_CommandList && m_IsRecording) {
            // PIX events for debugging
            #ifdef _WIN32
            // PIXBeginEvent would be used here in a full implementation
            // For now, just log the event
            #endif
        }
    }

    void D3D12CommandList::EndEvent() {
        std::cout << "D3D12CommandList: End event" << std::endl;

        if (m_CommandList && m_IsRecording) {
            // PIX events for debugging
            #ifdef _WIN32
            // PIXEndEvent would be used here in a full implementation
            #endif
        }
    }

    void D3D12CommandList::SetMarker(const char* name) {
        std::cout << "D3D12CommandList: Marker '" << name << "'" << std::endl;

        if (m_CommandList && m_IsRecording) {
            // PIX markers for debugging
            #ifdef _WIN32
            // PIXSetMarker would be used here in a full implementation
            #endif
        }
    }

    void D3D12CommandList::ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) {
        if (m_IsRecording && m_CommandList) {
            // Get the RTV descriptor handle for the target
            D3D12Device* d3d12Device = static_cast<D3D12Device*>(m_Device);

            // For swapchain images, get the current RTV
            uint32_t currentImageIndex = 0;
            if (d3d12Device->AcquireNextImage(currentImageIndex)) {
                auto rtvHandles = d3d12Device->GetSwapchainRTVs();
                if (currentImageIndex < rtvHandles.size()) {
                    float clearColor[4] = { r, g, b, a };
                    m_CommandList->ClearRenderTargetView(rtvHandles[currentImageIndex], clearColor, 0, nullptr);
                    std::cout << "✅ D3D12CommandList: Cleared render target " << target << " to (" << r << ", " << g << ", " << b << ", " << a << ")" << std::endl;
                } else {
                    std::cerr << "D3D12CommandList: Invalid swapchain image index " << currentImageIndex << std::endl;
                }
            } else {
                std::cerr << "D3D12CommandList: Failed to acquire swapchain image for clear" << std::endl;
            }
        }
    }


    void D3D12CommandList::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) {
        outWidth = outHeight = 0;
        auto* res = m_Device->GetD3D12Resource(texture);
        if (!res) return;
        D3D12_RESOURCE_DESC desc = res->GetDesc();
        outWidth = static_cast<uint32_t>(desc.Width);
        outHeight = static_cast<uint32_t>(desc.Height);
    }

} // namespace Rendering
} // namespace GameEngine
