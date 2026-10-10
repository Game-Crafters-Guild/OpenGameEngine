#include "HeadlessViewFixture.h"
#include "ScopedPipelineFrame.h"
#include "WorldOnlyPipeline.h"

#include "Engine/Rendering/ViewFinalize.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Passes/PixelPerfectUpscalePass.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPlatform.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace GameEngine::Testing
{
namespace RG = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::TextureDesc;
using ::GameEngine::Rendering::TextureFormat;
using ::GameEngine::Rendering::TextureUsage;

namespace
{

// The finalize's source: linear light at more precision than the destination
// step, so the sub-LSB information the three hypotheses are separated on
// survives the tap.
constexpr TextureFormat kViewColorFormat = TextureFormat::R16G16B16A16_FLOAT;
constexpr TextureFormat kViewDepthFormat = TextureFormat::D32_FLOAT;

// The .shaderpkg the finalize needs. AddSRGBEncodePassRG declines silently
// (one warning) when it cannot be loaded, so an unstaged package would show up
// as "the pass declared nothing" rather than as a missing file — the fixture
// names the file instead.
constexpr const char* kEncodeShaderPackage = "Shaders/encode_srgb.shaderpkg";

// The .shaderpkg the transport needs. AddPixelPerfectUpscalePassRG returns an
// invalid pass when it cannot be loaded, which without this check would surface
// as "the transport declared nothing" rather than as a missing file.
constexpr const char* kUpscaleShaderPackage = "Shaders/pixelperfect_upscale.shaderpkg";

// The SDF shaders the HUD needs. The base pair is load-or-throw inside
// UIManager, but the ENCODED fragment variant is deliberately optional there:
// an EncodedSrgb declaration without it draws NOTHING (UIManager_RenderRG.cpp
// skips the draws rather than borrowing a wrong-space pipeline). In a test
// that absence must be a named staging failure up front, not a HUD that
// silently never lands.
constexpr const char* kUiSdfVertShader = "Shaders/ui_sdf.vert.spv";
constexpr const char* kUiSdfFragShader = "Shaders/ui_sdf.frag.spv";
constexpr const char* kUiSdfEncodedFragShader = "Shaders/ui_sdf_encoded.frag.spv";

// Fixed content scale 1: CSS logical px == destination physical px, so the
// tests' rects are statements about destination pixels with no mapping between.
struct HudPlatform final : UI::IPlatformApi
{
    std::string GetClipboardText() const override { return m_Clipboard; }
    void SetClipboardText(const char* utf8) override { m_Clipboard = utf8 ? utf8 : ""; }
    float GetContentScale() const override { return 1.0f; }
    std::string m_Clipboard;
};

float HalfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    const std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exp == 0)
    {
        if (mant != 0)
        {
            int e = -1;
            std::uint32_t m = mant;
            do
            {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
        else
        {
            bits = sign;
        }
    }
    else if (exp == 0x1Fu)
    {
        bits = sign | 0x7F800000u | (mant << 13);
    }
    else
    {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::uint16_t FloatToHalf(float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    std::int32_t exp = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mant = bits & 0x7FFFFFu;
    if (exp >= 0x1F)
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    if (exp <= 0)
    {
        if (exp < -10)
            return static_cast<std::uint16_t>(sign);
        mant |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        const std::uint32_t half = (mant + (1u << (shift - 1))) >> shift;
        return static_cast<std::uint16_t>(sign | half);
    }
    const std::uint32_t rounded = mant + 0x1000u;
    if (rounded & 0x800000u)
    {
        ++exp;
        if (exp >= 0x1F)
            return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) |
                                      ((rounded >> 13) & 0x3FFu));
}

// Channel `ch` of pixel `px`, decoded to ENCODED [0,1] — the unit every
// statistic in this file reports in. A quantizing destination's code is divided
// by its own full scale; a float destination already holds the encoded value.
// Returns false for a format this fixture does not unpack.
bool ReadDestinationEncoded(const ViewOutputBytes& handoff, std::size_t px, int ch, float& out)
{
    switch (handoff.Format)
    {
    case TextureFormat::RGB10A2_UNORM:
    {
        // A2B10G10R10_UNORM_PACK32: R occupies the low 10 bits.
        std::uint32_t packed = 0;
        std::memcpy(&packed, &handoff.Bytes[px * 4], sizeof(packed));
        const auto code = (packed >> (10 * static_cast<std::uint32_t>(ch))) & 0x3FFu;
        out = static_cast<float>(code) / 1023.0f;
        return true;
    }
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
        out = static_cast<float>(handoff.Bytes[px * 4 + static_cast<std::size_t>(ch)]) / 255.0f;
        return true;
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
    {
        static constexpr int kBgra[3] = {2, 1, 0};
        out = static_cast<float>(handoff.Bytes[px * 4 + static_cast<std::size_t>(kBgra[ch])]) /
              255.0f;
        return true;
    }
    case TextureFormat::R16G16B16A16_FLOAT:
    {
        // The host policy's own destination. It stores the encode's output
        // verbatim — no code lattice, so no rounding term stands between the
        // dither and this reading.
        std::uint16_t half = 0;
        std::memcpy(&half, &handoff.Bytes[(px * 4 + static_cast<std::size_t>(ch)) * 2],
                    sizeof(half));
        out = HalfToFloat(half);
        return true;
    }
    default:
        return false;
    }
}

// The encode's output rounded onto the destination's code lattice exactly as
// the ROP does, in encoded units. A float destination has no lattice and is
// stored as computed.
float QuantizeToDestination(float encoded, float codeScale)
{
    if (codeScale <= 0.0f)
        return encoded;
    return std::round(encoded * codeScale) / codeScale;
}

// Whether the last frame left operands a statistic can be computed from. The
// format test IS the decoder, so the set of measurable destinations can never
// drift from the switch above.
bool CanMeasure(const ViewOutputBytes& handoff, const ViewStageTap& source)
{
    if (source.Rgba.empty() || handoff.Bytes.empty())
        return false;
    if (source.Width != handoff.Width || source.Height != handoff.Height)
        return false;
    float probe = 0.0f;
    return ReadDestinationEncoded(handoff, 0, 0, probe);
}

} // namespace

const char* ToString(HeadlessViewStatus status)
{
    switch (status)
    {
    case HeadlessViewStatus::Ok:
        return "Ok";
    case HeadlessViewStatus::NoDevice:
        return "NoDevice";
    case HeadlessViewStatus::StagedAssetMissing:
        return "StagedAssetMissing";
    case HeadlessViewStatus::FormatUnsupported:
        return "FormatUnsupported";
    }
    return "?";
}

float LinearToSrgbRef(float c)
{
    if (c <= 0.0031308f)
        return 12.92f * c;
    return 1.055f * std::pow(std::max(c, 1e-6f), 1.0f / 2.4f) - 0.055f;
}

float TriangularDitherRef(std::uint32_t x, std::uint32_t y)
{
    // gl_FragCoord.xy at a fully covered pixel is its CENTRE.
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float inner = 0.06711056f * px + 0.00583715f * py;
    const float frac1 = inner - std::floor(inner);
    const float outer = 52.9829189f * frac1;
    const float u = outer - std::floor(outer);
    const float o = 2.0f * u - 1.0f;
    const float sign = o < 0.0f ? -1.0f : (o > 0.0f ? 1.0f : 0.0f);
    return sign * (1.0f - std::sqrt(std::max(0.0f, 1.0f - std::abs(o))));
}

float DestinationCodeScale(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGB10A2_UNORM:
        return 1023.0f;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
        return 255.0f;
    default:
        return 0.0f;
    }
}

bool DestinationCodes(const ViewOutputBytes& image, std::size_t pixelIndex, std::uint32_t rgba[4])
{
    switch (image.Format)
    {
    case TextureFormat::RGB10A2_UNORM:
    {
        if ((pixelIndex + 1) * 4 > image.Bytes.size())
            return false;
        // A2B10G10R10_UNORM_PACK32: R in the low 10 bits, A in the top 2.
        std::uint32_t packed = 0;
        std::memcpy(&packed, &image.Bytes[pixelIndex * 4], sizeof(packed));
        rgba[0] = packed & 0x3FFu;
        rgba[1] = (packed >> 10) & 0x3FFu;
        rgba[2] = (packed >> 20) & 0x3FFu;
        rgba[3] = (packed >> 30) & 0x3u;
        return true;
    }
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
    {
        if ((pixelIndex + 1) * 4 > image.Bytes.size())
            return false;
        for (int ch = 0; ch < 4; ++ch)
            rgba[ch] = image.Bytes[pixelIndex * 4 + static_cast<std::size_t>(ch)];
        return true;
    }
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
    {
        if ((pixelIndex + 1) * 4 > image.Bytes.size())
            return false;
        static constexpr int kBgra[4] = {2, 1, 0, 3};
        for (int ch = 0; ch < 4; ++ch)
            rgba[ch] = image.Bytes[pixelIndex * 4 + static_cast<std::size_t>(kBgra[ch])];
        return true;
    }
    case TextureFormat::R16G16B16A16_FLOAT:
    {
        // The raw half pattern, not its decoded value: a float destination's
        // "code" is the bit pattern it stores, and that is what a byte gate
        // over an F16 hand-off is comparing.
        if ((pixelIndex + 1) * 8 > image.Bytes.size())
            return false;
        for (int ch = 0; ch < 4; ++ch)
        {
            std::uint16_t half = 0;
            std::memcpy(&half, &image.Bytes[(pixelIndex * 4 + static_cast<std::size_t>(ch)) * 2],
                        sizeof(half));
            rgba[ch] = half;
        }
        return true;
    }
    default:
        return false;
    }
}

bool ReadEncodedChannel(const ViewOutputBytes& image, std::size_t pixelIndex, int channel,
                        float& outEncoded)
{
    // Colour channels only: RGB10A2's alpha holds 2 bits and the decoder's
    // uniform /1023 would misreport it.
    if (channel < 0 || channel > 2)
        return false;
    const std::size_t bpp = image.Format == TextureFormat::R16G16B16A16_FLOAT ? 8u : 4u;
    if ((pixelIndex + 1) * bpp > image.Bytes.size())
        return false;
    return ReadDestinationEncoded(image, pixelIndex, channel, outEncoded);
}

std::uint32_t DestinationFullScaleCode(TextureFormat format, int channel)
{
    if (channel < 0 || channel > 3)
        return 0;
    switch (format)
    {
    case TextureFormat::RGB10A2_UNORM:
        return channel == 3 ? 3u : 1023u;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
        return 255u;
    case TextureFormat::R16G16B16A16_FLOAT:
        return 0x3C00u; // half(1.0)
    default:
        return 0;
    }
}

TransportGeometry BuildTransportIndexMap(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                                         std::uint32_t destinationWidth,
                                         std::uint32_t destinationHeight, std::uint32_t zoom,
                                         float fracX, float fracY,
                                         std::vector<TransportIndex>& outMap)
{
    TransportGeometry g{};
    g.SourceWidth = sourceWidth;
    g.SourceHeight = sourceHeight;
    g.DestinationWidth = destinationWidth;
    g.DestinationHeight = destinationHeight;
    g.Zoom = std::max(1u, zoom);
    g.FracX = fracX;
    g.FracY = fracY;
    outMap.clear();
    if (sourceWidth < 3 || sourceHeight < 3 || destinationWidth == 0 || destinationHeight == 0)
        return g;

    // The pass's padded-source contract: one texel of border per side.
    g.ReferenceWidth = sourceWidth - 2;
    g.ReferenceHeight = sourceHeight - 2;

    const float zoomF = static_cast<float>(g.Zoom);
    const float outW = static_cast<float>(g.ReferenceWidth) * zoomF;
    const float outH = static_cast<float>(g.ReferenceHeight) * zoomF;
    const float originX = std::floor((static_cast<float>(destinationWidth) - outW) * 0.5f);
    const float originY = std::floor((static_cast<float>(destinationHeight) - outH) * 0.5f);
    g.OutputOriginX = static_cast<std::uint32_t>(std::max(0.0f, originX));
    g.OutputOriginY = static_cast<std::uint32_t>(std::max(0.0f, originY));

    // Sample-window origin: the one-texel border moved by the sub-texel
    // remainder in the camera's direction (+X right, +Y up against the rows).
    const float sampleOriginX = 1.0f + fracX;
    const float sampleOriginY = 1.0f - fracY;

    float minBoundary = 1.0f;
    float minCentre = 1.0f;
    outMap.resize(static_cast<std::size_t>(destinationWidth) * destinationHeight);
    for (std::uint32_t dy = 0; dy < destinationHeight; ++dy)
    {
        for (std::uint32_t dx = 0; dx < destinationWidth; ++dx)
        {
            TransportIndex& e = outMap[static_cast<std::size_t>(dy) * destinationWidth + dx];
            // gl_FragCoord.xy at a fully covered pixel is its CENTRE.
            const float localX = static_cast<float>(dx) + 0.5f - originX;
            const float localY = static_cast<float>(dy) + 0.5f - originY;
            if (localX < 0.0f || localY < 0.0f || localX >= outW || localY >= outH)
            {
                e.Letterbox = true;
                ++g.LetterboxPixels;
                continue;
            }
            e.Letterbox = false;
            e.SampleX = sampleOriginX + localX / zoomF;
            e.SampleY = sampleOriginY + localY / zoomF;

            const float axis[2] = {e.SampleX, e.SampleY};
            for (const float s : axis)
            {
                const float frac = s - std::floor(s);
                minBoundary = std::min(minBoundary, std::min(frac, 1.0f - frac));
                minCentre = std::min(minCentre, std::abs(frac - 0.5f));
            }

            const float snappedX = std::floor(e.SampleX);
            const float snappedY = std::floor(e.SampleY);
            const float clampedX =
                std::clamp(snappedX, 0.0f, static_cast<float>(sourceWidth) - 1.0f);
            const float clampedY =
                std::clamp(snappedY, 0.0f, static_cast<float>(sourceHeight) - 1.0f);
            e.Clamped = clampedX != snappedX || clampedY != snappedY;
            if (e.Clamped)
                ++g.ClampedSamples;
            e.SourceX = static_cast<std::uint32_t>(clampedX);
            e.SourceY = static_cast<std::uint32_t>(clampedY);
            ++g.MappedPixels;
        }
    }
    if (g.MappedPixels > 0)
    {
        g.MinDistanceToTexelBoundary = minBoundary;
        g.MinDistanceToTexelCentre = minCentre;
    }
    return g;
}

std::uint32_t HeadlessViewFixture::DestinationUsageFlags()
{
    return static_cast<std::uint32_t>(TextureUsage::RenderTarget) |
           static_cast<std::uint32_t>(TextureUsage::TransferSrc);
}

std::uint32_t HeadlessViewFixture::TransportSourceUsageFlags()
{
    return DestinationUsageFlags() | static_cast<std::uint32_t>(TextureUsage::ShaderResource);
}

struct HeadlessViewFixture::Pools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Ring;
    explicit Pools(Rendering::IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

HeadlessViewFixture::HeadlessViewFixture() = default;

HeadlessViewFixture::~HeadlessViewFixture()
{
    Down();
}

HeadlessViewStatus HeadlessViewFixture::Up()
{
    m_StatusMessage.clear();

    Rendering::DeviceDesc dd{};
    dd.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    dd.enableDebugLayer = false;
    dd.enableDynamicRendering = true;
    auto device = Rendering::DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
    {
        m_StatusMessage = "no Vulkan device could be created or initialized";
        return HeadlessViewStatus::NoDevice;
    }
    m_Device = std::move(device);

    if (Rendering::Utils::ResolveShaderPath(kEncodeShaderPackage).empty())
    {
        m_StatusMessage = std::string("staged asset missing: ") + kEncodeShaderPackage +
                          " (the finalize pass declines silently without it)";
        return HeadlessViewStatus::StagedAssetMissing;
    }

    m_Services = std::make_unique<Engine::Renderer::RenderServices>();
    if (!m_Services->Initialize(m_Device.get()))
    {
        m_Services.reset();
        m_StatusMessage = "RenderServices::Initialize failed on a device that reported ready";
        return HeadlessViewStatus::NoDevice;
    }
    Testing::PinWorldOnlyPipeline(m_Services->Spine());
    m_Pools = std::make_unique<Pools>(m_Device.get());

    m_Camera = m_Services->Views().AllocateCamera("HeadlessView.Cam");
    Rendering::CameraData cd{};
    for (int i = 0; i < 16; i += 5)
    {
        cd.view[i] = 1.0f;
        cd.proj[i] = 1.0f;
        cd.viewProj[i] = 1.0f;
    }
    m_Services->Views().SetCameraData(m_Camera, cd);
    m_View = m_Services->Views().AllocateView("HeadlessView", m_Camera);
    m_Services->Views().SetViewRenderLayerMask(m_View, 1u);

    // One instance so the GPU-driven prologue does not vacuously early-out —
    // the spine's culling and bucketer stages are part of "a real world view".
    if (Rendering::GPUScene* scene = m_Services->GetGPUScene())
    {
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        scene->AddInstance(inst);
    }
    return HeadlessViewStatus::Ok;
}

void HeadlessViewFixture::Down()
{
    // The manager holds device resources (SDF rings, pipelines); it must go
    // before the device it was built over.
    m_Hud.reset();
    m_HudPlatform.reset();
    if (m_Services)
    {
        m_Services->Shutdown();
        m_Services.reset();
    }
    m_Pools.reset();
    if (m_Device)
    {
        m_Device->Shutdown();
        m_Device.reset();
    }
}

HeadlessViewStatus HeadlessViewFixture::StandUpHud(const HeadlessViewDesc& desc)
{
    // UIManager treats the encoded fragment variant as optional — an
    // EncodedSrgb declaration without it draws NOTHING (RenderRG skips the
    // draws rather than borrowing a wrong-space pipeline). Downstream of this
    // fixture that absence would read as "the HUD never landed", so it is
    // named here as the staging failure it is.
    for (const char* shader : {kUiSdfVertShader, kUiSdfFragShader, kUiSdfEncodedFragShader})
    {
        if (Rendering::Utils::ResolveShaderPath(shader).empty())
        {
            m_StatusMessage = std::string("staged asset missing: ") + shader +
                              " (an encoded-target HUD declare draws nothing without it)";
            return HeadlessViewStatus::StagedAssetMissing;
        }
    }

    m_HudPlatform = std::make_unique<HudPlatform>();
    m_Hud = std::make_unique<UIManager>(m_Device.get());
    m_Hud->SetPlatform(m_HudPlatform.get());
    // CSS logical px == destination physical px (content scale 1).
    m_Hud->SetLayoutSizeOverride(desc.Width, desc.Height);

    std::unique_ptr<UIElement> root;
    if (!UIParsing::XMLParser::ParseLayoutFromString(desc.Hud->Xml, root,
                                                     "HeadlessViewFixture.hud.xml") ||
        !root)
    {
        m_StatusMessage = "HUD XML parse failed";
        return HeadlessViewStatus::StagedAssetMissing;
    }
    m_Hud->SetRoot(std::move(root));

    Stylesheet sheet{};
    if (!UIParsing::CSSParser::ParseStylesFromString(desc.Hud->Css, sheet))
    {
        m_StatusMessage = "HUD CSS parse failed";
        return HeadlessViewStatus::StagedAssetMissing;
    }
    sheet.SourceName = "HeadlessViewFixture.hud.css";
    m_Hud->AddStylesheet(std::make_shared<const Stylesheet>(std::move(sheet)));

    // Style resolution, Yoga and primitive generation settle over successive
    // frames (deferred relayout lands a frame late), so the declare in the
    // real frame emits a fixed-point layout. Declare-only frames over
    // throwaway pools — the UI harness settle pattern; the settle target is
    // never realized, so its space is irrelevant to what settles.
    RG::RGResourcePool settlePersistent(m_Device.get());
    RG::RGTransientPool settleTransient(m_Device.get());
    RG::RGUploadRing settleRing(m_Device.get(), 2, 262144);
    for (int i = 0; i < 4; ++i)
    {
        m_Hud->Update(0.016f, /*interactive=*/true);
        RG::RGFrame settle(m_Device.get(), &settlePersistent, &settleTransient, &settleRing);
        settle.BeginFrame(static_cast<std::uint64_t>(i) + 1);
        TextureDesc td{};
        td.width = desc.Width;
        td.height = desc.Height;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(kViewColorFormat);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource);
        td.debugName = "HeadlessView.HudSettle";
        const RG::RGTexture target = settle.CreateTexture("HeadlessView.HudSettle", td);
        m_Hud->RenderRG(settle, target, UI::UITargetSpace::EncodedSrgb());
    }
    return HeadlessViewStatus::Ok;
}

HeadlessViewStatus HeadlessViewFixture::Render(const HeadlessViewDesc& desc)
{
    m_StatusMessage.clear();
    m_Handoff = {};
    m_PreHud = {};
    m_HudFacts = {};
    m_Hud.reset();
    m_Source = {};
    m_Facts = {};
    m_Transport = {};
    m_TransportIndex.clear();
    m_TransportShape = {};
    m_PassNames.clear();
    m_ScheduledPassNames.clear();

    if (!m_Device || !m_Services)
    {
        m_StatusMessage = "Render() before a successful Up()";
        return HeadlessViewStatus::NoDevice;
    }

    const bool hostPolicy = desc.Driver == FinalizeDriver::HostPolicy;
    // The host policy pins the destination to the source's format; only the
    // direct arm gets to name one, so only it has a capability to probe. A
    // transport stage additionally SAMPLES that destination, which is a
    // capability of its own.
    const TextureFormat probeFormat = hostPolicy ? kViewColorFormat : desc.DestinationFormat;
    const std::uint32_t probeUsage =
        desc.Transport ? TransportSourceUsageFlags() : DestinationUsageFlags();
    if (!m_Device->IsTextureFormatSupported(probeFormat, probeUsage))
    {
        m_StatusMessage = std::string("format capability absent on this device: ") +
                          Rendering::ToString(probeFormat) + " with usage RenderTarget|TransferSrc" +
                          (desc.Transport ? "|ShaderResource" : "");
        return HeadlessViewStatus::FormatUnsupported;
    }
    if (desc.Transport)
    {
        if (!m_Device->IsTextureFormatSupported(desc.Transport->DestinationFormat,
                                                DestinationUsageFlags()))
        {
            m_StatusMessage = std::string("format capability absent on this device: ") +
                              Rendering::ToString(desc.Transport->DestinationFormat) +
                              " with usage RenderTarget|TransferSrc (transport destination)";
            return HeadlessViewStatus::FormatUnsupported;
        }
        if (Rendering::Utils::ResolveShaderPath(kUpscaleShaderPackage).empty())
        {
            m_StatusMessage = std::string("staged asset missing: ") + kUpscaleShaderPackage +
                              " (the transport pass declines without it)";
            return HeadlessViewStatus::StagedAssetMissing;
        }
    }

    const uint32_t w = desc.Width;
    const uint32_t h = desc.Height;
    const std::size_t pixelCount = static_cast<std::size_t>(w) * h;
    if (!desc.ContentLinearRgba.empty() && desc.ContentLinearRgba.size() != pixelCount * 4)
    {
        m_StatusMessage = "ContentLinearRgba must hold Width*Height*4 floats";
        return HeadlessViewStatus::StagedAssetMissing;
    }
    if (desc.Overlay && (desc.Overlay->Width == 0 || desc.Overlay->Height == 0 ||
                         desc.Overlay->X + desc.Overlay->Width > w ||
                         desc.Overlay->Y + desc.Overlay->Height > h))
    {
        m_StatusMessage = "Overlay rect must be non-empty and inside the view";
        return HeadlessViewStatus::StagedAssetMissing;
    }
    if (desc.Hud)
    {
        const HeadlessViewStatus hudStatus = StandUpHud(desc);
        if (hudStatus != HeadlessViewStatus::Ok)
            return hudStatus;
    }

    // Content upload buffer, authored to the view colour's own format so the
    // injection is a plain transfer with no conversion of its own.
    Rendering::BufferHandle contentUpload{};
    const std::size_t contentBytes = pixelCount * 4 * sizeof(std::uint16_t);
    if (!desc.ContentLinearRgba.empty())
    {
        std::vector<std::uint16_t> halves(pixelCount * 4);
        for (std::size_t i = 0; i < halves.size(); ++i)
            halves[i] = FloatToHalf(desc.ContentLinearRgba[i]);
        contentUpload = m_Device->CreateUploadBuffer(contentBytes, "HeadlessView.Content");
        if (!contentUpload.IsValid())
        {
            m_StatusMessage = "content upload buffer creation failed";
            return HeadlessViewStatus::NoDevice;
        }
        m_Device->UpdateBuffer(contentUpload, 0, contentBytes, halves.data());
    }

    // Overlay upload, authored like the content: F16 halves, a plain transfer.
    Rendering::BufferHandle overlayUpload{};
    if (desc.Overlay)
    {
        const OverlayDesc& o = *desc.Overlay;
        const std::size_t overlayBytes =
            static_cast<std::size_t>(o.Width) * o.Height * 4 * sizeof(std::uint16_t);
        std::vector<std::uint16_t> halves(static_cast<std::size_t>(o.Width) * o.Height * 4);
        for (std::size_t px = 0; px < static_cast<std::size_t>(o.Width) * o.Height; ++px)
            for (int ch = 0; ch < 4; ++ch)
                halves[px * 4 + static_cast<std::size_t>(ch)] = FloatToHalf(o.LinearRgba[ch]);
        overlayUpload = m_Device->CreateUploadBuffer(overlayBytes, "HeadlessView.Overlay");
        if (!overlayUpload.IsValid())
        {
            m_StatusMessage = "overlay upload buffer creation failed";
            return HeadlessViewStatus::NoDevice;
        }
        m_Device->UpdateBuffer(overlayUpload, 0, overlayBytes, halves.data());
    }

    RG::RGFrame frame(m_Device.get(), &m_Pools->Persistent, &m_Pools->Transient, &m_Pools->Ring);
    ScopedPipelineFrame frameRegistration(m_Services->Spine(), frame);
    frame.BeginFrame(++m_FrameIndex);

    TextureDesc colorDesc{};
    colorDesc.width = w;
    colorDesc.height = h;
    colorDesc.depth = 1;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = 1;
    colorDesc.format = static_cast<uint32_t>(kViewColorFormat);
    // Transfer bits: the fixture uploads overlay pixels into this target
    // (CopyBufferToTextureSubresource) and reads the result back
    // (CopyTextureToBuffer). Pool-backed, so the desc is the only declaration.
    colorDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(TextureUsage::ShaderResource) |
                      static_cast<uint32_t>(TextureUsage::TransferSrc) |
                      static_cast<uint32_t>(TextureUsage::TransferDst);
    colorDesc.debugName = "HeadlessView.Color";
    TextureDesc depthDesc = colorDesc;
    depthDesc.format = static_cast<uint32_t>(kViewDepthFormat);
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil) |
                      static_cast<uint32_t>(TextureUsage::ShaderResource);
    depthDesc.debugName = "HeadlessView.Depth";

    const RG::RGTexture viewColor = frame.ImportPersistentTexture("HeadlessView.Color", colorDesc);
    const RG::RGTexture viewDepth = frame.ImportPersistentTexture("HeadlessView.Depth", depthDesc);
    if (!viewColor.IsValid() || !viewDepth.IsValid())
    {
        m_StatusMessage = "view target import failed";
        return HeadlessViewStatus::NoDevice;
    }

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    for (int i = 0; i < 4; ++i)
        clear.clearColorValue[i] = desc.ClearLinear[i];
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f; // reverse-Z far
    m_Services->Views().SetViewClearConfig(m_View, clear);

    // ── The real spine. ──
    m_Services->BeginWorldDrawFrame();
    m_Services->BuildWorldBatchKeys();
    const Engine::Renderer::Pipeline::ViewTargetsRG vt{m_View, viewColor, viewDepth, {}};
    Engine::Renderer::RenderServices::FrameGraphBuildParamsRG params{};
    params.ViewTargets = std::span<const Engine::Renderer::Pipeline::ViewTargetsRG>(&vt, 1);
    m_Services->Spine().BuildFrameGraph(frame, params);

    const auto pipelineOut = m_Services->GetPipelineOutputRG(frame, m_View);
    if (!pipelineOut.Out.IsValid())
    {
        m_StatusMessage = "the spine published no FinalColor for this view";
        return HeadlessViewStatus::NoDevice;
    }
    const UI::UITextureSpace pipelineSpace = m_Services->GetPipelineOutputSpaceRG();
    const RG::RGTexture source = pipelineOut.Out;

    // ── Content injection — the fixture's stand-in for drawn geometry. ──
    // Declared after the spine so it lands on top of the world pass's clear,
    // and read back afterwards so a gate can prove it landed rather than
    // assume the ordering.
    if (contentUpload.IsValid())
    {
        frame.AddPass(
            "HeadlessView.ContentInject", Rendering::PassPhase::kPostProcess,
            [&](RG::RGPassBuilder& p)
            {
                p.Write(source, RG::RGTextureWrite::CopyDst);
                p.PreventCulling();
            },
            [source, contentUpload, w, h](RG::RGContext& ctx)
            {
                ctx.Cmd->CopyBufferToTextureSubresource(
                    contentUpload, ctx.GetTexture(source), 0, 0, w, h, 0,
                    static_cast<std::size_t>(w) * 4 * sizeof(std::uint16_t));
            });
    }

    // ── Shape C's synthetic S1 writer (see the header block): declared after
    // the content injection and before the finalize, so its ink is upstream of
    // the OETF like any production overlay's. The WAW hazard on `source`
    // (recording order, RGSchedule.cpp) is what orders it after the injection;
    // the finalize's read is what orders it before the finalize. ──
    if (overlayUpload.IsValid())
    {
        const OverlayDesc o = *desc.Overlay;
        frame.AddPass(
            "HeadlessView.Overlay", Rendering::PassPhase::kOverlay,
            [&](RG::RGPassBuilder& p)
            {
                p.Write(source, RG::RGTextureWrite::CopyDst);
                p.PreventCulling();
            },
            [source, overlayUpload, o](RG::RGContext& ctx)
            {
                // Sub-region copy: the resting state MUST be supplied —
                // defaulted, the pre-copy transition treats the subresource as
                // discardable and every texel OUTSIDE the rect becomes
                // undefined (CommandList.h documents the trap; the boundary
                // assertions in the shape-C pin are what notice it). The graph
                // put the texture in CopyDst for this pass's write.
                ctx.Cmd->CopyBufferToTextureSubresource(
                    overlayUpload, ctx.GetTexture(source), 0, 0, o.Width, o.Height, 0,
                    static_cast<std::size_t>(o.Width) * 4 * sizeof(std::uint16_t), 1, 0, o.X, o.Y,
                    Rendering::ResourceState::CopyDest);
            });
    }

    // ── The finalize step under test. ──
    RG::RGTexture destination{};
    UI::UITextureSpace handoffSpace = pipelineSpace;
    if (hostPolicy)
    {
        // Unset PresentedFormat is the honest "no presented surface" statement
        // (Unknown), which the policy forwards and the format matrix resolves
        // through its 8-bit row — the same value a production caller with no
        // window would resolve.
        const Engine::Renderer::ViewFinalizeResult r = Engine::Renderer::DeclareViewFinalize(
            frame, source, pipelineSpace, "HeadlessView", desc.VolumeDebandThresholdLsb,
            desc.PresentedFormat.value_or(TextureFormat::Unknown), desc.HostRefusal);
        destination = r.Image;
        handoffSpace = r.Space;
        // The policy's own publish, never a restatement of it here: this arm
        // exists to gate what the host decided, and a fixture-side literal
        // would only ever gate itself.
        m_Facts.Step = r.Step;
    }
    else
    {
        TextureDesc dstDesc = colorDesc;
        dstDesc.format = static_cast<uint32_t>(desc.DestinationFormat);
        // The host policy's own finalize target is already created sampleable
        // (ViewFinalize.cpp: RenderTarget|ShaderResource), so only this arm has
        // to add the bit a transport stage needs to read it back through a
        // sampler.
        dstDesc.usage = desc.Transport ? TransportSourceUsageFlags() : DestinationUsageFlags();
        dstDesc.debugName = "HeadlessView.Destination";
        destination = frame.CreateTexture("HeadlessView.Destination", dstDesc);
        if (!destination.IsValid())
        {
            m_StatusMessage = "destination texture declaration failed";
            return HeadlessViewStatus::NoDevice;
        }
        const RG::RGPass encode = Rendering::Passes::AddSRGBEncodePassRG(
            frame, source, destination,
            {.InputSpace = desc.InputSpace,
             .Quantizer = desc.Quantizer,
             .VolumeDebandThresholdLsb = desc.VolumeDebandThresholdLsb,
             .PresentedFormat = desc.PresentedFormat},
            "HeadlessView.Finalize");
        if (!encode.IsValid())
        {
            m_StatusMessage = std::string("the finalize pass declared nothing — ") +
                              kEncodeShaderPackage + " resolved but did not load";
            return HeadlessViewStatus::StagedAssetMissing;
        }
        handoffSpace = UI::UITextureSpace::SdrFinalized();
        // Ground truth on this arm: the pass took exactly these operands one
        // statement ago (Unknown when the desc supplied no presented format —
        // the same value the pass's device fallback resolves headless).
        m_Facts.Step = Engine::Renderer::ViewFinalizeStep{
            desc.InputSpace, desc.Quantizer, desc.PresentedFormat.value_or(TextureFormat::Unknown)};
    }

    // Ground truth, read off the graph rather than off the desc: a driver that
    // pins its own destination format cannot leave the facts disagreeing with
    // the resource the pass actually wrote.
    m_Facts.DestinationFormat =
        static_cast<TextureFormat>(frame.Graph().ResourceDesc(destination.Id).Format);
    m_Facts.SourceFormat = static_cast<TextureFormat>(frame.Graph().ResourceDesc(source.Id).Format);
    // Headless every device-reading branch of the pass's arm selector is
    // unreachable on a non-backbuffer destination: the HDR-mode read is gated
    // on dstIsBackbuffer, and the manual-sRGB choice short-circuits on
    // !dstIsBackbuffer before the device is asked. That leaves one arm per
    // input space. A wrong guess here is not silent — the three-hypothesis
    // compare elects a different winner.
    //
    // No step means no pass was declared, which is the host policy having
    // declined: nothing was encoded, nothing was dithered, and predicting
    // otherwise would put a model over an image the pass never wrote.
    if (m_Facts.Step)
    {
        if (m_Facts.Step->InputSpace == Rendering::Passes::FinalizeInputSpace::EncodedSrgb)
        {
            const bool srgbRop = m_Facts.DestinationFormat == TextureFormat::RGBA8_SRGB ||
                                 m_Facts.DestinationFormat == TextureFormat::BGRA8_SRGB;
            m_Facts.PredictedOutEncoding = srgbRop ? 6 : 5;
        }
        else
        {
            m_Facts.PredictedOutEncoding = 1;
        }
        const float quantizerLsb =
            m_Facts.Step->Quantizer == Rendering::Passes::FinalizeQuantizer::None
                ? 0.0f
                : (m_Facts.Step->Quantizer == Rendering::Passes::FinalizeQuantizer::Presented
                       ? Rendering::Passes::EncodeDitherLsbForFormat(
                             desc.PresentedFormat.value_or(m_Device->GetSwapchainTextureFormat()))
                       : Rendering::Passes::EncodeDitherLsbForFormat(m_Facts.DestinationFormat));
        m_Facts.PredictedDitherLsb =
            Rendering::Passes::IsOutputDitherEnabled() ? quantizerLsb : 0.0f;
        m_Facts.PredictedDebandThresholdEncoded =
            Rendering::Passes::ResolveOutputDebandThresholdLsb(desc.VolumeDebandThresholdLsb) *
            quantizerLsb;
    }

    // ── Shape B: the real HUD composite, declared after the finalize and
    // before the transport, so a transport on the same desc samples the
    // COMPOSITE. The PreHud tap is declared FIRST: recording order is what
    // hands the scheduler its read-before-write hazard (RGSchedule.cpp derives
    // WAR edges from declaration order), so the tap observes the destination
    // between the finalize's write and the UI's — the same-frame relational
    // reference every composite expectation compares against. The tests assert
    // that schedule; nothing here trusts this comment. ──
    std::shared_ptr<Rendering::RGReadbackTicket> preHudTicket;
    if (desc.Hud)
    {
        preHudTicket = Rendering::RequestTextureReadbackRG(m_Device.get(), frame, destination,
                                                           "HeadlessView.PreHudTap");
        if (!preHudTicket)
        {
            m_StatusMessage = "pre-HUD readback declaration failed";
            return HeadlessViewStatus::NoDevice;
        }

        // The production consumption of the (image, space) pair: the space
        // operand is the frame's own hand-off stamp — the policy's published
        // one on the HostPolicy arm — run through the production mapper. A
        // fixture literal here would decouple the composite from the stamp and
        // the pairing gate could never fail.
        const UI::UITargetSpace hudSpace = UI::UITargetSpace::ForPipelineOutput(
            handoffSpace, m_Device->GetActiveHdrOutputMode());
        m_Hud->Update(0.016f, /*interactive=*/true);
        if (!m_Hud->RenderRG(frame, destination, hudSpace, RG::RGLoadOp::Load))
        {
            m_StatusMessage = "UIManager::RenderRG declined to declare the HUD pass";
            return HeadlessViewStatus::StagedAssetMissing;
        }
        m_HudFacts.TargetSpace = hudSpace;
        m_HudFacts.ResolvedOutputEncoding = m_Hud->GetLastResolvedOutputEncoding();
        m_HudFacts.TextSubpixelActive = m_Hud->GetLastTextSubpixelActive();
    }

    // ── The transport step under test (I4). Declared AFTER the finalize and
    // reading its destination, so the graph's read-after-write edge is what
    // orders the two; ScheduledPassNames() is where a gate confirms that
    // rather than assuming it. ──
    RG::RGTexture transportDst{};
    if (desc.Transport)
    {
        const TransportDesc& t = *desc.Transport;
        const RG::RGResourceDesc srcDesc = frame.Graph().ResourceDesc(destination.Id);
        if (srcDesc.Width < 3 || srcDesc.Height < 3)
        {
            m_StatusMessage = "the transport source is smaller than the pass's one-texel border "
                              "on each side, so it has no reference window at all";
            return HeadlessViewStatus::NoDevice;
        }

        TextureDesc td = colorDesc;
        td.width = t.DestinationWidth;
        td.height = t.DestinationHeight;
        td.format = static_cast<uint32_t>(t.DestinationFormat);
        td.usage = DestinationUsageFlags();
        td.debugName = "HeadlessView.Transported";
        transportDst = frame.CreateTexture("HeadlessView.Transported", td);
        if (!transportDst.IsValid())
        {
            m_StatusMessage = "transport destination texture declaration failed";
            return HeadlessViewStatus::NoDevice;
        }

        Rendering::Passes::PixelPerfectUpscaleParamsRG up{};
        up.Src = destination;
        up.Dst = transportDst;
        up.SourceWidth = srcDesc.Width;
        up.SourceHeight = srcDesc.Height;
        // The pass's padded-source contract. Derived, never taken from the
        // desc — see TransportDesc.
        up.ReferenceWidth = srcDesc.Width - 2;
        up.ReferenceHeight = srcDesc.Height - 2;
        up.Zoom = t.Zoom;
        up.FracX = t.FracX;
        up.FracY = t.FracY;
        // Inert for an EncodedSrgb input, whose one behaviour is byte
        // passthrough; it selects the linear arm's encode duality only.
        up.PassthroughLinear = false;
        const RG::RGPass transport = Rendering::Passes::AddPixelPerfectUpscalePassRG(
            frame, up, t.InputSpace, "HeadlessView.Transport");
        if (!transport.IsValid())
        {
            m_StatusMessage = std::string("the transport pass declared nothing — ") +
                              kUpscaleShaderPackage + " resolved but did not load";
            return HeadlessViewStatus::StagedAssetMissing;
        }

        m_TransportShape =
            BuildTransportIndexMap(srcDesc.Width, srcDesc.Height, t.DestinationWidth,
                                   t.DestinationHeight, t.Zoom, t.FracX, t.FracY, m_TransportIndex);
    }

    // ── Taps. Every readback is declared into THIS frame, so the operands and
    // the observation come from one submission and no temporal term can
    // decorrelate them. ──
    auto sourceTicket =
        Rendering::RequestTextureReadbackRG(m_Device.get(), frame, source, "HeadlessView.SrcTap");
    auto destTicket = Rendering::RequestTextureReadbackRG(m_Device.get(), frame, destination,
                                                          "HeadlessView.Handoff");
    std::shared_ptr<Rendering::RGReadbackTicket> transportTicket;
    if (transportDst.IsValid())
    {
        // Deliberately NOT "…Transport…": a gate looks the transport pass up by
        // name, and a readback sharing that substring would let it match a copy
        // rather than the pass it means to order against.
        transportTicket = Rendering::RequestTextureReadbackRG(m_Device.get(), frame, transportDst,
                                                             "HeadlessView.OutTap");
        if (!transportTicket)
        {
            m_StatusMessage = "transport readback declaration failed";
            return HeadlessViewStatus::NoDevice;
        }
    }
    if (!sourceTicket || !destTicket)
    {
        m_StatusMessage = "readback declaration failed";
        return HeadlessViewStatus::NoDevice;
    }

    frame.Execute();
    Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    m_Device->WaitForIdle();

    for (std::size_t p = 0; p < frame.Graph().PassCount(); ++p)
        m_PassNames.emplace_back(frame.Graph().PassName(static_cast<RG::RGPassId>(p)));
    for (const RG::RGPassId p : frame.Graph().ScheduledOrder())
        m_ScheduledPassNames.emplace_back(frame.Graph().PassName(p));

    Rendering::ViewReadbackResult srcResult{};
    Rendering::ViewReadbackResult dstResult{};
    Rendering::ViewReadbackResult transportResult{};
    Rendering::ViewReadbackResult preHudResult{};
    const bool gotSrc = sourceTicket->TryGet(srcResult);
    const bool gotDst = destTicket->TryGet(dstResult);
    const bool gotTransport = !transportTicket || transportTicket->TryGet(transportResult);
    const bool gotPreHud = !preHudTicket || preHudTicket->TryGet(preHudResult);
    Rendering::CancelPendingReadbacksRG(&frame);
    if (contentUpload.IsValid())
        m_Device->DestroyBuffer(contentUpload);
    if (overlayUpload.IsValid())
        m_Device->DestroyBuffer(overlayUpload);
    if (!gotSrc || !gotDst || !gotTransport || !gotPreHud)
    {
        m_StatusMessage = "a readback ticket did not resolve after WaitForIdle";
        return HeadlessViewStatus::NoDevice;
    }

    m_Handoff.Bytes = std::move(dstResult.pixels);
    m_Handoff.Width = dstResult.width;
    m_Handoff.Height = dstResult.height;
    m_Handoff.Format = dstResult.format;
    m_Handoff.Space = handoffSpace;

    if (preHudTicket)
    {
        m_PreHud.Bytes = std::move(preHudResult.pixels);
        m_PreHud.Width = preHudResult.width;
        m_PreHud.Height = preHudResult.height;
        m_PreHud.Format = preHudResult.format;
        m_PreHud.Space = handoffSpace;
    }

    if (transportTicket)
    {
        m_Transport.Bytes = std::move(transportResult.pixels);
        m_Transport.Width = transportResult.width;
        m_Transport.Height = transportResult.height;
        m_Transport.Format = transportResult.format;
        // The transport applies no transfer of its own on the encoded arm, so
        // the bytes still hold what the finalize stamped them with.
        m_Transport.Space = handoffSpace;
    }

    m_Source.Width = srcResult.width;
    m_Source.Height = srcResult.height;
    m_Source.Format = srcResult.format;
    m_Source.Space = pipelineSpace;
    m_Source.Rgba.assign(static_cast<std::size_t>(srcResult.width) * srcResult.height * 4, 0.0f);
    if (srcResult.format == TextureFormat::R16G16B16A16_FLOAT)
    {
        const auto* halves = reinterpret_cast<const std::uint16_t*>(srcResult.pixels.data());
        for (std::size_t i = 0; i < m_Source.Rgba.size(); ++i)
            m_Source.Rgba[i] = HalfToFloat(halves[i]);
    }
    else if (srcResult.format == TextureFormat::R32G32B32A32_FLOAT)
    {
        std::memcpy(m_Source.Rgba.data(), srcResult.pixels.data(),
                    std::min(srcResult.pixels.size(), m_Source.Rgba.size() * sizeof(float)));
    }
    else
    {
        m_Source.Rgba.clear();
        m_StatusMessage = std::string("source tap format not decodable: ") +
                          Rendering::ToString(srcResult.format);
        return HeadlessViewStatus::NoDevice;
    }

    return HeadlessViewStatus::Ok;
}

