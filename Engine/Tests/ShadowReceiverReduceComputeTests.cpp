// GPU-executed tests for the SDSM receiver reduce (depth_reduce.comp over
// Includes/shadow_receiver_reduce.glsl): a depth image of known geometry, seen
// through a pitched perspective or orthographic camera, goes through the real
// shader with the uniform block the ShadowMap node builds
// (ShadowReceiverReduce::MakeSetup), and the decoded result must match the
// receivers a CPU ray cast finds at the sampled pixels: the nearest receiver's
// view depth, per view-depth bin the light-space box of the receivers in it
// and of their rays, and the rays that reach past every bin (sky, or ground
// past the shadow distance). This pins the whole measuring chain the cascade
// fit trusts: pixel -> NDC (Y up), the inverse view-projection, the view depth
// the shader selects cascades by, the bin layout, the light basis the CPU fits
// in, and the order-preserving encoding of negative coordinates. It runs on the
// desktop kernel and on its GE_COMPAT_PROFILE arm, the one the web cook
// translates to WGSL, whose depth flush differs.

#include "Engine/Rendering/Pipeline/Nodes/ShadowReceiverReduce.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ShadowReceiverMeasurement.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <ostream>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::Engine::Renderer::DecodeShadowReceiverResult;
using GameEngine::Engine::Renderer::ShadowMapRenderFeature;
using GameEngine::Engine::Renderer::ShadowReceiverMeasurement;
using GameEngine::Engine::Renderer::ShadowReceiverReadback;
using GameEngine::Engine::Renderer::Pipeline::Nodes::ShadowReceiverReduce;
using Mathematics::AABB;
using Mathematics::Matrix4x4;
using Mathematics::Vector3;
using Mathematics::Vector4;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kWidth = 96;
constexpr uint32_t kHeight = 72;
constexpr float kNear = 0.5f;
constexpr float kFar = 1000.0f;
constexpr float kMaxShadowDistance = 80.0f;
// The shader samples one pixel in kStride x kStride.
constexpr uint32_t kStride = 2;
// A tower on flat ground: x in [-3, 3], z in [8, 14], y in [0, 12].
const AABB kTower{{-3.0f, 0.0f, 8.0f}, {3.0f, 12.0f, 14.0f}};

struct PinholeCamera
{
    CameraData Data{};
    Matrix4x4 ViewProj;
    Vector3 Position;
    Vector3 Forward;
    Vector3 Right;
    Vector3 Up;
    // The window's half height: per unit view depth under a perspective
    // projection, in world units under an orthographic one.
    float HalfY = 0.0f;
    float Aspect = 1.0f;
    bool Orthographic = false;
    float Far = kFar;
};

// A camera pitched down by `pitchDownDeg`: perspective with a 60 degree
// vertical field of view when `orthographicHalfHeight` is 0, else orthographic
// with that half height.
PinholeCamera MakeCamera(const Vector3& position, float pitchDownDeg, float orthographicHalfHeight,
                         float farPlane)
{
    PinholeCamera cam{};
    const float pitch = pitchDownDeg * kPi / 180.0f;
    cam.Position = position;
    cam.Forward = Vector3{0.0f, -std::sin(pitch), std::cos(pitch)};
    const float fovY = 60.0f * kPi / 180.0f;
    cam.Orthographic = orthographicHalfHeight > 0.0f;
    cam.HalfY = cam.Orthographic ? orthographicHalfHeight : std::tan(fovY * 0.5f);
    cam.Aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    cam.Far = farPlane;
    const Matrix4x4 view =
        Mathematics::MakeLookAtLH(position, position + cam.Forward, Vector3{0.0f, 1.0f, 0.0f});
    const float halfX = cam.HalfY * cam.Aspect;
    const Matrix4x4 proj =
        cam.Orthographic
            ? Mathematics::MakeOrthographicLH_ZO_ReverseZ(-halfX, halfX, -cam.HalfY, cam.HalfY, kNear, farPlane)
            : Mathematics::MakePerspectiveLH_ZO_ReverseZ(fovY, cam.Aspect, kNear, farPlane);
    cam.ViewProj = proj * view;
    std::memcpy(cam.Data.view, view.Data(), sizeof(cam.Data.view));
    std::memcpy(cam.Data.proj, proj.Data(), sizeof(cam.Data.proj));
    std::memcpy(cam.Data.viewProj, cam.ViewProj.Data(), sizeof(cam.Data.viewProj));
    cam.Data.cameraPos[0] = position.x;
    cam.Data.cameraPos[1] = position.y;
    cam.Data.cameraPos[2] = position.z;
    cam.Right = Vector3::Cross(Vector3{0.0f, 1.0f, 0.0f}, cam.Forward).Normalize();
    cam.Up = Vector3::Cross(cam.Forward, cam.Right);
    return cam;
}

