/**
 * @file SpecializationConstantsTest.cpp
 * @brief Unit tests for Specialization Constants functionality
 */

#include <gtest/gtest.h>
#include "Rendering/Core/SpecializationConstants.h"

using namespace GameEngine::Rendering;

class SpecializationConstantsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Fresh constants for each test
        m_Constants = std::make_unique<SpecializationConstants>();
    }

    void TearDown() override {
        m_Constants.reset();
    }

    std::unique_ptr<SpecializationConstants> m_Constants;
};

TEST_F(SpecializationConstantsTest, BasicConstantAddition) {
    // Test adding different types of constants
    m_Constants->AddConstant(0, 42u, "TestUInt");
    m_Constants->AddConstant(1, 3.14f, "TestFloat");
    m_Constants->AddConstant(2, true, "TestBool");

    EXPECT_EQ(m_Constants->GetConstantCount(), 3u);
    EXPECT_FALSE(m_Constants->IsEmpty());
}

TEST_F(SpecializationConstantsTest, TypeSafeRetrieval) {
    uint32_t originalValue = 123u;
    m_Constants->AddConstant(0, originalValue);

    uint32_t retrievedValue;
    ASSERT_TRUE(m_Constants->GetConstant(0, retrievedValue));
    EXPECT_EQ(retrievedValue, originalValue);

    // Type mismatch should fail
    float floatValue;
    EXPECT_FALSE(m_Constants->GetConstant(0, floatValue));
}

TEST_F(SpecializationConstantsTest, ConstantUpdate) {
    m_Constants->AddConstant(0, 100u);
    
    uint32_t value;
    ASSERT_TRUE(m_Constants->GetConstant(0, value));
    EXPECT_EQ(value, 100u);

    // Update the constant
    ASSERT_TRUE(m_Constants->UpdateConstant(0, 200u));
    ASSERT_TRUE(m_Constants->GetConstant(0, value));
    EXPECT_EQ(value, 200u);

    // Update with wrong type should fail
    EXPECT_FALSE(m_Constants->UpdateConstant(0, 3.14f));
}

TEST_F(SpecializationConstantsTest, CommonConstants) {
    // Test using common constant IDs
    m_Constants->AddConstant(CommonConstants::kWorkgroupSizeX, 32u);
    m_Constants->AddConstant(CommonConstants::kWorkgroupSizeY, 16u);
    m_Constants->AddConstant(CommonConstants::kEnableShadows, true);

    uint32_t workgroupX, workgroupY;
    bool enableShadows;

    ASSERT_TRUE(m_Constants->GetConstant(CommonConstants::kWorkgroupSizeX, workgroupX));
    ASSERT_TRUE(m_Constants->GetConstant(CommonConstants::kWorkgroupSizeY, workgroupY));
    ASSERT_TRUE(m_Constants->GetConstant(CommonConstants::kEnableShadows, enableShadows));

    EXPECT_EQ(workgroupX, 32u);
    EXPECT_EQ(workgroupY, 16u);
    EXPECT_TRUE(enableShadows);
}

TEST_F(SpecializationConstantsTest, BuilderPattern) {
    auto constants = SpecializationConstantsBuilder()
        .Add(0, 64u, "WorkgroupSize")
        .Add(1, true, "EnableOptimization")
        .Add(2, 2.5f, "ScaleFactor")
        .Build();

    EXPECT_EQ(constants.GetConstantCount(), 3u);

    uint32_t workgroupSize;
    bool enableOpt;
    float scaleFactor;

    ASSERT_TRUE(constants.GetConstant(0, workgroupSize));
    ASSERT_TRUE(constants.GetConstant(1, enableOpt));
    ASSERT_TRUE(constants.GetConstant(2, scaleFactor));

    EXPECT_EQ(workgroupSize, 64u);
    EXPECT_TRUE(enableOpt);
    EXPECT_FLOAT_EQ(scaleFactor, 2.5f);
}

TEST_F(SpecializationConstantsTest, VulkanSpecializationInfo) {
    m_Constants->AddConstant(0, 32u);
    m_Constants->AddConstant(1, true);
    m_Constants->AddConstant(2, 1.5f);

    const VkSpecializationInfo* specInfo = m_Constants->GetVkSpecializationInfo();
    ASSERT_NE(specInfo, nullptr);

    EXPECT_EQ(specInfo->mapEntryCount, 3u);
    EXPECT_NE(specInfo->pMapEntries, nullptr);
    EXPECT_GT(specInfo->dataSize, 0u);
    EXPECT_NE(specInfo->pData, nullptr);

    // Verify map entries are sorted by constant ID
    for (uint32_t i = 1; i < specInfo->mapEntryCount; ++i) {
        EXPECT_LT(specInfo->pMapEntries[i-1].constantID, specInfo->pMapEntries[i].constantID);
    }
}