HypothesisScores HeadlessViewFixture::ScoreEncodeHypotheses() const
{
    HypothesisScores out{};
    if (!CanMeasure(m_Handoff, m_Source))
        return out;

    const float codeScale = DestinationCodeScale(m_Handoff.Format);
    const std::size_t pixels = static_cast<std::size_t>(m_Handoff.Width) * m_Handoff.Height;
    double sums[3] = {0.0, 0.0, 0.0};
    std::size_t samples = 0;
    for (std::size_t px = 0; px < pixels; ++px)
    {
        const std::uint32_t x = static_cast<std::uint32_t>(px % m_Handoff.Width);
        const std::uint32_t y = static_cast<std::uint32_t>(px / m_Handoff.Width);
        const float dither = TriangularDitherRef(x, y) * m_Facts.PredictedDitherLsb;
        for (int ch = 0; ch < 3; ++ch)
        {
            float observed = 0.0f;
            if (!ReadDestinationEncoded(m_Handoff, px, ch, observed))
                return {};
            const float lin = std::max(0.0f, m_Source.Rgba[px * 4 + static_cast<std::size_t>(ch)]);
            const float enc = LinearToSrgbRef(lin);
            // The three hypotheses, each put through the destination's own
            // quantizer exactly as the ROP does. The clamp is the UNORM ROP's;
            // it is inert on a float destination, which does not clamp, only
            // because this fixture's content encodes well inside [0,1].
            const float models[3] = {
                QuantizeToDestination(std::clamp(enc + dither, 0.0f, 1.0f), codeScale),
                QuantizeToDestination(std::clamp(lin + dither, 0.0f, 1.0f), codeScale),
                QuantizeToDestination(std::clamp(LinearToSrgbRef(enc) + dither, 0.0f, 1.0f),
                                      codeScale)};
            for (int m = 0; m < 3; ++m)
                sums[m] += std::abs(static_cast<double>(observed) - models[m]);
            ++samples;
        }
    }
    if (samples == 0)
        return out;
    out.EncodeOnce = sums[0] / static_cast<double>(samples);
    out.LinearNoEncode = sums[1] / static_cast<double>(samples);
    out.DoubleEncode = sums[2] / static_cast<double>(samples);
    return out;
}