// The offset of the centre of pixel (x, y), row 0 at the top of the view, on
// the window: per unit view depth under a perspective camera, from the eye
// under an orthographic one.
Vector3 PixelOffset(const PinholeCamera& cam, uint32_t x, uint32_t y)
{
    const float ndcX = (static_cast<float>(x) + 0.5f) / static_cast<float>(kWidth) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(kHeight) * 2.0f;
    return cam.Right * (ndcX * cam.HalfY * cam.Aspect) + cam.Up * (ndcY * cam.HalfY);
}

// Nearest hit along the ray through the centre of pixel (x, y) within the far
// plane: the ground plane y = 0 or the tower, whichever comes first.
std::optional<Vector3> CastPixel(const PinholeCamera& cam, uint32_t x, uint32_t y)
{
    const Vector3 offset = PixelOffset(cam, x, y);
    Mathematics::Ray3D ray;
    ray.origin = cam.Orthographic ? cam.Position + offset : cam.Position;
    ray.direction = cam.Orthographic ? cam.Forward : (cam.Forward + offset).Normalize();
    float best = std::numeric_limits<float>::max();
    if (ray.direction.y < 0.0f)
        best = -ray.origin.y / ray.direction.y;
    float tMin = 0.0f;
    float tMax = 0.0f;
    if (Mathematics::IntersectRayAABB(ray, kTower, tMin, tMax))
        best = std::min(best, tMin);
    if (best == std::numeric_limits<float>::max())
        return std::nullopt;
    const Vector3 hit = ray.origin + ray.direction * best;
    if (Vector3::Dot(hit - cam.Position, cam.Forward) > cam.Far)
        return std::nullopt;
    return hit;
}

float ReverseZDepth(const PinholeCamera& cam, const Vector3& p)
{
    const Vector4 clip = cam.ViewProj.Transform(Vector4{p.x, p.y, p.z, 1.0f});
    return clip.z / clip.w;
}

// A build of the reduce: the package every desktop profile loads, or a bare
// SPIR-V blob staged under <build>/Shaders.
struct Kernel
{
    const char* Name;
    const char* Package;
    const char* Blob;
};

constexpr Kernel kDesktopKernel{"Desktop", "Shaders/depth_reduce.shaderpkg", nullptr};
// Its GE_COMPAT_PROFILE arm built for Vulkan, which no desktop profile executes.
constexpr Kernel kCompatKernel{"Compat", nullptr, "Shaders/Tests/depth_reduce_compat.comp.spv"};

void PrintTo(const Kernel& kernel, std::ostream* os) { *os << kernel.Name; }

std::string KernelName(const ::testing::TestParamInfo<Kernel>& info) { return info.param.Name; }

std::vector<uint8_t> LoadKernel(const Kernel& kernel)
{
    if (kernel.Blob)
        return Utils::ReadFile(kernel.Blob);
    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg(kernel.Package, ShaderSourceKind::SpirV, pkg, &err))
        return {};
    const auto itCs = pkg.stageBytes.find("cs");
    return itCs == pkg.stageBytes.end() ? std::vector<uint8_t>{} : itCs->second;
}

