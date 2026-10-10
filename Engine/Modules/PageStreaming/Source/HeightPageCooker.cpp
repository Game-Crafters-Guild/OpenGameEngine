#include "PageStreaming/HeightPageCooker.h"

#include "HeightPyramidBuilder.h"
#include "HeightSourceRows.h"
#include "SourceRangeReader.h"
#include "PageStreaming/HeightPageCodec.h"
#include "PageStreaming/PageStoreReader.h"
#include "PageStreaming/PageStoreWriter.h"

#include "AssetCore/SharedFileRead.h"
#include "Terrain/Heightfield.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>


namespace GameEngine::PageStreaming
{
namespace
{

// Bytes read per step of the whole-file scan.
constexpr std::size_t kScanChunkBytes = 64u * 1024u * 1024u;
// Level-0 rows read per step of the cook: one page row's owned rows.
constexpr uint32 kCookBandRows = kPageOwnedSamples;

std::string LowerExtension(const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string ResolveRawSource(const std::filesystem::path& file, HeightSourceFormat format, uint32 samplesX,
                             uint32 samplesZ, HeightCookSource& out)
{
    std::error_code ec;
    const uint64 fileBytes = std::filesystem::file_size(file, ec);
    if (ec)
        return "the heightmap " + file.string() + " could not be read";
    const uint64 sampleBytes = format == HeightSourceFormat::R16 ? 2u : 4u;
    if (samplesX < 2 || samplesZ < 2)
        return "the heightmap's grid is not set: set its Samples X and Samples Z";
    if (samplesX > kPageStoreMaxSamplesPerAxis || samplesZ > kPageStoreMaxSamplesPerAxis)
        return "the heightmap is " + std::to_string(samplesX) + " x " + std::to_string(samplesZ) +
               " samples, more than " + std::to_string(kPageStoreMaxSamplesPerAxis) +
               " along an axis: split it into terrains of at most that many samples a side";
    if (static_cast<uint64>(samplesX) * samplesZ * sampleBytes != fileBytes)
        return "the heightmap's Samples X and Samples Z (" + std::to_string(samplesX) + " x " +
               std::to_string(samplesZ) + ") do not account for its " + std::to_string(fileBytes) +
               " bytes: set the grid it was exported with";
    out = HeightCookSource{file, format, samplesX, samplesZ};
    return {};
}

std::string ResolvePngSource(const std::filesystem::path& file, HeightCookSource& out)
{
    Terrain::PngHeightmapSize size;
    if (std::string reason = Terrain::ResolvePngHeightmapSize(file, size); !reason.empty())
        return reason;
    if (size.Width < 2 || size.Height < 2)
        return "the PNG " + file.string() + " is smaller than 2 x 2 samples";
    if (size.Width > kPageStoreMaxSamplesPerAxis || size.Height > kPageStoreMaxSamplesPerAxis)
        return "the PNG " + file.string() + " is wider than " + std::to_string(kPageStoreMaxSamplesPerAxis) +
               " samples along an axis";
    out = HeightCookSource{file, HeightSourceFormat::Png16, size.Width, size.Height};
    return {};
}

// The running range of a scan's float samples; false at a sample that is not a finite number.
bool AccumulateRange(const uint8* bytes, std::size_t count, float32& lowest, float32& highest)
{
    const auto* samples = reinterpret_cast<const float32*>(bytes);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(samples[i]))
            return false;
        lowest = std::min(lowest, samples[i]);
        highest = std::max(highest, samples[i]);
    }
    return true;
}

HeightEncoding EncodingFor(const HeightCookRequest& request)
{
    if (request.Source.Format == HeightSourceFormat::R32)
        return ChooseAbsoluteHeightEncoding(request.Identity.MinHeight, request.Identity.MaxHeight);
    return NormalizedHeightEncoding();
}

PageStoreHeader MakeHeader(const HeightCookRequest& request)
{
    const HeightEncoding encoding = EncodingFor(request);
    PageStoreHeader header;
    header.Format = encoding.Format;
    header.Filter = PageFieldFilter::HeightTent;
    header.Units = request.Source.Format == HeightSourceFormat::R32 ? PageHeightUnits::Absolute
                                                                    : PageHeightUnits::Normalized;
    header.FaceCount = 1;
    header.SamplesX = request.Source.SamplesX;
    header.SamplesZ = request.Source.SamplesZ;
    header.Quantum = encoding.Quantum;
    header.Key = request.Key;
    return header;
}

// A previous store whose pages a cook can patch: same grid, encoding and units.
bool CanPatch(const PageStoreHeader& previous, const PageStoreHeader& next)
{
    return previous.Format == next.Format && previous.Filter == next.Filter && previous.Units == next.Units &&
           previous.FaceCount == next.FaceCount && previous.SamplesX == next.SamplesX &&
           previous.SamplesZ == next.SamplesZ && previous.Quantum.Offset == next.Quantum.Offset &&
           previous.Quantum.Step == next.Quantum.Step;
}

std::string StartStore(const HeightCookRequest& request, const PageStoreHeader& header, PageStoreWriter& writer,
                       bool& outPatching)
{
    outPatching = false;
    std::error_code ec;
    if (!request.Previous.empty() && request.Previous != request.Output && std::filesystem::exists(request.Previous, ec))
    {
        PageStoreLayout previousLayout;
        {
            PageStoreReader previous;
            if (previous.Open(request.Previous).empty())
                previousLayout = previous.Layout();
        }
        if (!previousLayout.Levels.empty() && CanPatch(previousLayout.Header, header))
        {
            previousLayout.Header.Key = header.Key;
            outPatching = true;
            return writer.OpenForPatch(request.Previous, request.Output, std::move(previousLayout));
        }
    }
    return writer.Create(request.Output, MakePageStoreLayout(header));
}

bool Cancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load();
}

} // namespace