ResidualStats HeadlessViewFixture::EncodeOnceResidual() const
{
    ResidualStats out{};
    if (!CanMeasure(m_Handoff, m_Source))
        return out;

    const std::size_t pixels = static_cast<std::size_t>(m_Handoff.Width) * m_Handoff.Height;
    double sum = 0.0;
    double signedSum = 0.0;
    double signedSumSq = 0.0;
    double worst = 0.0;
    std::size_t samples = 0;
    for (std::size_t px = 0; px < pixels; ++px)
    {
        for (int ch = 0; ch < 3; ++ch)
        {
            float observed = 0.0f;
            if (!ReadDestinationEncoded(m_Handoff, px, ch, observed))
                return {};
            const float lin = std::max(0.0f, m_Source.Rgba[px * 4 + static_cast<std::size_t>(ch)]);
            // The model carries neither the dither nor the destination's
            // lattice: the statistic must be able to report a fractional
            // displacement, so both mechanisms show up IN it rather than
            // cancelling out of it.
            const float model = std::clamp(LinearToSrgbRef(lin), 0.0f, 1.0f);
            const double d = static_cast<double>(observed) - model;
            sum += std::abs(d);
            signedSum += d;
            signedSumSq += d * d;
            worst = std::max(worst, std::abs(d));
            ++samples;
        }
    }
    if (samples == 0)
        return out;
    out.MeanEncoded = sum / static_cast<double>(samples);
    out.MaxEncoded = worst;
    // Centered, so a constant offset is reported as an offset and not as
    // spread: any near-constant pattern must read as rounding alone here,
    // whatever DC value it carries.
    const double mean = signedSum / static_cast<double>(samples);
    out.StdEncoded =
        samples > 1
            ? std::sqrt(std::max(0.0, (signedSumSq - static_cast<double>(samples) * mean * mean) /
                                          static_cast<double>(samples - 1)))
            : 0.0;
    out.Samples = samples;
    return out;
}

