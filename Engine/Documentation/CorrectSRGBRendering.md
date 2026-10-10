# Correct sRGB Rendering

Where the engine converts between linear and sRGB, and the rules a render-graph pass follows so it keeps working when its target format changes.

## 1. Color Types and Conversions

Engine colors are explicit and type-safe, defined in `Engine/Modules/Types/Include/Types/Color.h`:

- `ColorLinear` – canonical engine/rendering color (linear RGBA floats, `a` defaults to 1).
- `ColorSRGB`  – authored / UI color (sRGB-encoded RGBA floats, `a` defaults to 1).
- There are **no implicit conversions** between the two.
- Explicit helpers:
  - `ColorLinear ColorSRGB::ToLinear() const;`
  - `ColorSRGB ColorLinear::ToSRGB() const;`
  - `ColorSRGB ColorSRGB::FromSRGB255(uint8 r8, uint8 g8, uint8 b8, uint8 a8);`
- Engine-wide alias: `using Color = ColorLinear;` (see `Types/Types.h`).

**Rule of thumb:** internal rendering code should use `ColorLinear` / `Color`, and only convert to/from `ColorSRGB` at well-defined boundaries.

## 2. UI and sRGB Input

UI/theme colors are typically authored as ARGB 0xAARRGGBB values in sRGB space. The central helper is `GameEngine::ColorUtils::ARGBToLinearFloats` in `Engine/Modules/Types/Include/Types/ColorUtils.h`:

- ARGB bytes are interpreted as **sRGB**.
- We construct a `ColorSRGB` via `FromSRGB255` and call `ToLinear()`.
- The resulting linear RGBA floats are what UI shaders see.

Effectively:

- **Input:** ARGB bytes in sRGB.
- **Storage / shading:** `ColorLinear` in linear space.
- **Output:** See the swapchain/manual-encode section below.

### 2.1. Texture Color Space and Assets

At the asset level, textures carry a minimal `TextureColorSpace` classification in
`Engine/Include/Assets/TextureAsset.h`:

- `Unknown` – import/editor has not decided yet (should not reach runtime GPU decisions).
- `SRGB` – authored in sRGB space; we prefer `*_SRGB` GPU formats.
- `Linear` – non-sRGB data; we use linear/UNORM or float formats.

Import-time heuristics are conservative and can be overridden by tools:

- HDR / float textures (e.g. `.hdr`, KTX with >8-bit components) → **Linear**.
- Common 8-bit authoring formats (`.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`, `.psd`) → **SRGB**.
- Everything else defaults to **Linear** unless explicitly tagged.

Runtime code uses this to choose GPU formats, for example UI background textures
(`UIManager.cpp`) pick `RGBA8_SRGB` vs `RGBA8_UNORM` from the texture's
`TextureColorSpace`.

## 3. Swapchain Formats and Manual sRGB Encode

The device exposes the swapchain format and whether we need a manual sRGB encode:

- `TextureFormat GetSwapchainTextureFormat() const;`
- `bool SwapchainNeedsManualSRGBEncode() const;` (in `Rendering/Core/Device.h`).

`SwapchainNeedsManualSRGBEncode()` returns `true` for UNORM swapchains (e.g. `RGBA8_UNORM`, `BGRA8_UNORM`) and `false` for sRGB swapchains (`RGBA8_SRGB`, `BGRA8_SRGB`).

**Policy:**

- If the swapchain is **sRGB**: we render in linear and let the GPU/driver handle sRGB conversion.
- If the swapchain is **UNORM**: we render fully in linear into an intermediate render target, then do one explicit **linear → sRGB** pass into the swapchain.

## 4. RenderGraph Pipeline Variant Rules

`RenderPassContext::GetOrCreatePipelineVariant` (in `RenderGraph.cpp`) lets pipelines auto-match dynamic rendering formats from the current pass attachments.

When using it from a render-graph pass **you should normally not hard-code color/depth formats or sample counts**:

- For color attachments:
  - Leave `PipelineDesc::colorAttachmentFormats` **empty**, or
  - Fill with zeros: e.g. `colorAttachmentFormats = {0u}; // 0 = auto`.
- For depth:
  - Leave `depthAttachmentFormat = 0` to auto-fill from the current depth attachment.
- For samples:
  - Leave `rasterizationSamples = 0` (or `1` with no explicit formats) to auto-fill from attachments.

Debug builds validate that any **non-zero** `colorAttachmentFormats[i]` or `depthAttachmentFormat` match the formats of the textures you attached in the builder. If they do not, you will see an assertion similar to:

> GetOrCreatePipelineVariant: explicit colorAttachmentFormats[i] mismatches current attachment format. Use 0 for auto or ensure formats match.

## 5. Guidelines for Future Code

When adding or modifying render-graph passes:

1. Prefer **auto formats** (0/empty) for `colorAttachmentFormats`, `depthAttachmentFormat`, and `rasterizationSamples`.
2. Only hard-code formats when:
   - The pass always uses a fixed texture format, and
   - You are sure the attached textures use that exact format.
3. For UI or Editor overlays that may redirect to different targets (swapchain vs off-screen RT), always use auto formats.
4. Keep all shading in linear; convert:
   - From sRGB to linear at asset/UI input boundaries.
   - From linear to sRGB once at the very end if the swapchain requires it.