TEST_F(SpecializationConstantsTest, ConstantRemoval) {
    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(1, 200u);
    m_Constants->AddConstant(2, 300u);

    EXPECT_EQ(m_Constants->GetConstantCount(), 3u);

    m_Constants->RemoveConstant(1);
    EXPECT_EQ(m_Constants->GetConstantCount(), 2u);

    uint32_t value;
    EXPECT_TRUE(m_Constants->GetConstant(0, value));
    EXPECT_FALSE(m_Constants->GetConstant(1, value)); // Should be removed
    EXPECT_TRUE(m_Constants->GetConstant(2, value));
}

TEST_F(SpecializationConstantsTest, ClearAllConstants) {
    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(1, 200u);
    
    EXPECT_FALSE(m_Constants->IsEmpty());
    
    m_Constants->Clear();
    
    EXPECT_TRUE(m_Constants->IsEmpty());
    EXPECT_EQ(m_Constants->GetConstantCount(), 0u);
    EXPECT_EQ(m_Constants->GetVkSpecializationInfo(), nullptr);
}

TEST_F(SpecializationConstantsTest, ConstantMerging) {
    SpecializationConstants other;
    other.AddConstant(10, 500u);
    other.AddConstant(11, false);

    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(10, 999u); // Will be overwritten

    m_Constants->Merge(other, true); // Overwrite existing

    EXPECT_EQ(m_Constants->GetConstantCount(), 3u);

    uint32_t value;
    bool boolValue;
    ASSERT_TRUE(m_Constants->GetConstant(0, value));
    EXPECT_EQ(value, 100u); // Original value preserved

    ASSERT_TRUE(m_Constants->GetConstant(10, value));
    EXPECT_EQ(value, 500u); // Overwritten value

    ASSERT_TRUE(m_Constants->GetConstant(11, boolValue));
    EXPECT_FALSE(boolValue); // New value
}

TEST_F(SpecializationConstantsTest, HashGeneration) {
    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(1, 200u);
    
    uint64_t hash1 = m_Constants->GetHash();
    EXPECT_NE(hash1, 0u);

    // Same constants should produce same hash
    SpecializationConstants identical;
    identical.AddConstant(0, 100u);
    identical.AddConstant(1, 200u);
    
    uint64_t hash2 = identical.GetHash();
    EXPECT_EQ(hash1, hash2);

    // Different constants should produce different hash
    SpecializationConstants different;
    different.AddConstant(0, 101u); // Different value
    different.AddConstant(1, 200u);
    
    uint64_t hash3 = different.GetHash();
    EXPECT_NE(hash1, hash3);
}

TEST_F(SpecializationConstantsTest, VariantCreation) {
    m_Constants->AddConstant(0, 100u);
    m_Constants->AddConstant(1, true);

    auto variant = m_Constants->CreateVariant();
    
    // Variant should have same constants
    EXPECT_EQ(variant.GetConstantCount(), m_Constants->GetConstantCount());
    
    uint32_t value;
    bool boolValue;
    ASSERT_TRUE(variant.GetConstant(0, value));
    ASSERT_TRUE(variant.GetConstant(1, boolValue));
    EXPECT_EQ(value, 100u);
    EXPECT_TRUE(boolValue);

    // Modifying variant shouldn't affect original
    variant.UpdateConstant(0, 999u);
    
    ASSERT_TRUE(m_Constants->GetConstant(0, value));
    EXPECT_EQ(value, 100u); // Original unchanged
    
    ASSERT_TRUE(variant.GetConstant(0, value));
    EXPECT_EQ(value, 999u); // Variant changed
}

TEST_F(SpecializationConstantsTest, ErrorHandling) {
    // Test non-existent constant
    uint32_t value;
    EXPECT_FALSE(m_Constants->GetConstant(999, value));
    EXPECT_FALSE(m_Constants->UpdateConstant(999, 123u));

    // Test removing non-existent constant (should not crash)
    m_Constants->RemoveConstant(999);
    EXPECT_EQ(m_Constants->GetConstantCount(), 0u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
