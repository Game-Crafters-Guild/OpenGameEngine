#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
struct AssistantAction;

/// The image a call's result names: the PNG a capture tool wrote (take_screenshot, a texture
/// of capture_resource) in the editor's capture directory, and its size. The conversation keeps
/// only this; the pixels stay in the file.
struct AgentCallImage
{
    std::string Path;
    uint32_t Width = 0;
    uint32_t Height = 0;

    bool operator==(const AgentCallImage&) const = default;
};

/// A box's size in pixels.
struct AgentCallImageSize
{
    float Width = 0.0f;
    float Height = 0.0f;

    bool operator==(const AgentCallImageSize&) const = default;
};

/// What a call's row shows under its line.
struct AgentCallMedia
{
    /// The image the call's result names: for a take_screenshot or capture_resource call that
    /// ran, the `filePath` of the PNG it wrote in the editor's capture directory
    /// (Editor::CaptureOutputDirectory) with its `width` and `height`; nullopt for any other
    /// tool, a call that did not run, and a path outside that directory.
    std::optional<AgentCallImage> Image;
    /// The assets the call names, each once: every string of its arguments, then of its
    /// result, an object's members in name order, that is asset GUID text or a path whose
    /// extension is a model's, a material's or a texture's, apart from the call's own image.
    std::vector<std::string> Resources;

    bool Empty() const { return !Image && Resources.empty(); }
};

/// `action`'s image and assets, reading its arguments and its result once.
AgentCallMedia AgentCallMediaOf(const AssistantAction& action);

/// The size an image of `width` x `height` shows inline at: its own size, scaled down
/// uniformly to fit within `maxWidth` x `maxHeight`, never up.
AgentCallImageSize FitAgentCallImage(uint32_t width, uint32_t height, float maxWidth, float maxHeight);
} // namespace GameEngine
