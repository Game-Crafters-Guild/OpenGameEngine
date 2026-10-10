#include <gtest/gtest.h>
#include "Rendering/Core/CommandList.h"

using namespace GameEngine::Rendering;

TEST(ResourceBarrierTest, DefaultsAndExplicitSubresource) {
    // Default constructed
    ResourceBarrier def;
    EXPECT_EQ(def.type, ResourceBarrier::Buffer);
    EXPECT_EQ(def.bufferHandle, INVALID_HANDLE);
    EXPECT_EQ(def.textureHandle, INVALID_HANDLE);
    EXPECT_EQ(def.subresource.baseMip, 0u);
    EXPECT_EQ(def.subresource.levelCount, 1u);
    EXPECT_EQ(def.subresource.baseLayer, 0u);
    EXPECT_EQ(def.subresource.layerCount, 1u);
    EXPECT_EQ(def.srcQueueFamily, kQueueFamilyIgnored);
    EXPECT_EQ(def.dstQueueFamily, kQueueFamilyIgnored);
    EXPECT_EQ(def.srcStageMask, 0ull);
    EXPECT_EQ(def.dstStageMask, 0ull);
    EXPECT_EQ(def.srcAccessMask, 0ull);
    EXPECT_EQ(def.dstAccessMask, 0ull);
    // Only RenderGraph recording may claim layout authority: a default (or
    // factory-built) barrier must resolve oldLayout through the trackers.
    EXPECT_FALSE(def.rgAuthoritativeLayout);

    // Explicit texture barrier with subresource
    TextureHandle tex(123, 1);
    auto tb = ResourceBarrier::CreateTextureBarrier(tex, ResourceState::ShaderResource, ResourceState::CopyDest,
                                                    2u, 3u, 4u, 5u);
    EXPECT_EQ(tb.type, ResourceBarrier::Texture);
    EXPECT_EQ(tb.textureHandle, tex);
    EXPECT_EQ(tb.subresource.baseMip, 2u);
    EXPECT_EQ(tb.subresource.levelCount, 3u);
    EXPECT_EQ(tb.subresource.baseLayer, 4u);
    EXPECT_EQ(tb.subresource.layerCount, 5u);
    EXPECT_EQ(tb.srcQueueFamily, kQueueFamilyIgnored);
    EXPECT_EQ(tb.dstQueueFamily, kQueueFamilyIgnored);
    EXPECT_FALSE(tb.rgAuthoritativeLayout);
}

TEST(ResourceBarrierTest, BufferBarrierDefaults) {
    BufferHandle buf(456, 1);
    auto bb = ResourceBarrier::CreateBufferBarrier(buf, ResourceState::CopySource, ResourceState::CopyDest);
    EXPECT_EQ(bb.type, ResourceBarrier::Buffer);
    EXPECT_EQ(bb.bufferHandle, buf);
    // Subresource ignored for buffers; should be default full range
    EXPECT_EQ(bb.subresource.baseMip, 0u);
    EXPECT_EQ(bb.subresource.levelCount, 1u);
    EXPECT_EQ(bb.subresource.baseLayer, 0u);
    EXPECT_EQ(bb.subresource.layerCount, 1u);
}

