#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/ParserRegistry.h"

#include <atomic>

using namespace GameEngine;

namespace
{

class CountingParser final : public AssetParser
{
  public:
    explicit CountingParser(std::atomic<int>* canParseCalls)
        : m_CanParseCalls(canParseCalls)
    {
    }

    AssetType GetAssetType() const override { return AssetType::Unknown; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".foo"}; }

    bool CanParse(const std::filesystem::path& /*filePath*/) const override
    {
        if (m_CanParseCalls)
        {
            m_CanParseCalls->fetch_add(1);
        }
        return true;
    }

    AssetParseResult Parse(const AssetMetadata& /*metadata*/, AssetManager& /*assetManager*/) override
    {
        return AssetParseResult::NotForMe("test");
    }

    std::string GetName() const override { return "CountingParser"; }

    int GetPriority() const override { return 999; }

  private:
    std::atomic<int>* m_CanParseCalls = nullptr;
};

} // namespace

TEST(ParserRegistry, DoesNotProbeParsersForOtherExtensions_FindParser)
{
    std::atomic<int> canParseCalls{0};

    ParserRegistry reg;
    auto p = std::make_shared<CountingParser>(&canParseCalls);
    ASSERT_TRUE(reg.RegisterParser(p, p->GetPriority()));

    // Query for a different extension; this must not touch the .foo parser at all.
    EXPECT_EQ(reg.FindParser("dummy.bar"), nullptr);
    EXPECT_EQ(canParseCalls.load(), 0);
}

TEST(ParserRegistry, DoesNotProbeParsersForOtherExtensions_ParseAsset)
{
    std::atomic<int> canParseCalls{0};

    ParserRegistry reg;
    auto p = std::make_shared<CountingParser>(&canParseCalls);
    ASSERT_TRUE(reg.RegisterParser(p, p->GetPriority()));

    AssetManager mgr; // not initialized; ParseAsset should return early and not touch it.
    AssetMetadata md{};
    md.Path = "dummy.bar";
    md.Extension = ".bar";

    AssetParseResult r = reg.ParseAsset(md, mgr);
    EXPECT_FALSE(r.Success);
    EXPECT_EQ(canParseCalls.load(), 0);
}
