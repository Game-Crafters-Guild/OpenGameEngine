# HDR Post-Processing User Guide

This guide covers how to use the HDR post-processing system in the Editor: setting up tonemapping and bloom through PostProcessVolume components and the render pipeline inspector. Exposure is set on the Camera, not on a volume.

---

## Quick Start

1. Select any entity in the Hierarchy (or create a new empty entity)
2. In the Inspector, click **Add Component** and choose **PostProcessVolume**
3. Adjust the settings:
   - **Bloom**: add a **Bloom Effect** component to the same entity; its **Threshold** and **Intensity** control glow on bright areas
   - **Tonemap Mode**: choose between Khronos PBR Neutral (the default), ACES, Reinhard, AgX, Filmic, Linear, ICtCp Tonemapper (2025, GT7), or ACES 2

Changes take effect immediately in the viewport.

---

## PostProcessVolume Component

The PostProcessVolume is an ECS component that controls how the final image looks. Add it to any entity in the scene.

### Properties

| Property | Default | Range | Description |
|----------|---------|-------|-------------|
| **Priority** | 0 | any int | Higher priority volumes override lower ones |
| **Weight** | 1.0 | 0.0 - 1.0 | Blend strength when overriding lower-priority volumes |
| **Tonemap Mode** | Khronos PBR Neutral | dropdown | Which tonemapping curve to use |
| **Dither Mode** | Bayer 8x8 | legacy dropdown | Retained in volume data; normal tonemapping no longer uses it to choose a dither pattern |

Bloom is not a volume property: it is set on a **Bloom Effect** component on the same entity (see [Bloom Controls](#bloom-controls)). Exposure is not a volume property either: it lives on the **Camera** component (views without a camera, such as the Scene View, use the world default). A volume can add an **Exposure Adjustment** effect, which offsets the camera's exposure in stops. How the camera meters a scene is in [Exposure](Rendering/exposure.html).

### Multiple Volumes

You can have multiple PostProcessVolume entities in a scene. They are blended by Priority and Weight:

- Volumes are sorted by **Priority** (lowest first, highest last)
- Each volume blends into the accumulated result using its **Weight**
- Float properties are linearly interpolated
- Enum properties (tonemap mode, dither mode) use the highest-priority volume's value

**Example**: A "base" volume at priority 0 with ACES tonemapping, and a "cinematic" volume at priority 10 with AgX and an Exposure Adjustment effect at -1 stop. When the cinematic volume is enabled, it overrides the base.

---

## Tonemap Modes

| Mode | Best For | Character |
|------|----------|-----------|
| **Khronos PBR Neutral** | General purpose (**default**) | Compresses highlights toward white while preserving hue and material albedo. Least stylized |
| **ACES** | Filmic grade | Warm, filmic look with good contrast. Industry standard |
| **Reinhard** | Soft, natural look | Simple, preserves color ratios. Can look washed out |
| **AgX** | High contrast scenes | Excellent hue preservation. Similar to Blender's default |
| **Filmic** | Cinematic look | Strong contrast, deep blacks. Uncharted 2 style |
| **Linear** | Debugging | No curve at all. Shows raw values |

---

## Bloom Controls

Bloom adds a glow effect around bright areas of the image.

### How to Set Up Bloom

1. Ensure a light source or emissive material creates HDR values > 1.0
2. Add a **Bloom Effect** component to the PostProcessVolume entity and set **Threshold** to control what's "bright enough" to glow
3. Adjust **Intensity** to control the glow brightness. **Diffusion** adds a separate threshold-free haze; [Bloom](Bloom.html) describes every control

### Tips

- **Threshold = 0.0**: Everything blooms (artistic choice, can look dreamy)
- **Threshold = 1.0**: Only super-bright pixels bloom (default, subtle)
- **Threshold = 2.0+**: Only extremely bright areas bloom (realistic)
- **Knee** controls the softness of the threshold edge. 0.0 = hard cutoff, 0.5 = very soft transition
- **Intensity = 0.0**: no highlight bloom. With Diffusion also at 0.0 and the depth veil off, the bloom passes are skipped

---

## Render Pipeline Inspector

The post-processing passes are defined in the render pipeline asset (`ForwardPlus.rendergraph`). You can edit pass parameters directly:

1. Select the `.rendergraph` asset in the Project panel
2. In the Inspector, expand the FullscreenShader passes (BloomThreshold, BloomCombine, Tonemap)
3. Edit the **pushConstants** section to change default values

These defaults are the base values used when no PostProcessVolume exists in the scene. When a PostProcessVolume is present and the pass has `ppOverrides: true`, the volume's values override the JSON defaults.

### Disabling Bloom

Remove the **Bloom Effect** component, or set its **Intensity** and **Diffusion** to 0 with the depth veil off. The bloom passes are then skipped; no render graph edit is needed.

### Disabling Tonemapping

Set the Tonemap pass to `enabled: false`. The viewport will show raw HDR values clamped by the display, which will look washed out but can be useful for debugging.

---

## Debug Modes

The tonemap shader retains two debug modes via the `ditherMode` push constant. Normal tonemapping emits linear color; the terminal output-encoding pass owns display encoding and dithering:

| ditherMode | Effect |
|------------|--------|
| 0 or 1 | Normal tonemapping, without local dithering |
| 97 | No special mode; follows the normal tonemap path |
| 98 | Show raw HDR values (no tonemapping, exposure only) |
| 99 | Solid magenta (shader execution test) |

To use a debug mode, set the `ditherMode` push constant in the Tonemap pass via the Render Pipeline Inspector. With `ppOverrides: true`, a matching volume value can override that JSON default. These modes change the tonemap output; the terminal pass still performs the configured display encoding.

The terminal pass sizes its dither to the presented output format. Changing the legacy volume dropdown between Bayer and Blue noise does not change that terminal dither.

---

## Custom Render Pipelines

The post-processing system works with any `.rendergraph` pipeline, not just ForwardPlus. To add post-processing to a custom pipeline:

1. Declare the bloom texture resources in the `resources` section:
   ```json
   "BloomA": {
     "kind": "Texture",
     "scope": "PerView",
     "format": "R16G16B16A16_FLOAT",
     "extent": { "scale": [0.5, 0.5] }
   }
   ```

2. Add FullscreenShader passes in the `passes` array after your world render pass

3. Set `"ppOverrides": true` on passes that should respond to PostProcessVolume

4. Reference the shader packages by name (e.g., `"Shaders/tonemap.shaderpkg"`)

The system is fully modular. You can use just tonemapping without bloom, or add additional custom fullscreen passes.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Scene looks too dark | Exposure too low | Raise the Camera exposure, or add an Exposure Adjustment effect with positive compensation |
| Scene looks washed out | No tonemapping | Ensure Tonemap pass is enabled |
| No bloom visible | Threshold too high or no bright pixels | Lower Bloom Threshold or increase light intensity |
| Bloom everywhere | Threshold too low | Increase Bloom Threshold |
| Banding artifacts | Missing or incorrectly configured output finalization | Check that the host declares the terminal output-encoding pass for the presented format; the legacy volume Dither Mode does not select its pattern |
| Colors look wrong | Wrong tonemap mode | Try Khronos PBR Neutral (default) or check outEncoding matches swapchain format |
