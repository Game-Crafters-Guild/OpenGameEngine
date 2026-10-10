#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Passes/FinalizeContract.h"
#include "UI/UITextureSpace.h"

namespace GameEngine {

/// One RuntimeHost frame's composited image, handed to RuntimeHostHooks::OnFrameComposited
/// after the HUD composite and before the terminal encode to the window.
///
/// A consumer reads the image back (Rendering::RequestTextureReadbackRG on GetFrame()), or
/// adds passes that derive its own image from it, while the frame is still being declared.
/// The host stamps every readback declared on the frame once the frame is submitted, so a
/// ticket becomes pollable after the GPU finishes that frame.
class RuntimeFrameReadback
{
public:
    /// What the composited image holds and how the frame quantizes it for the window.
    struct Composite
    {
        /// The image the terminal pass encodes to the window: the pipeline output with the
        /// HUD on it. Invalid when the pipeline produced no output this frame.
        Rendering::RenderGraph::RGTexture PresentSrc;
        /// EncodedSrgb when the frame finalized before the HUD composite (every SDR frame),
        /// Linear otherwise; the terminal pass reads PresentSrc under this space.
        Rendering::Passes::FinalizeInputSpace PresentInput = Rendering::Passes::FinalizeInputSpace::Unspecified;
        /// The space the bytes of a readback of PresentSrc hold.
        UI::UITextureSpace PresentSpace;
        /// The window swapchain's format this frame.
        Rendering::TextureFormat PresentedFormat = Rendering::TextureFormat::Unknown;
        /// The deband threshold, in least significant bits of the presented step, that the
        /// pass owning the frame's quantization applies.
        float DebandThresholdLsb = 0.0f;
    };

    /// A view of `frame`, recorded on `device`, whose composited image is `composite`. The host
    /// builds one per frame; it is valid only for the duration of the hook call.
    RuntimeFrameReadback(Rendering::RenderGraph::RGFrame& frame, Rendering::IDevice& device,
                         const Composite& composite)
        : m_Frame(frame)
        , m_Device(device)
        , m_Composite(composite)
    {
    }
    RuntimeFrameReadback(const RuntimeFrameReadback&) = delete;
    RuntimeFrameReadback& operator=(const RuntimeFrameReadback&) = delete;

    /// The frame being declared; passes a consumer adds run before the terminal encode.
    Rendering::RenderGraph::RGFrame& GetFrame() const { return m_Frame; }
    /// The device the frame records on, which readbacks of the frame are created against.
    Rendering::IDevice& GetDevice() const { return m_Device; }
    /// The composited image and the space and quantization it holds.
    const Composite& GetComposite() const { return m_Composite; }

private:
    Rendering::RenderGraph::RGFrame& m_Frame;
    Rendering::IDevice& m_Device;
    Composite m_Composite;
};

} // namespace GameEngine