std::string ResolveHeightCookSource(const std::filesystem::path& file, uint32 samplesX, uint32 samplesZ,
                                    HeightCookSource& out)
{
    const std::string ext = LowerExtension(file);
    if (ext == ".r32")
        return ResolveRawSource(file, HeightSourceFormat::R32, samplesX, samplesZ, out);
    if (ext == ".r16")
        return ResolveRawSource(file, HeightSourceFormat::R16, samplesX, samplesZ, out);
    if (ext == ".png")
        return ResolvePngSource(file, out);
    if (ext == ".tif" || ext == ".tiff")
        return "GeoTIFF heightmaps are not read: export the DEM as .r32 (32-bit float heights in meters, rows "
               "from the smallest Z, nodata filled) and set its Samples X and Samples Z";
    return "'" + ext + "' is not a heightmap format: use .r32, .r16 or a 16-bit PNG";
}

std::string ScanHeightSource(const HeightCookSource& source, AssetIOService* io, HeightSourceIdentity& out,
                             const std::atomic<bool>* cancel, HeightCookProgress* progress)
{
    std::error_code ec;
    const uint64 size = std::filesystem::file_size(source.File, ec);
    if (ec)
        return "the heightmap " + source.File.string() + " could not be read";

    HeightSourceIdentity identity;
    identity.ContentHash = Hashing::kFnv1a64OffsetBasis;
    float32 lowest = std::numeric_limits<float32>::max();
    float32 highest = std::numeric_limits<float32>::lowest();
    const bool ranged = source.Format == HeightSourceFormat::R32;

    // One chunk is read ahead while the one before it is hashed.
    SourceRangeReader reader(source.File, io);
    std::vector<uint8> chunk;
    uint64 offset = 0;
    if (size > 0)
        reader.Request(0, std::min<uint64>(kScanChunkBytes, size));
    while (offset < size)
    {
        if (!reader.Take(chunk))
            return "the heightmap " + source.File.string() + " could not be read to its end";
        offset += chunk.size();
        if (offset < size)
            reader.Request(offset, std::min<uint64>(kScanChunkBytes, size - offset));
        if (Cancelled(cancel))
        {
            if (offset < size)
                reader.Take(chunk); // never leave a read outstanding past the reader
            return "the cook was cancelled";
        }
        identity.ContentHash = Hashing::Fnv1a64(chunk.data(), chunk.size(), identity.ContentHash);
        if (progress)
            progress->RowsDone.store(offset * source.SamplesZ / size);
        if (ranged && !AccumulateRange(chunk.data(), chunk.size() / 4u, lowest, highest))
        {
            if (offset < size)
                reader.Take(chunk);
            return "the heightmap has samples that are not numbers (nodata): fill them before export";
        }
    }
    if (ranged)
    {
        identity.MinHeight = lowest;
        identity.MaxHeight = highest;
    }
    out = identity;
    return {};
}

HeightCookResult CookHeightPageStore(const HeightCookRequest& request)
{
    HeightCookResult result;
    HeightSourceRows rows;
    if (result.Error = rows.Open(request.Source, request.Io); !result.Error.empty())
        return result;

    const PageStoreHeader header = MakeHeader(request);
    PageStoreWriter writer;
    if (result.Error = StartStore(request, header, writer, result.Patched); !result.Error.empty())
        return result;

    // Each band is read while the band before it builds the pyramid.
    HeightPyramidBuilder builder(writer, result.Patched, request.Pool);
    const uint32 height = request.Source.SamplesZ;
    std::vector<float32> band(static_cast<std::size_t>(request.Source.SamplesX) * kCookBandRows);
    rows.RequestRows(0, std::min(kCookBandRows, height));
    bool readOk = true;
    for (uint32 row = 0; row < height;)
    {
        const uint32 count = std::min(kCookBandRows, height - row);
        readOk = rows.TakeRows(band.data());
        if (!readOk)
            break;
        const uint32 nextRow = row + count;
        if (nextRow < height)
            rows.RequestRows(nextRow, std::min(kCookBandRows, height - nextRow));
        if (Cancelled(request.Cancel))
        {
            if (nextRow < height)
                rows.TakeRows(band.data()); // never leave a read outstanding past the reader
            writer.Abandon();
            result.Error = "the cook was cancelled";
            return result;
        }
        builder.PushLevel0Rows(band.data(), count);
        row = nextRow;
        if (request.Progress)
            request.Progress->RowsDone.store(static_cast<uint64>(height) + row);
    }
    if (!readOk || !builder.Finish())
    {
        writer.Abandon();
        result.Error = readOk ? "the page store could not be written (is the disk full?)"
                              : "the heightmap " + request.Source.File.string() + " could not be read to its end";
        return result;
    }
    result.StoreBytes = writer.Layout().TotalBytes();
    result.PagesWritten = builder.PagesWritten();
    result.PagesUnchanged = builder.PagesUnchanged();
    result.Error = writer.Finish();
    return result;
}

} // namespace GameEngine::PageStreaming