DitherAmplitude HeadlessViewFixture::MeasureDitherAmplitude() const
{
    DitherAmplitude out{};
    if (!CanMeasure(m_Handoff, m_Source))
        return out;

    // Least squares through the origin. The displacement from the UNDITHERED
    // model is (amplitude * TPDF) plus the destination's rounding error, and
    // rounding is unbiased over any unit interval of model values — the ramp
    // sweeps hundreds of codes, so it enters the slope as variance and not as
    // offset, whatever the amplitude. Nothing here reads the production
    // amplitude table, so the result can be used to gate it.
    const std::size_t pixels = static_cast<std::size_t>(m_Handoff.Width) * m_Handoff.Height;
    double sumTd = 0.0;
    double sumTt = 0.0;
    double sumDd = 0.0;
    std::size_t samples = 0;
    for (std::size_t px = 0; px < pixels; ++px)
    {
        const std::uint32_t x = static_cast<std::uint32_t>(px % m_Handoff.Width);
        const std::uint32_t y = static_cast<std::uint32_t>(px / m_Handoff.Width);
        const double t = TriangularDitherRef(x, y);
        for (int ch = 0; ch < 3; ++ch)
        {
            float observed = 0.0f;
            if (!ReadDestinationEncoded(m_Handoff, px, ch, observed))
                return {};
            const float lin = std::max(0.0f, m_Source.Rgba[px * 4 + static_cast<std::size_t>(ch)]);
            const double d = static_cast<double>(observed) - LinearToSrgbRef(lin);
            sumTd += t * d;
            sumTt += t * t;
            sumDd += d * d;
            ++samples;
        }
    }
    if (samples == 0 || sumTt <= 0.0)
        return out;
    out.Encoded = sumTd / sumTt;
    out.Correlation = sumDd > 0.0 ? sumTd / std::sqrt(sumTt * sumDd) : 0.0;
    out.Samples = samples;
    return out;
}