class ShadowReceiverReduceComputeTest : public ::testing::TestWithParam<Kernel>
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        std::vector<uint8_t> shader = LoadKernel(GetParam());
        ASSERT_FALSE(shader.empty()) << GetParam().Name << " kernel is not staged under <build>/Shaders";

        const DescriptorType types[3] = {DescriptorType::CombinedImageSampler,
                                         DescriptorType::StorageBuffer, DescriptorType::UniformBuffer};
        for (uint32_t binding = 0; binding < 3; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = types[binding];
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shader));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        desc.DebugName = "ShadowReceiverReduceTest";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(m_Pipeline.IsValid());
        m_Sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("ShadowReceiverReduceTest"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto set : m_Sets)
            m_Device->DestroyDescriptorSet(set);
        for (auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
    }

    BufferHandle Buffer(uint64_t size, BufferUsage usage, BufferMemoryUsage memory)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = memory;
        desc.debugName = "ShadowReceiverReduceTest.Buffer";
        const BufferHandle buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    // Runs the reduce over `depth` (kWidth x kHeight, row 0 at the top) and
    // returns the raw result sides.
    ShadowReceiverReadback Reduce(const std::vector<float>& depth,
                                  const ShadowReceiverReduce::Setup& setup)
    {
        ShadowReceiverReadback readback{};
        readback.Measured = setup.Measured;
        TextureDesc td{};
        td.width = kWidth;
        td.height = kHeight;
        td.depth = td.mipLevels = td.sampleCount = td.arrayLayers = 1;
        // The shader reads depth as a sampled float, so the fixture carries it in a
        // colour format: a buffer copy into a D32_FLOAT image writes nothing on
        // Vulkan (the copy's aspect is colour), which leaves every sample as sky.
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        td.debugName = "ShadowReceiverReduceTest.Depth";
        const TextureHandle texture = m_Device->CreateTexture(td);
        m_Textures.push_back(texture);
        EXPECT_TRUE(texture.IsValid());

        const BufferHandle upload =
            Buffer(depth.size() * sizeof(float), BufferUsage::TransferSrc, BufferMemoryUsage::Upload);
        const BufferHandle params = Buffer(sizeof(ShadowReceiverReduce::ParamsGPU), BufferUsage::Uniform,
                                           BufferMemoryUsage::Upload);
        const BufferHandle result =
            Buffer(ShadowReceiverMeasurement::kResultBytes,
                   static_cast<BufferUsage>(static_cast<uint32_t>(BufferUsage::Storage) |
                                            static_cast<uint32_t>(BufferUsage::TransferDst)),
                   BufferMemoryUsage::Readback);
        void* mapped = m_Device->MapBuffer(upload);
        EXPECT_NE(mapped, nullptr);
        std::memcpy(mapped, depth.data(), depth.size() * sizeof(float));
        m_Device->UnmapBuffer(upload);
        mapped = m_Device->MapBuffer(params);
        EXPECT_NE(mapped, nullptr);
        std::memcpy(mapped, &setup.Params, sizeof(setup.Params));
        m_Device->UnmapBuffer(params);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = m_Layout;
        const DescriptorSetHandle set = m_Device->CreateDescriptorSet(dsDesc);
        m_Sets.push_back(set);
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, texture, m_Sampler);
        m_Device->UpdateStorageBufferBinding(set, 1, result, 0, ShadowReceiverMeasurement::kResultBytes);
        DescriptorSetUpdate uniform{};
        uniform.binding = 2;
        uniform.type = DescriptorType::UniformBuffer;
        uniform.buffers = {params};
        uniform.bufferOffsets = {0};
        uniform.bufferRanges = {sizeof(ShadowReceiverReduce::ParamsGPU)};
        m_Device->UpdateDescriptorSet(set, uniform);

        constexpr size_t kSideBytes = ShadowReceiverMeasurement::kWordsPerSide * sizeof(uint32_t);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, kWidth, kHeight);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                                 ResourceState::ShaderResource));
        commands->FillBuffer(result, 0, kSideBytes, 0xFFFFFFFFu);
        commands->FillBuffer(result, kSideBytes, kSideBytes, 0u);
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(result, ResourceState::CopyDest,
                                                                ResourceState::UnorderedAccess));
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        uint32_t groupsX = 0;
        uint32_t groupsY = 0;
        ShadowReceiverReduce::DispatchGroups(kWidth, kHeight, groupsX, groupsY);
        commands->Dispatch(groupsX, groupsY, 1);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();

        mapped = m_Device->MapBuffer(result);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(readback.MinWords, mapped, ShadowReceiverMeasurement::kResultBytes);
        m_Device->UnmapBuffer(result);
        return readback;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    SamplerHandle m_Sampler{};
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
    std::vector<DescriptorSetHandle> m_Sets;
};

// The CPU reference for one sampled receiver: its view depth and, when it lies
// within the binned range, its light-space position and fractional bin, and its
// ray's direction per unit view depth in light space.
struct ReferenceSample
{
    float ViewDepth = 0.0f;
    bool Binned = false;
    float BinCoordinate = 0.0f;
    Vector3 LightSpace{};
    Vector3 Ray{};
};

// Light-space AABBs of the strict and loose reference boxes of each bin: a
// sample on a bin edge may land in either neighbour, so the strict box holds
// the samples unambiguously inside a bin, the loose box also the edge samples
// of both neighbours, and the measured box lies between the two. `field`
// picks the quantity boxed.
void ReferenceBoxes(const std::vector<ReferenceSample>& samples, Vector3 ReferenceSample::*field,
                    std::vector<AABB>& strict, std::vector<AABB>& loose)
{
    constexpr uint32_t kBins = ShadowReceiverMeasurement::kDepthBins;
    strict.assign(kBins, AABB::Empty());
    loose.assign(kBins, AABB::Empty());
    for (const ReferenceSample& s : samples)
    {
        if (!s.Binned)
            continue;
        const float t = s.BinCoordinate;
        const uint32_t bin = std::min(static_cast<uint32_t>(t), kBins - 1);
        const bool onEdge = std::abs(t - std::round(t)) < 1e-3f;
        if (!onEdge)
            strict[bin].Expand(s.*field);
        loose[bin].Expand(s.*field);
        if (onEdge)
        {
            const uint32_t other = t < std::round(t) ? std::min(bin + 1, kBins - 1) : (bin > 0 ? bin - 1 : 0);
            loose[other].Expand(s.*field);
        }
    }
}

