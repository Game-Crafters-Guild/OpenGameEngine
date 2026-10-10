#include "AssetCore/GUID.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace GameEngine {

GUID::GUID() {
    m_Data.fill(0);
}

GUID::GUID(const Data& data) : m_Data(data) {
}

GUID GUID::FromBytes(const uint8 (&bytes)[kSize]) {
    Data data{};
    std::copy_n(bytes, kSize, data.begin());
    return GUID(data);
}

void GUID::WriteBytes(uint8 (&out)[kSize]) const {
    std::copy_n(m_Data.begin(), kSize, out);
}

GUID::GUID(const String& str) {
    // Parse string format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    String cleanStr = str;
    cleanStr.erase(std::remove(cleanStr.begin(), cleanStr.end(), '-'), cleanStr.end());

    if (cleanStr.length() != kSize * 2) {
        m_Data.fill(0);
        return;
    }

    for (size_t i = 0; i < kSize; ++i) {
        String byteStr = cleanStr.substr(i * 2, 2);
        if (std::from_chars(byteStr.c_str(), byteStr.c_str() + 2, m_Data[i], 16).ec != std::errc())
        {
            m_Data.fill(0);
            return;
        }
    }
}

namespace {

// OS entropy words folded into each per-thread seed. 192 bits of entropy plus a
// process-wide sequence number: two threads, or two processes, cannot start from
// the same generator state.
constexpr size_t kSeedEntropyWords = 6;

// One generator per thread, so concurrent Generate calls share no state and
// never contend. The generator is trivially destructible: a call during thread
// or process teardown still reads valid state.
std::mt19937_64& ThreadGenerator()
{
    thread_local std::mt19937_64 generator = [] {
        static std::atomic<uint64> seedSequence{0};
        const uint64 sequence = seedSequence.fetch_add(1, std::memory_order_relaxed);

        std::random_device entropy;
        std::array<uint32, kSeedEntropyWords + 2> seedWords{};
        for (size_t i = 0; i < kSeedEntropyWords; ++i)
        {
            seedWords[i] = entropy();
        }
        seedWords[kSeedEntropyWords] = static_cast<uint32>(sequence);
        seedWords[kSeedEntropyWords + 1] = static_cast<uint32>(sequence >> 32);

        std::seed_seq seed(seedWords.begin(), seedWords.end());
        return std::mt19937_64(seed);
    }();
    return generator;
}

} // anonymous namespace

GUID GUID::Generate() {
    std::mt19937_64& generator = ThreadGenerator();
    const uint64 words[2] = { generator(), generator() };
    static_assert(sizeof(words) == kSize);

    Data data;
    std::memcpy(data.data(), words, kSize);

    // Set version (4) and variant bits according to RFC 4122
    data[6] = (data[6] & 0x0F) | 0x40; // Version 4
    data[8] = (data[8] & 0x3F) | 0x80; // Variant bits

    return GUID(data);
}

GUID GUID::Null() {
    return GUID();
}

bool GUID::IsNull() const {
    return std::all_of(m_Data.begin(), m_Data.end(), [](uint8 byte) {
        return byte == 0;
    });
}

String GUID::ToString() const {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (size_t i = 0; i < kSize; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << '-';
        }
        ss << std::setw(2) << static_cast<int>(m_Data[i]);
    }

    return ss.str();
}

String GUID::ToCompactString() const {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (uint8 byte : m_Data) {
        ss << std::setw(2) << static_cast<int>(byte);
    }

    return ss.str();
}

bool GUID::operator==(const GUID& other) const {
    return m_Data == other.m_Data;
}

bool GUID::operator!=(const GUID& other) const {
    return !(*this == other);
}

bool GUID::operator<(const GUID& other) const {
    return m_Data < other.m_Data;
}

} // namespace GameEngine

namespace GameEngine {

namespace {

// FNV-1a over a byte sequence. Uses an explicit uint64_t accumulator (not size_t)
// so the derived bits are identical on every platform — a 32-bit build must
// produce the same GUID as the 64-bit editor, or cross-machine identity breaks.
inline uint64_t FnvHash(const uint8* data, size_t length, uint64_t seed = 14695981039346656037ULL)
{
    uint64_t h = seed;
    for (size_t i = 0; i < length; ++i)
    {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL; // FNV prime
    }
    return h;
}

// SplitMix64 finalizer — a strong bit-avalanche of a 64-bit value. Used to derive the
// upper GUID word from the lower hash so it is well-distributed instead of correlated.
inline uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

} // anonymous namespace

GUID GUID::Derive(const GUID& parent, const String& subKey) {
    // Deterministic derivation: FNV-1a over raw parent bytes + separator + subKey bytes.
    // Same inputs -> same output on every platform (explicit uint64_t accumulators).
    //
    // This output is a PERSISTENCE CONTRACT, not runtime-only: embedded material/clip
    // GUIDs (DeriveMaterialGuid / DeriveEmbeddedClipGuid) are baked into scenes, and the
    // project source uses derived identity in shipped builds. Treat the byte layout as
    // FROZEN — changing it re-keys every derived asset and requires a coordinated re-bake.
    const auto& parentBytes = parent.GetData();

    // Lower 64 bits: FNV-1a over parentBytes || '|' || subKey.
    uint64_t h1 = FnvHash(parentBytes.data(), kSize);
    const uint8 separator = '|';
    h1 = FnvHash(&separator, 1, h1);
    h1 = FnvHash(reinterpret_cast<const uint8*>(subKey.data()), subKey.size(), h1);

    // Upper 64 bits: avalanche the lower hash with a SplitMix64 finalizer, so the upper
    // word is well distributed rather than correlated with the lower one. Identity
    // entropy is ~64 bits — astronomically collision-safe at project asset counts.
    uint64_t h2 = SplitMix64(h1);

    GUID::Data data{};
    for (int i = 0; i < 8; ++i) {
        data[i]     = static_cast<uint8>((h1 >> (i * 8)) & 0xFF);
        data[8 + i] = static_cast<uint8>((h2 >> (i * 8)) & 0xFF);
    }

    // Set RFC4122-like bits for stability
    data[6] = (data[6] & 0x0F) | 0x40; // Version 4-style
    data[8] = (data[8] & 0x3F) | 0x80; // Variant

    return GUID(data);
}
} // namespace GameEngine


// std::hash<GameEngine::GUID> is defined inline in the header (see GUID.h).
