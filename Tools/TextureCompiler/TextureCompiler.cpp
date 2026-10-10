// TextureCompiler — offline PNG/TGA/JPG -> KTX2 encoder.
//
// Produces UASTC 4x4 KTX2 with a full mip chain and Zstd supercompression —
// the engine's universal texture container. At load the engine transcodes to
// the device-appropriate GPU format (BC7 on desktop; RGBA32 fallback), so one
// encoded asset serves every backend. Color space is written into the KTX2
// DFD (sRGB vs linear transfer), which the engine treats as authoritative.
//
// Usage:
//   TextureCompiler <input> <output.ktx2> (--srgb | --linear)
//                   [--normal-map] [--uastc-quality 0..4] [--zstd 1..22]
//                   [--no-mips] [--uncompressed] [--threads N]
//
//   --srgb / --linear   Required: transfer function written to the KTX2 DFD.
//                       Albedo/emissive are sRGB; normal/MR/AO/masks are linear.
//   --normal-map        Renormalize XYZ after each mip downsample (implies --linear).
//   --uastc-quality     UASTC encode level (default 2 = KTX_PACK_UASTC_LEVEL_DEFAULT).
//   --zstd              Zstd supercompression level (default 18).
//   --no-mips           Encode only the base level.
//   --uncompressed      Keep raw RGBA8 levels (no UASTC). For content block
//                       compression visibly degrades (smooth gradient swatches);
//                       Zstd still applies on disk.
//   --threads           Encoder threads (default: hardware concurrency).
//
// Verification mode (quality guards / tooling, not a runtime path):
//   TextureCompiler --decode <input.ktx2> <output.png> [--level N]
//   Inflates + transcodes the KTX2 back to RGBA8 (UASTC payloads via the same
//   libktx transcode family the engine uses) and writes one mip level as PNG.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct Options
{
    std::string InputPath;
    std::string OutputPath;
    bool SrgbSet = false;
    bool Srgb = false;
    bool NormalMap = false;
    bool GenerateMips = true;
    bool Uncompressed = false;
    uint32_t UastcQuality = KTX_PACK_UASTC_LEVEL_DEFAULT;
    uint32_t ZstdLevel = 18;
    uint32_t Threads = 0;
};

void PrintUsage()
{
    std::fprintf(stderr,
                 "Usage: TextureCompiler <input> <output.ktx2> (--srgb | --linear)\n"
                 "                       [--normal-map] [--uastc-quality 0..4] [--zstd 1..22]\n"
                 "                       [--no-mips] [--uncompressed] [--threads N]\n"
                 "       TextureCompiler --decode <input.ktx2> <output.png> [--level N]\n");
}

// Verification path: KTX2 -> RGBA8 -> PNG for one mip level. UASTC payloads
// transcode through the same libktx family the engine's loader uses, so the
// output is what a runtime without BC support would sample.
int DecodeKtx2ToPng(const std::string& inPath, const std::string& outPath, uint32_t level)
{
    ktxTexture2* tex = nullptr;
    KTX_error_code err = ktxTexture2_CreateFromNamedFile(inPath.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
    if (err != KTX_SUCCESS)
    {
        std::fprintf(stderr, "Failed to load '%s': %s\n", inPath.c_str(), ktxErrorString(err));
        return 1;
    }
    if (ktxTexture2_NeedsTranscoding(tex))
    {
        err = ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0);
        if (err != KTX_SUCCESS)
        {
            std::fprintf(stderr, "Transcode failed: %s\n", ktxErrorString(err));
            ktxTexture_Destroy(ktxTexture(tex));
            return 1;
        }
    }
    else if (tex->vkFormat != VK_FORMAT_R8G8B8A8_SRGB && tex->vkFormat != VK_FORMAT_R8G8B8A8_UNORM)
    {
        std::fprintf(stderr, "Unsupported vkFormat %u (expected RGBA8)\n", tex->vkFormat);
        ktxTexture_Destroy(ktxTexture(tex));
        return 1;
    }
    if (level >= tex->numLevels)
    {
        std::fprintf(stderr, "Level %u out of range (%u levels)\n", level, tex->numLevels);
        ktxTexture_Destroy(ktxTexture(tex));
        return 1;
    }
    ktx_size_t offset = 0;
    err = ktxTexture_GetImageOffset(ktxTexture(tex), level, 0, 0, &offset);
    if (err != KTX_SUCCESS)
    {
        std::fprintf(stderr, "GetImageOffset failed: %s\n", ktxErrorString(err));
        ktxTexture_Destroy(ktxTexture(tex));
        return 1;
    }
    const uint8_t* data = ktxTexture_GetData(ktxTexture(tex)) + offset;
    const uint32_t w = std::max(tex->baseWidth >> level, 1u);
    const uint32_t h = std::max(tex->baseHeight >> level, 1u);
    const int ok = stbi_write_png(outPath.c_str(), static_cast<int>(w), static_cast<int>(h), 4, data,
                                  static_cast<int>(w) * 4);
    ktxTexture_Destroy(ktxTexture(tex));
    if (!ok)
    {
        std::fprintf(stderr, "Failed to write '%s'\n", outPath.c_str());
        return 1;
    }
    std::printf("%s -> %s (%ux%u, level %u)\n", inPath.c_str(), outPath.c_str(), w, h, level);
    return 0;
}

