/**
 * @file D3D12Resources.cpp
 * @brief DirectX 12 resource utilities and helpers
 */

#include "D3D12Device.h"
#include "Rendering/Core/Device.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>

namespace GameEngine::Rendering {

    // DirectX 12 resource utilities
    namespace D3D12ResourceUtils {

        DXGI_FORMAT GetD3D12Format(Format format) {
            switch (format) {
                case Format::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
                case Format::R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case Format::B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
                case Format::B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                case Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
                case Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
                case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
                default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_RESOURCE_FLAGS GetD3D12ResourceFlags(TextureUsage usage) {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

            if (static_cast<uint32_t>(usage & TextureUsage::RenderTarget)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (static_cast<uint32_t>(usage & TextureUsage::DepthStencil)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            }
            if (static_cast<uint32_t>(usage & TextureUsage::UnorderedAccess)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }

            return flags;
        }

        D3D12_HEAP_TYPE GetD3D12HeapType(MemoryType memoryType) {
            switch (memoryType) {
                case MemoryType::DeviceLocal:
                    return D3D12_HEAP_TYPE_DEFAULT;
                case MemoryType::HostVisible:
                    return D3D12_HEAP_TYPE_UPLOAD;
                case MemoryType::HostCached:
                    return D3D12_HEAP_TYPE_READBACK;
                default:
                    return D3D12_HEAP_TYPE_DEFAULT;
            }
        }

        D3D12_RESOURCE_STATES GetD3D12ResourceState(ResourceState state) {
            switch (state) {
                case ResourceState::Common:
                    return D3D12_RESOURCE_STATE_COMMON;
                case ResourceState::VertexBuffer:
                    return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                case ResourceState::IndexBuffer:
                    return D3D12_RESOURCE_STATE_INDEX_BUFFER;
                case ResourceState::ConstantBuffer:
                    return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                case ResourceState::ShaderResource:
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

        void PrintAdapterInfo(IDXGIAdapter1* adapter) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                std::wcout << L"\n=== DirectX 12 Adapter Info ===" << std::endl;
                std::wcout << L"Description: " << desc.Description << std::endl;
                std::wcout << L"Dedicated Video Memory: " << (desc.DedicatedVideoMemory / (1024 * 1024)) << L" MB" << std::endl;
                std::wcout << L"Dedicated System Memory: " << (desc.DedicatedSystemMemory / (1024 * 1024)) << L" MB" << std::endl;
                std::wcout << L"Shared System Memory: " << (desc.SharedSystemMemory / (1024 * 1024)) << L" MB" << std::endl;
                std::wcout << L"===============================" << std::endl;
            }
        }

    } // namespace D3D12ResourceUtils

} // namespace GameEngine::Rendering
