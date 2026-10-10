/**
 * @file D3D12Swapchain.cpp
 * @brief DirectX 12 swapchain utilities and helpers
 */

#include "D3D12Device.h"
#include "Rendering/Core/Device.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>

namespace GameEngine::Rendering {

    // DirectX 12 swapchain utilities
    namespace D3D12SwapchainUtils {

        DXGI_SWAP_CHAIN_DESC1 CreateSwapchainDesc(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t bufferCount) {
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.Format = format;
            desc.Stereo = FALSE;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = bufferCount;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

            return desc;
        }

        HRESULT CreateSwapchainForHwnd(
            IDXGIFactory4* factory,
            ID3D12CommandQueue* commandQueue,
            HWND hwnd,
            const DXGI_SWAP_CHAIN_DESC1& desc,
            IDXGISwapChain1** swapchain) {

            return factory->CreateSwapChainForHwnd(
                commandQueue,
                hwnd,
                &desc,
                nullptr,
                nullptr,
                swapchain
            );
        }

        HRESULT GetSwapchainBuffer(IDXGISwapChain* swapchain, uint32_t bufferIndex, ID3D12Resource** resource) {
            return swapchain->GetBuffer(bufferIndex, IID_PPV_ARGS(resource));
        }

        void CreateRenderTargetViews(
            ID3D12Device* device,
            IDXGISwapChain* swapchain,
            ID3D12DescriptorHeap* rtvHeap,
            uint32_t bufferCount,
            std::vector<ID3D12Resource*>& backBuffers) {

            SIZE_T rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();

            backBuffers.resize(bufferCount);

            for (uint32_t i = 0; i < bufferCount; i++) {
                HRESULT hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i]));
                if (SUCCEEDED(hr)) {
                    device->CreateRenderTargetView(backBuffers[i], nullptr, rtvHandle);
                    rtvHandle.ptr += rtvDescriptorSize;
                }
            }
        }

        void TransitionResource(
            ID3D12GraphicsCommandList* commandList,
            ID3D12Resource* resource,
            D3D12_RESOURCE_STATES stateBefore,
            D3D12_RESOURCE_STATES stateAfter) {

            if (stateBefore == stateAfter) return;

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = stateBefore;
            barrier.Transition.StateAfter = stateAfter;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            commandList->ResourceBarrier(1, &barrier);
        }

        void PrintSwapchainInfo(IDXGISwapChain* swapchain) {
            DXGI_SWAP_CHAIN_DESC desc;
            if (SUCCEEDED(swapchain->GetDesc(&desc))) {
                std::cout << "\n=== DirectX 12 Swapchain Info ===" << std::endl;
                std::cout << "Buffer Count: " << desc.BufferCount << std::endl;
                std::cout << "Resolution: " << desc.BufferDesc.Width << "x" << desc.BufferDesc.Height << std::endl;
                std::cout << "Format: " << desc.BufferDesc.Format << std::endl;
                std::cout << "Refresh Rate: " << desc.BufferDesc.RefreshRate.Numerator << "/" << desc.BufferDesc.RefreshRate.Denominator << std::endl;
                std::cout << "Sample Count: " << desc.SampleDesc.Count << std::endl;
                std::cout << "Windowed: " << (desc.Windowed ? "Yes" : "No") << std::endl;
                std::cout << "==================================" << std::endl;
            }
        }

    } // namespace D3D12SwapchainUtils

} // namespace GameEngine::Rendering