bool ParseArgs(int argc, char** argv, Options& out)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto nextValue = [&](uint32_t& dst) -> bool {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "Missing value for %s\n", arg.c_str());
                return false;
            }
            dst = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            return true;
        };
        if (arg == "--srgb")
        {
            out.SrgbSet = true;
            out.Srgb = true;
        }
        else if (arg == "--linear")
        {
            out.SrgbSet = true;
            out.Srgb = false;
        }
        else if (arg == "--normal-map")
        {
            out.NormalMap = true;
            out.SrgbSet = true;
            out.Srgb = false;
        }
        else if (arg == "--no-mips")
        {
            out.GenerateMips = false;
        }
        else if (arg == "--uncompressed")
        {
            out.Uncompressed = true;
        }
        else if (arg == "--uastc-quality")
        {
            if (!nextValue(out.UastcQuality))
                return false;
            out.UastcQuality = std::min(out.UastcQuality, static_cast<uint32_t>(KTX_PACK_UASTC_MAX_LEVEL));
        }
        else if (arg == "--zstd")
        {
            if (!nextValue(out.ZstdLevel))
                return false;
            out.ZstdLevel = std::clamp(out.ZstdLevel, 1u, 22u);
        }
        else if (arg == "--threads")
        {
            if (!nextValue(out.Threads))
                return false;
        }
        else if (arg.rfind("--", 0) == 0)
        {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        }
        else
        {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 2 || !out.SrgbSet)
        return false;
    out.InputPath = positional[0];
    out.OutputPath = positional[1];
    if (out.Threads == 0)
        out.Threads = std::max(1u, std::thread::hardware_concurrency());
    return true;
}

// One RGBA8 mip level, tightly packed.
struct MipImage
{
    std::vector<uint8_t> Pixels;
    uint32_t Width = 0;
    uint32_t Height = 0;
};