// The measured box `got` of one bin lies between its strict and loose
// reference boxes, within `tolerance` per axis.
void ExpectBinBox(const AABB& got, const AABB& strict, const AABB& loose, float tolerance)
{
    if (!strict.IsEmpty())
        ASSERT_FALSE(got.IsEmpty());
    if (got.IsEmpty())
        return;
    ASSERT_FALSE(loose.IsEmpty()) << "a bin no receiver reaches holds a box";
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!strict.IsEmpty())
        {
            EXPECT_LE(got.min[axis], strict.min[axis] + tolerance) << "axis " << axis;
            EXPECT_GE(got.max[axis], strict.max[axis] - tolerance) << "axis " << axis;
        }
        EXPECT_GE(got.min[axis], loose.min[axis] - tolerance) << "axis " << axis;
        EXPECT_LE(got.max[axis], loose.max[axis] + tolerance) << "axis " << axis;
    }
}

TEST_P(ShadowReceiverReduceComputeTest, MeasuresTheNearestReceiverAndEachBinsLightSpaceBox)
{
    // A pitched camera over a tower: the top rows see sky (depth 0), the
    // middle rows ground past the shadow distance (measured near/far, never
    // binned), the rest the tower and the ground in front of it. Through the
    // orthographic window the rays are parallel, so its far plane is what
    // leaves the top rows empty.
    const PinholeCamera cameras[2] = {MakeCamera(Vector3{1.5f, 18.0f, -30.0f}, 18.0f, 0.0f, kFar),
                                      MakeCamera(Vector3{1.5f, 18.0f, -30.0f}, 18.0f, 15.0f, 90.0f)};
    const Vector3 lightDir = Vector3{0.35f, -0.65f, 0.45f}.Normalize();
    for (const PinholeCamera& cam : cameras)
    {
        SCOPED_TRACE(cam.Orthographic ? "orthographic" : "perspective");
        const ShadowReceiverReduce::Setup setup =
            ShadowReceiverReduce::MakeSetup(cam.Data, kNear, cam.Far, cam.Orthographic, lightDir,
                                            kMaxShadowDistance, kWidth, kHeight);
        const Matrix4x4 lightRot = ShadowMapRenderFeature::CascadeLightRotation(lightDir);
        const float binsPerLog = setup.Params.BinParams[1];

        std::vector<float> depth(kWidth * kHeight, 0.0f);
        std::vector<ReferenceSample> samples;
        // The rays of the samples that reach past every bin: sky, or ground past
        // the shadow distance.
        AABB past = AABB::Empty();
        uint32_t sky = 0;
        uint32_t beyond = 0;
        float nearest = std::numeric_limits<float>::max();
        // The depth words: the bits of the farthest and of the nearest sampled
        // receiver depth (reverse-Z, so the smallest and the largest value).
        uint32_t farthestBits = std::numeric_limits<uint32_t>::max();
        uint32_t nearestBits = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const std::optional<Vector3> hit = CastPixel(cam, x, y);
                if (hit)
                    depth[y * kWidth + x] = ReverseZDepth(cam, *hit);
                if (x % kStride != 0 || y % kStride != 0)
                    continue;
                // The sample's ray parameter: its direction per unit view depth
                // under a perspective camera, its offset from the eye under an
                // orthographic one.
                const Vector3 offset = PixelOffset(cam, x, y);
                const Vector4 ray =
                    lightRot.Transform(Vector4{cam.Orthographic ? offset : cam.Forward + offset, 0.0f});
                if (!hit)
                {
                    ++sky;
                    past.Expand(Vector3{ray.x, ray.y, ray.z});
                    continue;
                }
                uint32_t bits = 0;
                std::memcpy(&bits, &depth[y * kWidth + x], sizeof(bits));
                farthestBits = std::min(farthestBits, bits);
                nearestBits = std::max(nearestBits, bits);
                ReferenceSample s{};
                s.ViewDepth = Vector3::Dot(*hit - cam.Position, cam.Forward);
                nearest = std::min(nearest, s.ViewDepth);
                s.Binned = s.ViewDepth <= setup.Measured.BinFar;
                s.Ray = Vector3{ray.x, ray.y, ray.z};
                if (!s.Binned)
                {
                    ++beyond;
                    past.Expand(s.Ray);
                }
                s.BinCoordinate = std::log(std::max(s.ViewDepth, setup.Measured.BinNear) /
                                           setup.Measured.BinNear) *
                                  binsPerLog;
                const Vector4 ls = lightRot.Transform(Vector4{hit->x, hit->y, hit->z, 1.0f});
                s.LightSpace = Vector3{ls.x, ls.y, ls.z};
                samples.push_back(s);
            }
        }
        ASSERT_GT(sky, 0u) << "the pose must show sky";
        ASSERT_GT(beyond, 0u) << "the pose must show receivers past the shadow distance";

        const ShadowReceiverReadback readback = Reduce(depth, setup);
        EXPECT_EQ(readback.MinWords[0], farthestBits);
        EXPECT_EQ(readback.MaxWords[0], nearestBits);
        ShadowReceiverMeasurement measured{};
        ASSERT_TRUE(DecodeShadowReceiverResult(readback, measured));
        EXPECT_NEAR(measured.NearDepth, nearest, 0.01f);

        constexpr uint32_t kBins = ShadowReceiverMeasurement::kDepthBins;
        std::vector<AABB> strict;
        std::vector<AABB> loose;
        std::vector<AABB> strictRays;
        std::vector<AABB> looseRays;
        ReferenceBoxes(samples, &ReferenceSample::LightSpace, strict, loose);
        ReferenceBoxes(samples, &ReferenceSample::Ray, strictRays, looseRays);
        uint32_t occupied = 0;
        for (uint32_t bin = 0; bin < kBins; ++bin)
        {
            SCOPED_TRACE(::testing::Message() << "bin " << bin << " depth ["
                                              << measured.BinNearDepth(bin) << ", "
                                              << measured.BinFarDepth(bin) << ")");
            occupied += strict[bin].IsEmpty() ? 0u : 1u;
            EXPECT_EQ(measured.Bins[bin].IsEmpty(), measured.Rays[bin].IsEmpty())
                << "a bin's surfaces and their rays come from the same samples";
            // Reconstruction through fp32 reverse-Z depth: centimetres at these
            // distances, and that over the depth for a perspective ray.
            const float tolerance = 0.02f + 2e-4f * measured.BinFarDepth(bin);
            ExpectBinBox(measured.Bins[bin], strict[bin], loose[bin], tolerance);
            ExpectBinBox(measured.Rays[bin], strictRays[bin], looseRays[bin],
                         cam.Orthographic ? tolerance : tolerance / measured.BinNearDepth(bin));
        }
        EXPECT_GT(occupied, 8u) << "the pose must spread receivers over many bins";

        const AABB& gotPast = measured.Rays[kBins];
        ASSERT_FALSE(gotPast.IsEmpty()) << "no ray recorded past the last bin";
        for (int axis = 0; axis < 3; ++axis)
        {
            EXPECT_NEAR(gotPast.min[axis], past.min[axis], 1e-3f) << "axis " << axis;
            EXPECT_NEAR(gotPast.max[axis], past.max[axis], 1e-3f) << "axis " << axis;
        }
        // The context roots the rays at the camera, in the bins' light space.
        const Vector3 cameraLs = lightRot.TransformPoint(cam.Position);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(measured.Measured.CameraLightSpace[axis], cameraLs[axis], 1e-4f) << "axis " << axis;
    }
}

TEST_P(ShadowReceiverReduceComputeTest, AViewOfSkyOnlyDecodesAsNothingMeasured)
{
    const PinholeCamera cam = MakeCamera(Vector3{0.0f, 18.0f, -30.0f}, 18.0f, 0.0f, kFar);
    const ShadowReceiverReduce::Setup setup =
        ShadowReceiverReduce::MakeSetup(cam.Data, kNear, kFar, false, Vector3{0.0f, -1.0f, 0.0f},
                                        kMaxShadowDistance, kWidth, kHeight);
    const std::vector<float> depth(kWidth * kHeight, 0.0f);
    ShadowReceiverMeasurement measured{};
    EXPECT_FALSE(DecodeShadowReceiverResult(Reduce(depth, setup), measured));
    EXPECT_FALSE(measured.Valid);
}

INSTANTIATE_TEST_SUITE_P(Kernels, ShadowReceiverReduceComputeTest,
                         ::testing::Values(kDesktopKernel, kCompatKernel), KernelName);

} // namespace
