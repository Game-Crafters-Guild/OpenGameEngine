#include "UI/UITextureSpace.h"

namespace GameEngine
{
namespace UI
{

namespace
{

// What a format can physically represent, as far as colour space goes.
enum class FormatColorClass : uint8_t
{
    // sRGB view: the sampler decodes on read, so the stored bytes are encoded.
    SrgbEncoded,
    // 8 bits per channel, no encoding in the view. Enough range for encoded
    // bytes; too few levels for linear content to survive quantisation.
    Unorm8,
    // Float or >8bpc normalized: holds linear content, SDR or HDR.
    WideLinear,
    // Integer, depth, or Unknown — carries no colour-space meaning.
    NotClassifiable
};

FormatColorClass ClassifyFormat(Rendering::TextureFormat format)
{
    using Rendering::TextureFormat;
    switch (format)
    {
    case TextureFormat::RGBA8_SRGB:
    case TextureFormat::BGRA8_SRGB:
    case TextureFormat::BC7_SRGB:
    case TextureFormat::BC1_SRGB:
        return FormatColorClass::SrgbEncoded;

    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::R8_UNORM:
    case TextureFormat::R8G8_UNORM:
    case TextureFormat::BC1_UNORM:
    case TextureFormat::BC3_UNORM:
    case TextureFormat::BC5_UNORM:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::BC4_UNORM:
        return FormatColorClass::Unorm8;

    case TextureFormat::R32G32B32A32_FLOAT:
    case TextureFormat::R16G16B16A16_FLOAT:
    case TextureFormat::R16G16B16A16_UNORM:
    case TextureFormat::R11G11B10_FLOAT:
    case TextureFormat::RGB10A2_UNORM:
    case TextureFormat::R16_FLOAT:
    case TextureFormat::R16G16_FLOAT:
    case TextureFormat::R32_FLOAT:
    case TextureFormat::BC6H_UF16:
        return FormatColorClass::WideLinear;

    default:
        // Unknown, integer and depth formats. Deliberately a default arm: this
        // list is "everything that is not a UI colour source", and it grows
        // with every new integer/depth format — the enumerated arms above are
        // the ones a new colour format must join.
        return FormatColorClass::NotClassifiable;
    }
}

} // namespace

UITextureSpaceCheck CheckTextureSpaceAgainstFormat(Rendering::TextureFormat format,
                                                   UITextureSpace space)
{
    const FormatColorClass formatClass = ClassifyFormat(format);
    if (formatClass == FormatColorClass::NotClassifiable)
        return UITextureSpaceCheck::FormatNotClassifiable;

    static_assert(UITextureSpace::kKindCount == 4,
                  "A new UITextureSpace needs a validity rule against the format classes here.");
    bool valid = false;
    switch (space.GetKind())
    {
    case UITextureSpace::Kind::SrgbAuthored:
        valid = formatClass == FormatColorClass::SrgbEncoded ||
                formatClass == FormatColorClass::Unorm8;
        break;
    case UITextureSpace::Kind::SdrFinalized:
        // Everything EXCEPT an sRGB view. The space promises texels that arrive
        // exactly as written; a view carrying the transfer curve would decode
        // them on read and break that promise. A wide store is the normal case:
        // these are dithered, still-continuous encoded values, and F16 holds them
        // with room to spare (worst-case error 2^-12, a quarter of the 1/1023
        // step they were dithered for), so the presenting store rounds them to
        // the same code it would have without the trip through the composite.
        valid = formatClass == FormatColorClass::Unorm8 ||
                formatClass == FormatColorClass::WideLinear;
        break;
    case UITextureSpace::Kind::DisplayLinearSdr:
    case UITextureSpace::Kind::HdrLinear:
        valid = formatClass == FormatColorClass::WideLinear;
        break;
    case UITextureSpace::Kind::Count:
        break; // size sentinel; no factory produces it
    }
    return valid ? UITextureSpaceCheck::Valid : UITextureSpaceCheck::ContradictsFormat;
}

bool IsEncodedAtRest(UITextureSpace space)
{
    static_assert(UITextureSpace::kKindCount == 4,
                  "A new UITextureSpace needs an encoded-at-rest rule here.");
    switch (space.GetKind())
    {
    case UITextureSpace::Kind::SrgbAuthored:
    case UITextureSpace::Kind::SdrFinalized:
        return true;
    case UITextureSpace::Kind::DisplayLinearSdr:
    case UITextureSpace::Kind::HdrLinear:
        return false;
    case UITextureSpace::Kind::Count:
        break; // size sentinel; no factory produces it
    }
    return false;
}

const char* ToString(UITextureSpace space)
{
    static_assert(UITextureSpace::kKindCount == 4, "A new UITextureSpace needs a name here.");
    const char* name = "";
    switch (space.GetKind())
    {
    case UITextureSpace::Kind::SrgbAuthored:
        name = "SrgbAuthored";
        break;
    case UITextureSpace::Kind::SdrFinalized:
        name = "SdrFinalized";
        break;
    case UITextureSpace::Kind::DisplayLinearSdr:
        name = "DisplayLinearSdr";
        break;
    case UITextureSpace::Kind::HdrLinear:
        name = "HdrLinear";
        break;
    case UITextureSpace::Kind::Count:
        break; // size sentinel; no factory produces it
    }
    return name;
}

} // namespace UI
} // namespace GameEngine