// Re-unit-length the XYZ channels after filtering; keeps W (usually unused or
// height) untouched. Filtered normals shorten, which visibly flattens relief
// at distance if left unnormalized.
void RenormalizeNormals(MipImage& mip)
{
    const size_t count = static_cast<size_t>(mip.Width) * mip.Height;
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t* px = mip.Pixels.data() + i * 4;
        float x = static_cast<float>(px[0]) / 255.0f * 2.0f - 1.0f;
        float y = static_cast<float>(px[1]) / 255.0f * 2.0f - 1.0f;
        float z = static_cast<float>(px[2]) / 255.0f * 2.0f - 1.0f;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-5f)
        {
            x /= len;
            y /= len;
            z /= len;
        }
        else
        {
            x = 0.0f;
            y = 0.0f;
            z = 1.0f;
        }
        auto toByte = [](float v) {
            return static_cast<uint8_t>(std::clamp((v * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        px[0] = toByte(x);
        px[1] = toByte(y);
        px[2] = toByte(z);
    }
}

// Build the full mip chain from the base level. sRGB content filters in linear
// space via stb_image_resize2's sRGB path; data textures filter linearly.
std::vector<MipImage> BuildMipChain(MipImage base, const Options& opt)
{
    std::vector<MipImage> chain;
    chain.push_back(std::move(base));
    if (!opt.GenerateMips)
        return chain;

    while (chain.back().Width > 1 || chain.back().Height > 1)
    {
        const MipImage& src = chain.back();
        MipImage dst;
        dst.Width = std::max(src.Width / 2u, 1u);
        dst.Height = std::max(src.Height / 2u, 1u);
        dst.Pixels.resize(static_cast<size_t>(dst.Width) * dst.Height * 4);

        const stbir_pixel_layout layout = STBIR_RGBA;
        if (opt.Srgb)
        {
            stbir_resize_uint8_srgb(src.Pixels.data(), static_cast<int>(src.Width),
                                    static_cast<int>(src.Height), 0,
                                    dst.Pixels.data(), static_cast<int>(dst.Width),
                                    static_cast<int>(dst.Height), 0, layout);
        }
        else
        {
            stbir_resize_uint8_linear(src.Pixels.data(), static_cast<int>(src.Width),
                                      static_cast<int>(src.Height), 0,
                                      dst.Pixels.data(), static_cast<int>(dst.Width),
                                      static_cast<int>(dst.Height), 0, layout);
        }
        if (opt.NormalMap)
            RenormalizeNormals(dst);
        chain.push_back(std::move(dst));
    }
    return chain;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "--decode")
    {
        uint32_t level = 0;
        for (int i = 4; i + 1 < argc; ++i)
        {
            if (std::string(argv[i]) == "--level")
                level = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        }
        return DecodeKtx2ToPng(argv[2], argv[3], level);
    }

    Options opt;
    if (!ParseArgs(argc, argv, opt))
    {
        PrintUsage();
        return 2;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(opt.InputPath.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        std::fprintf(stderr, "Failed to decode '%s': %s\n", opt.InputPath.c_str(),
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 1;
    }

    MipImage baseMip;
    baseMip.Width = static_cast<uint32_t>(width);
    baseMip.Height = static_cast<uint32_t>(height);
    baseMip.Pixels.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
    stbi_image_free(decoded);

    const std::vector<MipImage> chain = BuildMipChain(std::move(baseMip), opt);

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = opt.Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    createInfo.baseWidth = static_cast<ktx_uint32_t>(width);
    createInfo.baseHeight = static_cast<ktx_uint32_t>(height);
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = static_cast<ktx_uint32_t>(chain.size());
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    KTX_error_code err = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (err != KTX_SUCCESS)
    {
        std::fprintf(stderr, "ktxTexture2_Create failed: %s\n", ktxErrorString(err));
        return 1;
    }

    for (size_t level = 0; level < chain.size(); ++level)
    {
        err = ktxTexture_SetImageFromMemory(ktxTexture(texture), static_cast<ktx_uint32_t>(level), 0, 0,
                                            chain[level].Pixels.data(), chain[level].Pixels.size());
        if (err != KTX_SUCCESS)
        {
            std::fprintf(stderr, "SetImageFromMemory (level %zu) failed: %s\n", level, ktxErrorString(err));
            ktxTexture_Destroy(ktxTexture(texture));
            return 1;
        }
    }

    if (!opt.Uncompressed)
    {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = opt.Threads;
        params.uastcFlags = opt.UastcQuality & KTX_PACK_UASTC_LEVEL_MASK;

        err = ktxTexture2_CompressBasisEx(texture, &params);
        if (err != KTX_SUCCESS)
        {
            std::fprintf(stderr, "UASTC encode failed: %s\n", ktxErrorString(err));
            ktxTexture_Destroy(ktxTexture(texture));
            return 1;
        }
    }

    err = ktxTexture2_DeflateZstd(texture, opt.ZstdLevel);
    if (err != KTX_SUCCESS)
    {
        std::fprintf(stderr, "Zstd supercompression failed: %s\n", ktxErrorString(err));
        ktxTexture_Destroy(ktxTexture(texture));
        return 1;
    }

    err = ktxTexture_WriteToNamedFile(ktxTexture(texture), opt.OutputPath.c_str());
    ktxTexture_Destroy(ktxTexture(texture));
    if (err != KTX_SUCCESS)
    {
        std::fprintf(stderr, "Failed to write '%s': %s\n", opt.OutputPath.c_str(), ktxErrorString(err));
        return 1;
    }

    char encodeDesc[32];
    if (opt.Uncompressed)
        std::snprintf(encodeDesc, sizeof(encodeDesc), "RGBA8");
    else
        std::snprintf(encodeDesc, sizeof(encodeDesc), "UASTC q%u", opt.UastcQuality);
    std::printf("%s -> %s (%ux%u, %zu mips, %s, %s, zstd %u)\n",
                opt.InputPath.c_str(), opt.OutputPath.c_str(),
                static_cast<uint32_t>(width), static_cast<uint32_t>(height), chain.size(),
                opt.Srgb ? "sRGB" : "linear", encodeDesc, opt.ZstdLevel);
    return 0;
}
