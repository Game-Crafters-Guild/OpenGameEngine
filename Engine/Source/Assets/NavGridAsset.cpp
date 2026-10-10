#include "Assets/NavGridAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <cstring>
#include <fstream>

namespace GameEngine {

namespace {

static constexpr char kMagic[4] = {'N', 'G', 'R', 'D'};
static constexpr uint32 kVersion = 1;

// Header layout (field-by-field, no struct memcpy):
// Offset  Size  Content
// 0       4     Magic "NGRD"
// 4       4     Version (uint32)
// 8       1     GridType (uint8)
// 9       4     CellSize (float32)
// 13      4     OriginX
// 17      4     OriginY
// 21      4     OriginZ
// 25      4     Width (uint32)
// 29      4     Depth (uint32)
// 33      4     MaxSlope
// 37      4     MaxStepHeight
// 41      ...   costs array, then blocked array
static constexpr size_t kHeaderSize = 41;

template<typename T>
static T ReadField(const uint8* data, size_t& offset)
{
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

template<typename T>
static void WriteField(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

} // namespace

NavGridAsset::NavGridAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::NavigationGrid, path)
{
}

NavGridAsset::~NavGridAsset()
{
    Unload();
}

bool NavGridAsset::Load()
{
    if (GetState() == AssetState::Loaded)
    {
        return true;
    }

    SetState(AssetState::Loading);

    if (!Exists())
    {
        Logger::Log::Error("NavGrid file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    std::vector<uint8> fileData;
    if (!ReadFileBytesShared(GetPath(), fileData))
    {
        Logger::Log::Error("Failed to read NavGrid file: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    return LoadFromData(fileData);
}

bool NavGridAsset::LoadFromData(const Vector<uint8>& data)
{
    if (GetState() == AssetState::Loaded)
    {
        return true;
    }

    SetState(AssetState::Loading);

    if (!ParseBinary(data.data(), data.size()))
    {
        SetState(AssetState::Failed);
        return false;
    }

    SetState(AssetState::Loaded);
    Logger::Log::Info("NavGrid loaded: {} ({}x{}, cellSize={})",
                      GetName(), m_Settings.Width, m_Settings.Depth, m_Settings.CellSize);
    return true;
}

bool NavGridAsset::ParseBinary(const uint8* data, size_t size)
{
    if (!data || size < kHeaderSize)
    {
        Logger::Log::Error("NavGrid data too small: {} bytes (minimum {})", size, kHeaderSize);
        return false;
    }

    // Validate magic
    if (std::memcmp(data, kMagic, 4) != 0)
    {
        Logger::Log::Error("NavGrid invalid magic bytes");
        return false;
    }

    size_t offset = 4;

    const uint32 version = ReadField<uint32>(data, offset);
    if (version != kVersion)
    {
        Logger::Log::Error("NavGrid unsupported version: {} (expected {})", version, kVersion);
        return false;
    }

    m_Settings.Type = static_cast<Pathfinding::GridType>(ReadField<uint8>(data, offset));
    m_Settings.CellSize = ReadField<float32>(data, offset);
    m_Settings.OriginX = ReadField<float32>(data, offset);
    m_Settings.OriginY = ReadField<float32>(data, offset);
    m_Settings.OriginZ = ReadField<float32>(data, offset);
    m_Settings.Width = ReadField<uint32>(data, offset);
    m_Settings.Depth = ReadField<uint32>(data, offset);
    m_Settings.MaxSlope = ReadField<float32>(data, offset);
    m_Settings.MaxStepHeight = ReadField<float32>(data, offset);

    static_assert(sizeof(float32) == 4, "float32 must be 4 bytes for binary format");

    if (m_Settings.Width == 0 || m_Settings.Depth == 0)
    {
        Logger::Log::Error("NavGrid dimensions must be non-zero: {}x{}", m_Settings.Width, m_Settings.Depth);
        return false;
    }

    // Check for integer overflow before multiplication
    if (m_Settings.Width > 0 && m_Settings.Depth > UINT32_MAX / m_Settings.Width)
    {
        Logger::Log::Error("NavGrid dimensions overflow: {}x{}", m_Settings.Width, m_Settings.Depth);
        return false;
    }

    const uint32 cellCount = m_Settings.Width * m_Settings.Depth;
    const size_t costsBytes = static_cast<size_t>(cellCount) * sizeof(float32);
    const size_t blockedBytes = static_cast<size_t>(cellCount);
    const size_t expectedSize = kHeaderSize + costsBytes + blockedBytes;

    if (size < expectedSize)
    {
        Logger::Log::Error("NavGrid data truncated: {} bytes (expected {} for {}x{} grid)",
                           size, expectedSize, m_Settings.Width, m_Settings.Depth);
        return false;
    }

    m_Costs.resize(cellCount);
    std::memcpy(m_Costs.data(), data + offset, costsBytes);
    offset += costsBytes;

    m_Blocked.resize(cellCount);
    std::memcpy(m_Blocked.data(), data + offset, blockedBytes);

    return true;
}

void NavGridAsset::Unload()
{
    m_Costs.clear();
    m_Blocked.clear();
    m_Settings = Pathfinding::GridSettings{};
    SetState(AssetState::Unloaded);
}

size_t NavGridAsset::GetMemoryUsage() const
{
    return m_Costs.size() * sizeof(float32) + m_Blocked.size() * sizeof(uint8);
}

const Pathfinding::GridSettings& NavGridAsset::GetGridSettings() const
{
    return m_Settings;
}

const std::vector<float32>& NavGridAsset::GetCosts() const
{
    return m_Costs;
}

const std::vector<uint8>& NavGridAsset::GetBlocked() const
{
    return m_Blocked;
}

bool NavGridAsset::SaveToFile(const std::filesystem::path& path,
                              const Pathfinding::GridSettings& settings,
                              const float32* costs, const uint8* blocked,
                              uint32 cellCount)
{
    if (!costs || !blocked)
    {
        Logger::Log::Error("NavGrid SaveToFile: null data pointers");
        return false;
    }

    if (cellCount != settings.Width * settings.Depth)
    {
        Logger::Log::Error("NavGrid SaveToFile: cellCount {} does not match Width*Depth ({}x{})",
                           cellCount, settings.Width, settings.Depth);
        return false;
    }

    // Ensure parent directory exists
    auto parentDir = path.parent_path();
    if (!parentDir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        Logger::Log::Error("NavGrid SaveToFile: failed to open '{}'", path.string());
        return false;
    }

    // Header
    file.write(kMagic, 4);
    WriteField(file, kVersion);
    WriteField(file, static_cast<uint8>(settings.Type));
    WriteField(file, settings.CellSize);
    WriteField(file, settings.OriginX);
    WriteField(file, settings.OriginY);
    WriteField(file, settings.OriginZ);
    WriteField(file, settings.Width);
    WriteField(file, settings.Depth);
    WriteField(file, settings.MaxSlope);
    WriteField(file, settings.MaxStepHeight);

    // Costs array
    file.write(reinterpret_cast<const char*>(costs),
               static_cast<std::streamsize>(cellCount) * sizeof(float32));

    // Blocked array
    file.write(reinterpret_cast<const char*>(blocked),
               static_cast<std::streamsize>(cellCount));

    if (!file.good())
    {
        Logger::Log::Error("NavGrid SaveToFile: write error for '{}'", path.string());
        return false;
    }

    Logger::Log::Info("NavGrid saved: {} ({}x{})", path.string(), settings.Width, settings.Depth);
    return true;
}

} // namespace GameEngine