std::filesystem::path HeadlessViewFixture::WriteResidualMap(const std::string& tag) const
{
    if (!CanMeasure(m_Handoff, m_Source))
        return {};
    // Brightness unit: one destination code where the destination has codes,
    // and the 8-bit step otherwise — a float target has no code of its own, and
    // 1/255 is the coarsest step any surface this engine presents to quantizes
    // at, which is the scale banding is judged by.
    const float codeScale = DestinationCodeScale(m_Handoff.Format);
    const float unit = codeScale > 0.0f ? codeScale : 255.0f;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("headless-view-residual-" + tag + ".ppm");
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return {};
    f << "P6\n"
      << m_Handoff.Width << " " << m_Handoff.Height << "\n255\n";
    const std::size_t pixels = static_cast<std::size_t>(m_Handoff.Width) * m_Handoff.Height;
    for (std::size_t px = 0; px < pixels; ++px)
    {
        for (int ch = 0; ch < 3; ++ch)
        {
            float observed = 0.0f;
            ReadDestinationEncoded(m_Handoff, px, ch, observed);
            const float lin = std::max(0.0f, m_Source.Rgba[px * 4 + static_cast<std::size_t>(ch)]);
            // Undithered, like EncodeOnceResidual's model. Folding the
            // PREDICTED dither in would move the model together with the
            // shader under a wrong-amplitude mutation — the prediction reads
            // the same production table — and render the map clean for
            // exactly the defect it is cited to diagnose.
            const float model = std::clamp(LinearToSrgbRef(lin), 0.0f, 1.0f);
            // 32 bytes of brightness per destination LSB of displacement,
            // saturating at 255 (~8 LSB).
            const int v =
                static_cast<int>(std::min(255.0f, std::abs(observed - model) * unit * 32.0f));
            const char b = static_cast<char>(v);
            f.write(&b, 1);
        }
    }
    return path;
}

} // namespace GameEngine::Testing
