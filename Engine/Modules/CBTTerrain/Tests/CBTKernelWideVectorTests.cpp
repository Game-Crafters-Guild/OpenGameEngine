// The compiled CBT kernels carry no 64-bit integer VECTOR arithmetic or conversion.
//
// Trap: on the NVIDIA driver, a chain of 64-bit integer vector ops (a vector multiply-add of the
// barycentric weights and a vector-to-float conversion) can evaluate as a lane sum, (u+v, u+v), on
// a fraction of invocations; observed after a terrain re-provision with blocking readbacks between
// frames. Each affected bisector caches a degenerate sliver, which Classify culls and then freezes
// on. The kernels therefore do every 64-bit operation on scalars (cbt_domain.glsl,
// cbt_kernels.comp), which is exact and costs nothing, and this test holds the compiled blobs to
// it: the source can drift back to a vector form through a helper, a macro or a refactor without
// anyone reading the SPIR-V.
//
// The check walks the instruction stream (not a byte search: an opcode's value can occur inside a
// literal or an id). Loads, stores, composite construction and extraction of a 64-bit vector are
// data movement and allowed; integer arithmetic, shifts, bitwise ops and conversions are not.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "CBTTestHarness.h"

using namespace GameEngine::CBTTerrain::Test;

namespace
{
constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kSpvHeaderWords = 5u;
constexpr uint32_t kSpvOpTypeInt = 21u;
constexpr uint32_t kSpvOpTypeVector = 23u;
constexpr uint32_t kWideIntegerBits = 64u;

struct ForbiddenOp
{
    uint32_t Opcode;
    const char* Name;
};

// Integer arithmetic, shifts, bitwise ops and conversions (SPIR-V unified1 opcode values).
constexpr ForbiddenOp kForbiddenOnWideVectors[] = {
    {109u, "OpConvertFToU"},        {110u, "OpConvertFToS"},       {111u, "OpConvertSToF"},
    {112u, "OpConvertUToF"},        {113u, "OpUConvert"},          {114u, "OpSConvert"},
    {124u, "OpBitcast"},            {126u, "OpSNegate"},           {128u, "OpIAdd"},
    {130u, "OpISub"},               {132u, "OpIMul"},              {134u, "OpUDiv"},
    {135u, "OpSDiv"},               {137u, "OpUMod"},              {138u, "OpSRem"},
    {139u, "OpSMod"},               {194u, "OpShiftRightLogical"}, {195u, "OpShiftRightArithmetic"},
    {196u, "OpShiftLeftLogical"},   {197u, "OpBitwiseOr"},         {198u, "OpBitwiseXor"},
    {199u, "OpBitwiseAnd"},         {200u, "OpNot"},
};

const char* ForbiddenOpName(uint32_t opcode)
{
    for (const ForbiddenOp& op : kForbiddenOnWideVectors)
        if (op.Opcode == opcode)
            return op.Name;
    return nullptr;
}

struct WideVectorScan
{
    bool Valid = false;             // the module parsed from magic to its last instruction
    uint32_t WideVectorTypes = 0;   // 64-bit integer vector types the module declares
    std::vector<std::string> Offenders; // "<op> at word <n>", in module order
    std::string Error;
};

// One forward pass. The logical layout puts every name and decoration before the type section, so
// once a 64-bit vector type exists, an instruction whose first operand word is that type id is a
// result-typed instruction producing a value of it.
WideVectorScan ScanWideVectorOps(const std::vector<uint32_t>& words)
{
    WideVectorScan scan;
    if (words.size() < kSpvHeaderWords || words[0] != kSpvMagic)
    {
        scan.Error = "not a SPIR-V module";
        return scan;
    }
    std::unordered_set<uint32_t> wideIntegers;
    std::unordered_set<uint32_t> wideVectorTypes;
    std::unordered_set<uint32_t> wideVectorValues;
    for (size_t i = kSpvHeaderWords; i < words.size();)
    {
        const uint32_t opcode = words[i] & 0xFFFFu;
        const uint32_t wordCount = words[i] >> 16;
        if (wordCount == 0u || i + wordCount > words.size())
        {
            scan.Error = "malformed instruction stream at word " + std::to_string(i);
            return scan;
        }
        const uint32_t* operands = &words[i + 1u];
        const uint32_t operandCount = wordCount - 1u;
        if (opcode == kSpvOpTypeInt && operandCount >= 2u && operands[1] == kWideIntegerBits)
            wideIntegers.insert(operands[0]);
        else if (opcode == kSpvOpTypeVector && operandCount >= 2u && wideIntegers.count(operands[1]) != 0u)
            wideVectorTypes.insert(operands[0]);
        else if (operandCount >= 2u && wideVectorTypes.count(operands[0]) != 0u)
            wideVectorValues.insert(operands[1]);

        if (const char* name = ForbiddenOpName(opcode); name && operandCount >= 2u)
        {
            bool wide = wideVectorTypes.count(operands[0]) != 0u;
            for (uint32_t k = 2u; k < operandCount && !wide; ++k)
                wide = wideVectorValues.count(operands[k]) != 0u;
            if (wide)
                scan.Offenders.push_back(std::string(name) + " at word " + std::to_string(i));
        }
        i += wordCount;
    }
    scan.WideVectorTypes = static_cast<uint32_t>(wideVectorTypes.size());
    scan.Valid = true;
    return scan;
}

std::vector<uint32_t> ReadWords(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize bytes = file.tellg();
    if (bytes <= 0 || (bytes % 4) != 0)
        return {};
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4u);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), bytes);
    return file ? words : std::vector<uint32_t>{};
}

std::string Join(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines)
        out += "\n  " + line;
    return out;
}

// Asserts the blob parses and carries no forbidden op; returns the scan for arm-specific checks.
WideVectorScan ExpectNoWideVectorOps(const char* blob)
{
    const std::filesystem::path path = ShaderOutputDir() / blob;
    const std::vector<uint32_t> words = ReadWords(path);
    EXPECT_FALSE(words.empty()) << "cannot read " << path.string() << " (CBTCompileShaders not built?)";
    const WideVectorScan scan = ScanWideVectorOps(words);
    EXPECT_TRUE(scan.Valid) << path.string() << ": " << scan.Error;
    EXPECT_TRUE(scan.Offenders.empty())
        << blob << " does 64-bit integer VECTOR arithmetic or conversion; do it per component on "
        << "scalars (see the trap in cbt_domain.glsl):" << Join(scan.Offenders);
    return scan;
}
} // namespace

TEST(CBTKernelWideVectorOps, WideHeapKernelsUseScalar64BitArithmeticOnly)
{
    const WideVectorScan scan = ExpectNoWideVectorOps("cbt_kernels.comp.spv");
    // The wide arm does hold 64-bit vectors (the barycentric walk's state): a scan that saw none
    // would be looking at the wrong module, not proving anything.
    EXPECT_GT(scan.WideVectorTypes, 0u);
}

TEST(CBTKernelWideVectorOps, NarrowHeapKernelsUseScalar64BitArithmeticOnly)
{
    ExpectNoWideVectorOps("cbt_kernels_heap32.comp.spv");
}
