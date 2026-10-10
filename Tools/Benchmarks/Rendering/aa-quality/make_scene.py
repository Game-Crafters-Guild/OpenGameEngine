#!/usr/bin/env python3
"""Build a self-contained AA chart project, without modifying user assets."""
import argparse
import json
import math
from pathlib import Path


def generate(project):
    assets = project / "Assets"
    materials = assets / "Materials"
    materials.mkdir(parents=True, exist_ok=True)
    (assets / "Scenes").mkdir(exist_ok=True)
    (project / "game.config").write_text(json.dumps({
        "gameName": "AA Quality Lab", "startupScene": "Scenes/AAQuality.scene",
        "window": {"width": 1280, "height": 900}}, indent=2) + "\n")
    (materials / "solid.glsl").write_text('''SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.baseColor = Mat.uBaseColor.rgb * (Mat.uParams0.x > 0.5 ? 1.0 : 1000.0);
    o.normalWS = normalize(sIn.normalWS);
    o.roughness = Mat.uParams0.y;
    o.metallic = Mat.uParams0.x;
    return o;
}
''')
    # Deliberately unfiltered shader signal: measures shading aliasing separately
    # from geometry coverage. MSAA cannot supersample this triangle's interior.
    (materials / "checker.glsl").write_text('''SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    vec2 cell = floor(sIn.uv0 * 96.0);
    o.baseColor = vec3(1000.0 * mix(0.02, 0.8, mod(cell.x + cell.y, 2.0)));
    o.normalWS = normalize(sIn.normalWS);
    return o;
}
''')
    # UV excursions outside a partially covered triangle must not sample this
    # diagnostic red border. It exposes center versus centroid interpolation.
    (materials / "uv.glsl").write_text('''SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    bool outside = any(lessThan(sIn.uv0, vec2(0.0))) || any(greaterThan(sIn.uv0, vec2(1.0)));
    o.baseColor = 1000.0 * (outside ? vec3(1.0, 0.0, 0.0) : vec3(0.7));
    o.normalWS = normalize(sIn.normalWS);
    return o;
}
''')
    for name, color, shader, lit, roughness in [
        ("white", [0.8]*3, "solid", False, 0.5),
        ("black", [0.015]*3, "solid", False, 0.5),
        ("gray", [0.15]*3, "solid", False, 0.5),
        ("checker", [1]*3, "checker", False, 0.5),
        ("uv", [1]*3, "uv", False, 0.5),
        ("gloss", [0.8, 0.65, 0.35], "solid", True, 0.04),
    ]:
        (materials / (name + ".material")).write_text(json.dumps({
            "schemaVersion": 3, "materialName": "AA " + name,
            "lightingModel": "StandardPBR" if lit else "Unlit",
            "alphaMode": "Opaque", "doubleSided": True,
            "surfaceShader": shader + ".glsl",
            "properties": {"baseColor": color + [1], "roughness": roughness,
                           "metallic": 1.0 if lit else 0.0, "opacity": 1.0},
            "textures": {}}, indent=2) + "\n")
    lines = ['[scene name="AA Quality Lab" version=1]', '']

    def entity(name, pos, scale, material="white", primitive="Cube", angle=0):
        a = math.radians(angle) / 2
        lines.extend([f'[entity id="{name}"]', f'Name.value = "{name}"',
                      f'MeshRenderer.meshPrimitive = {primitive}',
                      'MeshRenderer.enabled = true', 'MeshRenderer.castShadows = false',
                      'MeshRenderer.receiveShadows = false', 'MeshRenderer.motionVectors = true',
                      f'MeshRenderer.material = [path="Materials/{material}.material"]',
                      'Transform.position = (' + ', '.join(map(str, pos)) + ')',
                      'Transform.scale = (' + ', '.join(map(str, scale)) + ')',
                      f'Transform.rotation = (0, 0, {math.sin(a):.9f}, {math.cos(a):.9f})', ''])

    # Front-facing chart at z=0. Camera looks +Z from (0,0,-18).
    entity("Background", (0, 0, 1), (22, 14, .1), "black")
    for i, angle in enumerate([3, 7, 13, 23, 37, 53, 71]):
        entity(f"Diagonal_{i}", (-6.0 + i*.85, 3.2, 0), (.045, 3.2, .08), angle=angle)
    for i in range(40):
        entity(f"Fence_{i:02}", (-8 + i*.16, -.7, 0), (.025 + (i%5)*.012, 2.4, .08))
    for i in range(10):
        entity(f"UVEdge_{i}", (2+i*.52, 3.2, 0), (.035, 3, .03), "uv", angle=17)
    entity("Checker", (4.7, -.5, 0), (5, 3, .1), "checker", angle=11)
    entity("Gloss", (-5, -4, 0), (2, 2, 2), "gloss", "Sphere")
    entity("Occluder", (-9, -.7, -.6), (1.4, 2.8, .1), "gray")
    lines.extend(['[entity id="light"]', 'Name.value = "Light"',
                  'Light.enabled = true', 'Light.type = 0', 'Light.intensity = 8000',
                  'Light.color = (1, 1, 1)', 'Light.castsShadows = false',
                  'Transform.rotation = (0.2, 0.3, 0, 0.9327379)', '',
                  '[entity id="post"]', 'Name.value = "Controlled Post"',
                  'PostProcessVolume.enabled = true', 'PostProcessVolume.isGlobal = true',
                  'PostProcessVolume.weight = 1', 'PostProcessVolume.tonemap = 0',
                  'PostProcessVolume.ditherMode = 0', ''])
    (assets / "Scenes/AAQuality.scene").write_text('\n'.join(lines))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    generate(parser.parse_args().project.resolve())
