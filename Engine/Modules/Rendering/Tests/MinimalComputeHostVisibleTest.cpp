#include <iostream>
#include <vector>
#include <memory>
#include <cassert>
#include <fstream>


#include <Rendering/Core/Device.h>
#include <Rendering/Core/CommandList.h>
#include <Rendering/Common/Utils.h>
#include "TestUtils.h"


using namespace GameEngine::Rendering;

class MinimalComputeHostVisibleTest {
public:
    bool Initialize() {
        DeviceDesc deviceDesc{};
        deviceDesc.applicationName = "MinimalComputeHostVisibleTest";
        deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
        deviceDesc.enableDebugLayer = true;

        m_Device = DeviceFactory::CreateDevice(deviceDesc);
        if (!m_Device || !m_Device->Initialize(deviceDesc)) {
            std::cerr << "Failed to initialize device" << std::endl;
            return false;
        }
        return true;
    }

    bool RunTest() {
        // Create a host-visible storage buffer directly written by compute
        const uint32_t elementCount = 10;
        const size_t bufferSize = elementCount * sizeof(uint32_t);

        BufferDesc hostStorage{};
        hostStorage.size = bufferSize;
        hostStorage.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
        hostStorage.memoryUsage = BufferMemoryUsage::Readback; // CPU-visible
        hostStorage.flags = BufferCreateFlags::PersistentlyMapped;
        hostStorage.debugName = "HostVisibleStorage";

        BufferHandle storageBuffer = m_Device->CreateBuffer(hostStorage);
        if (!storageBuffer.IsValid()) {
            std::cerr << "Failed to create host-visible storage buffer" << std::endl;
            return false;
        }

        // Zero initialize
        std::vector<uint32_t> zeros(elementCount, 0);
        m_Device->UpdateBuffer(storageBuffer, 0, bufferSize, zeros.data());

        // Load compute shader via test helper
        std::vector<uint8_t> shaderCode = GameEngine::Rendering::Tests::ReadSpirvBytes("minimal_test.comp.spv");
        if (shaderCode.empty()) {
            std::cerr << "Failed to load shader" << std::endl;
            return false;
        }

        // Pipeline with storage buffer at set=0,binding=0
        PipelineDesc pipelineDesc{};
        pipelineDesc.type = PipelineType::Compute;
        pipelineDesc.computeShader = shaderCode;
        DescriptorSetLayoutDesc setLayout{};
        DescriptorBinding bind{};
        bind.binding = 0;
        bind.type = DescriptorType::StorageBuffer;
        bind.count = 1;
        bind.shaderStages = kShaderStageCompute; // compute stage
        setLayout.bindings.push_back(bind);
        pipelineDesc.descriptorSetLayouts.push_back(setLayout);

        PipelineHandle pipeline = m_Device->CreatePipeline(pipelineDesc);
        if (pipeline == 0) {
            std::cerr << "Failed to create compute pipeline" << std::endl;
            return false;
        }

        // Create a descriptor set via public API
        DescriptorSetDesc ds{};
        ds.layout = setLayout;
        ds.debugName = "HostVisibleDS";
        DescriptorSetHandle dsh = m_Device->CreateDescriptorSet(ds);
        if (!dsh.IsValid()) {
            std::cerr << "Failed to create descriptor set" << std::endl;
            return false;
        }

        DescriptorSetUpdate upd{};
        upd.binding = 0;
        upd.type = DescriptorType::StorageBuffer;
        upd.buffers = { storageBuffer };
        upd.bufferOffsets = { 0 };
        upd.bufferRanges = { bufferSize };
        m_Device->UpdateDescriptorSet(dsh, upd);

        // Build command list
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(pipeline);

        // Bind descriptor set via typed API
        cl->BindDescriptorSet(0, dsh, pipeline);

        cl->Dispatch(1,1,1);
        cl->End();

        std::vector<CommandList*> cls = { cl.get() };
        m_Device->ExecuteCommandLists(cls);
        m_Device->WaitForIdle();

        // Validate by mapping the same buffer
        void* mapped = m_Device->MapBuffer(storageBuffer);
        if (!mapped) {
            std::cerr << "Failed to map host-visible storage buffer" << std::endl;
            return false;
        }
        uint32_t* data = static_cast<uint32_t*>(mapped);
        bool ok = true;
        for (uint32_t i = 0; i < elementCount; ++i) {
            uint32_t expected = i + 42;
            if (data[i] != expected) {
                std::cerr << "Mismatch at " << i << ": got " << data[i] << ", expected " << expected << std::endl;
                ok = false;
            }
        }
        m_Device->UnmapBuffer(storageBuffer);

        m_Device->DestroyBuffer(storageBuffer);
        m_Device->DestroyPipeline(pipeline);
        return ok;
    }

    void Shutdown() { if (m_Device) m_Device->Shutdown(); }

private:
    bool LoadShaderFile(const std::string& filename, std::vector<uint8_t>& data) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        size_t fileSize = static_cast<size_t>(file.tellg());
        data.resize(fileSize);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        file.close();
        return true;
    }

private:
    std::unique_ptr<IDevice> m_Device;
};

int main() {
    MinimalComputeHostVisibleTest test;
    if (!test.Initialize()) return -1;
    bool ok = false;
    try { ok = test.RunTest(); }
    catch (const std::exception& e) { std::cerr << e.what() << std::endl; }
    test.Shutdown();
    std::cout << (ok ? "SUCCESS" : "FAIL") << std::endl;
    return ok ? 0 : -1;
}

