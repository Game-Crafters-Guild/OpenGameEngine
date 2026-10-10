#include "Assets/LodAssetSettings.h"
#include "Assets/MeshLODGenerator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
#include <string>

using namespace GameEngine;

namespace {

// RAII: force a comma-decimal C locale for the scope, restore on exit. Returns
// whether the locale was actually available on this machine.
class ScopedCommaLocale {
public:
    ScopedCommaLocale() {
        m_Previous = std::setlocale(LC_ALL, nullptr);
        if (m_Previous) m_Saved = m_Previous;
        // German uses a comma decimal separator; try a few spellings.
        for (const char* name : {"de-DE", "de_DE.UTF-8", "de_DE", "German_Germany.1252"}) {
            if (std::setlocale(LC_ALL, name)) { m_Applied = true; break; }
        }
    }
    ~ScopedCommaLocale() {
        if (!m_Saved.empty()) std::setlocale(LC_ALL, m_Saved.c_str());
    }
    bool Applied() const { return m_Applied; }
private:
    const char* m_Previous = nullptr;
    std::string m_Saved;
    bool m_Applied = false;
};

} // namespace

TEST(LodAssetSettings, CsvRoundTripsDefaults) {
    const float ratios[4] = {1.0f, 0.5f, 0.25f, 0.1f};
    const std::string csv = LodAssetSettings::EncodeFloatCsv(ratios, 4u);

    float out[4] = {9, 9, 9, 9};
    LodAssetSettings::DecodeFloatCsv(csv, out, 4u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 0.5f);
    EXPECT_FLOAT_EQ(out[2], 0.25f);
    EXPECT_FLOAT_EQ(out[3], 0.1f);
}

TEST(LodAssetSettings, CsvMalformedTokensKeepDefaults) {
    float out[4] = {1.0f, 0.5f, 0.25f, 0.1f}; // caller pre-seeds defaults
    LodAssetSettings::DecodeFloatCsv("abc, ,0.9", out, 4u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);  // "abc" -> default
    EXPECT_FLOAT_EQ(out[1], 0.5f);  // empty -> default
    EXPECT_FLOAT_EQ(out[2], 0.9f);  // parsed
    EXPECT_FLOAT_EQ(out[3], 0.1f);  // missing -> default
}

// N8: std::from_chars is locale-independent, so comma-separated dot-decimals
// survive a comma-decimal locale (where std::stof would misparse).
TEST(LodAssetSettings, CsvParsesUnderCommaDecimalLocale) {
    ScopedCommaLocale locale;
    if (!locale.Applied()) GTEST_SKIP() << "comma-decimal locale not installed";

    float out[4] = {0, 0, 0, 0};
    LodAssetSettings::DecodeFloatCsv("1,0.5,0.25,0.1", out, 4u);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 0.5f);
    EXPECT_FLOAT_EQ(out[2], 0.25f);
    EXPECT_FLOAT_EQ(out[3], 0.1f);

    // Encode is locale-safe too: no comma appears inside a value.
    const std::string csv = LodAssetSettings::EncodeFloatCsv(out, 4u);
    EXPECT_EQ(std::count(csv.begin(), csv.end(), ','), 3);
}

TEST(LodAssetSettings, ResolveUsesGlobalTierWhenUseGlobal) {
    LODImportSettings global;
    global.AutoGenerateOnImport = true;
    global.Config.LodCount = 3;

    LodAssetSettings perAsset;              // UseGlobal defaults true
    perAsset.Generate = false;              // ignored when UseGlobal
    perAsset.GenerateSkinned = true;        // ignored when UseGlobal

    const ResolvedLodSettings r = ResolveLodSettings(perAsset, global);
    EXPECT_TRUE(r.Generate);                // from global
    EXPECT_FALSE(r.GenerateSkinned);        // global tier never generates skinned
    EXPECT_EQ(r.Config.LodCount, 3u);       // from global
}

TEST(LodAssetSettings, ResolveUsesPerAssetWhenOverriding) {
    LODImportSettings global;
    global.AutoGenerateOnImport = false;

    LodAssetSettings perAsset;
    perAsset.UseGlobal = false;
    perAsset.Generate = true;
    perAsset.GenerateSkinned = true;
    perAsset.LodCount = 2;
    perAsset.TargetRatios[1] = 0.4f;

    const ResolvedLodSettings r = ResolveLodSettings(perAsset, global);
    EXPECT_TRUE(r.Generate);
    EXPECT_TRUE(r.GenerateSkinned);
    EXPECT_EQ(r.Config.LodCount, 2u);
    EXPECT_FLOAT_EQ(r.Config.TargetRatios[1], 0.4f);
}

TEST(LodAssetSettings, ToConfigClampsLodCount) {
    LodAssetSettings s;
    s.LodCount = 99;                        // out of range
    EXPECT_EQ(s.ToConfig().LodCount, MeshLODConfig::kMaxLODs);
    s.LodCount = 0;
    EXPECT_EQ(s.ToConfig().LodCount, 1u);
}
