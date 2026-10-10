/**
 * @file D3D12Memory.cpp
 * @brief DirectX 12 memory management utilities
 */

#include "D3D12Device.h"
#include "Rendering/Core/Device.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>

namespace GameEngine::Rendering {

    // DirectX 12 memory utilities
    namespace D3D12MemoryUtils {

        SIZE_T GetAlignedSize(SIZE_T size, SIZE_T alignment) {
            return (size + alignment - 1) & ~(alignment - 1);
        }

        D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc) {
            return device->GetResourceAllocationInfo(0, 1, &desc);
        }

        HRESULT CreateCommittedResource(
            ID3D12Device* device,
            const D3D12_HEAP_PROPERTIES& heapProps,
            D3D12_HEAP_FLAGS heapFlags,
            const D3D12_RESOURCE_DESC& desc,
            D3D12_RESOURCE_STATES initialState,
            const D3D12_CLEAR_VALUE* clearValue,
            ID3D12Resource** resource) {

            return device->CreateCommittedResource(
                &heapProps,
                heapFlags,
                &desc,
                initialState,
                clearValue,
                IID_PPV_ARGS(resource)
            );
        }

        D3D12_HEAP_PROPERTIES GetDefaultHeapProperties() {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;
            return heapProps;
        }

        D3D12_HEAP_PROPERTIES GetUploadHeapProperties() {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;
            return heapProps;
        }

        D3D12_HEAP_PROPERTIES GetReadbackHeapProperties() {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;
            return heapProps;
        }

        void PrintMemoryInfo(ID3D12Device* device) {
            DXGI_QUERY_VIDEO_MEMORY_INFO localMemoryInfo = {};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMemoryInfo = {};

            // Get DXGI adapter from device
            LUID adapterLuid = device->GetAdapterLuid();

            // Note: This is a simplified version. In a full implementation,
            // you would need to get the adapter and query memory info properly.
            std::cout << "\n=== DirectX 12 Memory Info ===" << std::endl;
            std::cout << "Memory information requires DXGI adapter access" << std::endl;
            std::cout << "Adapter LUID: " << adapterLuid.HighPart << ":" << adapterLuid.LowPart << std::endl;
            std::cout << "===============================" << std::endl;
        }

        bool IsUMAArchitecture(ID3D12Device* device) {
            D3D12_FEATURE_DATA_ARCHITECTURE archData = {};
            HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &archData, sizeof(archData));

            if (SUCCEEDED(hr)) {
                return archData.UMA;
            }

            return false; // Assume discrete GPU if check fails
        }

    } // namespace D3D12MemoryUtils

} // namespace GameEngine::Rendering
