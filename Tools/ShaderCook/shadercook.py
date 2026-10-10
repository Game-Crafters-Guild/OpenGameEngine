#!/usr/bin/env python3
"""GLSL -> SPIR-V(1.1) -> WGSL cook for the WebGPU backend.

Pipeline (each stage's constraint is load-bearing, see the web platform plan):
  glslc  --target-env=vulkan1.1   naga rejects SPIR-V 1.4+ (OpCopyLogical),
                                  and a 1.3 pack panics wgpu-native.
  naga   --keep-coordinate-space  without it naga bakes a clip-space Y flip
                                  the SPIR-V path does not have.
  tint                            Chrome/Dawn conformance oracle; errors on
                                  uniformity violations naga only warns on.
                                  Missing tint fails the cook unless the
                                  caller opts out explicitly.

The naga under toolchain/<host>/ is version-matched to the engine's wgpu-native
port (see toolchain/manifest.json); translator and desktop validator are the
same code. Provision both tools with toolchain/setup.py.
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

kStageByExtension = {
    ".vert": "vertex",
    ".frag": "fragment",
    ".comp": "compute",
}

kSelfTestShader = """#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) buffer OutBuf { uint values[]; };
void main() { values[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 3u + 7u; }
"""


def HostToolchainDir() -> Path:
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        host = "macos-arm64" if machine in ("arm64", "aarch64") else "macos-x64"
    elif sys.platform.startswith("linux"):
        host = "linux-x64"
    else:
        host = "windows-x64"
    return Path(__file__).resolve().parent / "toolchain" / host


def FindTool(name: str, explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.exists() else None
    vendored = HostToolchainDir() / (name + (".exe" if sys.platform == "win32" else ""))
    if vendored.exists():
        return vendored
    found = shutil.which(name)
    return Path(found) if found else None


def Run(argv: list[str | Path]) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True)


# WebGPU/WGSL has no combined image samplers, and naga's SPIR-V frontend
# rejects the OpImage-of-combined-load pattern GLSL's textureSize() produces.
# spirv-opt --split-combined-image-sampler separates them but leaves the new
# sampler on the IMAGE's binding slot; the cook moves exactly those samplers to
# kSamplerBindingOffset + original so the pair coexists. The backend builds its
# bind group layouts with the same rule
# (Engine::Renderer::MaterialBindingCache::kSamplerBindingOffset).
#
# A sampler the SHADER declared separately is already in a slot of its own and
# must be left alone: the compat material set declares its samplers at
# binding+64 itself, and renumbering those again would land them at 128 —
# bindings no layout builds. "Created by the split" is exactly "shares a
# (set, binding) with an image variable", so that collision is the test.
kSamplerBindingOffset = 64

kOpDecorate = 71
kOpTypeImage = 25
kOpTypeSampler = 26
kOpTypeSampledImage = 27
kOpTypePointer = 32
kOpVariable = 59
kOpFunction = 54
kDecorationBinding = 33
kDecorationDescriptorSet = 34

# Instructions that legally reference a type id BEFORE the types section:
# the debug and annotation sections (opcodes below OpTypeVoid, plus the
# decoration family). Only references at or after the types section decide
# where a type definition has to sit.
kOpTypeVoid = 19
kDecorationOpcodes = frozenset({71, 72, 73, 74, 75})
# OpTypeVoid through OpTypeForwardPointer: the type declarations, result id first.
kOpTypeLast = 39


def HoistSamplerTypes(spv: bytes) -> bytes:
    """Move OpTypeSampler and its UniformConstant pointer ahead of their uses.

    spirv-opt --split-combined-image-sampler emits the sampler variable it
    creates next to the image it split, and reuses any OpTypeSampler the module
    already had. When that existing type was declared late — a shader whose only
    separate sampler is the IBL one, declared after the combined samplers — the
    generated variables forward-reference their own pointer type and the module
    fails spirv-val ("ID '<n>' has not been defined"). naga's "invalid id" is
    downstream of that and correct.

    Sampler types have no operands of their own, so hoisting them to the first
    use is always legal.
    """
    words = memoryview(spv).cast("I")
    total = len(words)

    instructions: list[tuple[int, int, int]] = []  # (start word, word count, opcode)
    index = 5  # past the SPIR-V header
    while index < total:
        count = words[index] >> 16
        if count == 0:
            return spv
        instructions.append((index, count, words[index] & 0xFFFF))
        index += count

    sampler_types = {words[start + 1] for start, _, opcode in instructions
                     if opcode == kOpTypeSampler}
    if not sampler_types:
        return spv
    hoisted = set(sampler_types)
    for start, count, opcode in instructions:
        if opcode == kOpTypePointer and count >= 4 and words[start + 3] in sampler_types:
            hoisted.add(words[start + 1])

    def Defines(instruction) -> bool:
        start, _, opcode = instruction
        return opcode in (kOpTypeSampler, kOpTypePointer) and words[start + 1] in hoisted

    def References(instruction) -> bool:
        start, count, opcode = instruction
        if opcode < kOpTypeVoid or opcode in kDecorationOpcodes:
            return False
        return any(words[start + offset] in hoisted for offset in range(1, count))

    definitions = [i for i, ins in enumerate(instructions) if Defines(ins)]
    body = next((i for i, ins in enumerate(instructions) if ins[2] == kOpFunction), len(instructions))
    first_use = next((i for i, ins in enumerate(instructions)
                      if i not in definitions and i < body and References(ins)), None)
    if first_use is None or first_use > definitions[0]:
        return spv

    # Definitions land as a block at the first use, OpTypeSampler ahead of the
    # pointers that point at it; everything else keeps its order.
    reordered = sorted(range(len(instructions)),
                       key=lambda i: (2, i) if i >= first_use and i not in definitions
                       else (0, i) if i < first_use and i not in definitions
                       else (1, instructions[i][2] != kOpTypeSampler, i))

    out = bytearray(spv[:5 * 4])
    for i in reordered:
        start, count, _ = instructions[i]
        out += spv[start * 4:(start + count) * 4]
    return bytes(out)


def HoistForwardReferencedPointers(spv: bytes) -> bytes:
    """Move an OpTypePointer that the global section uses before defining it to its first use.

    The same split as HoistSamplerTypes, on the image half: the image variable
    spirv-opt creates for a split combined sampler reuses any UniformConstant
    pointer to the same image type the module already had. When that pointer
    belongs to a separate image the shader declared later (a samplerless
    texture2DArray sharing its image type with a combined sampler2DArray), the
    generated variable forward-references it, spirv-val reports "ID '<n>' has
    not been defined" and naga "invalid id".

    A pointer's only operand is its pointee, so it moves to its first use
    whenever the pointee is defined ahead of that use. Anything else is left for
    the validator to report.
    """
    words = memoryview(spv).cast("I")
    total = len(words)

    instructions: list[tuple[int, int, int]] = []  # (start word, word count, opcode)
    index = 5  # past the SPIR-V header
    while index < total:
        count = words[index] >> 16
        if count == 0:
            return spv
        instructions.append((index, count, words[index] & 0xFFFF))
        index += count

    body = next((i for i, ins in enumerate(instructions) if ins[2] == kOpFunction), len(instructions))
    order = list(range(len(instructions)))
    while True:
        # Where each type is defined; a type's result id is its first operand.
        position = {}
        for slot, i in enumerate(order[:body]):
            start, count, opcode = instructions[i]
            if kOpTypeVoid <= opcode <= kOpTypeLast and count >= 2:
                position.setdefault(words[start + 1], slot)
        moved = False
        for slot, i in enumerate(order[:body]):
            start, count, opcode = instructions[i]
            if opcode < kOpTypeVoid or opcode in kDecorationOpcodes:
                continue
            for offset in range(1, count):
                operand = words[start + offset]
                defined_at = position.get(operand)
                if defined_at is None or defined_at <= slot:
                    continue
                d_start, d_count, d_opcode = instructions[order[defined_at]]
                if d_opcode != kOpTypePointer or d_count < 4:
                    continue
                pointee_at = position.get(words[d_start + 3])
                if pointee_at is None or pointee_at >= slot:
                    continue
                order.insert(slot, order.pop(defined_at))
                moved = True
                break
            if moved:
                break
        if not moved:
            break

    out = bytearray(spv[:5 * 4])
    for i in order:
        start, count, _ = instructions[i]
        out += spv[start * 4:(start + count) * 4]
    return bytes(out)


def RenumberSamplerBindings(spv: bytes) -> tuple[bytes, list[dict]]:
    words = bytearray(spv)

    def ReadWord(index: int) -> int:
        return int.from_bytes(words[index * 4:(index + 1) * 4], "little")

    def WriteWord(index: int, value: int) -> None:
        words[index * 4:(index + 1) * 4] = value.to_bytes(4, "little")

    sampler_types: set[int] = set()
    image_types: set[int] = set()
    sampler_pointer_types: set[int] = set()
    image_pointer_types: set[int] = set()
    sampler_variables: set[int] = set()
    image_variables: set[int] = set()
    binding_decorations: dict[int, int] = {}  # target id -> word index of value
    set_decorations: dict[int, int] = {}      # target id -> word index of value

    index = 5  # past the SPIR-V header
    total = len(words) // 4
    while index < total:
        first = ReadWord(index)
        opcode = first & 0xFFFF
        count = first >> 16
        if count == 0:
            break
        if opcode == kOpTypeSampler:
            sampler_types.add(ReadWord(index + 1))
        elif opcode in (kOpTypeImage, kOpTypeSampledImage):
            image_types.add(ReadWord(index + 1))
        elif opcode == kOpTypePointer and count >= 4:
            pointee = ReadWord(index + 3)
            if pointee in sampler_types:
                sampler_pointer_types.add(ReadWord(index + 1))
            elif pointee in image_types:
                image_pointer_types.add(ReadWord(index + 1))
        elif opcode == kOpVariable and count >= 4:
            pointer_type = ReadWord(index + 1)
            if pointer_type in sampler_pointer_types:
                sampler_variables.add(ReadWord(index + 2))
            elif pointer_type in image_pointer_types:
                image_variables.add(ReadWord(index + 2))
        elif opcode == kOpDecorate and count >= 4:
            decoration = ReadWord(index + 2)
            if decoration == kDecorationBinding:
                binding_decorations[ReadWord(index + 1)] = index + 3
            elif decoration == kDecorationDescriptorSet:
                set_decorations[ReadWord(index + 1)] = index + 3
        index += count

    def SlotOf(variable: int) -> tuple[int, int] | None:
        value_index = binding_decorations.get(variable)
        if value_index is None:
            return None
        set_index = set_decorations.get(variable)
        return (ReadWord(set_index) if set_index is not None else 0, ReadWord(value_index))

    image_slots = {slot for slot in (SlotOf(v) for v in image_variables) if slot is not None}

    remap = []
    for variable in sampler_variables:
        slot = SlotOf(variable)
        if slot is None or slot not in image_slots:
            continue
        value_index = binding_decorations[variable]
        original = ReadWord(value_index)
        WriteWord(value_index, original + kSamplerBindingOffset)
        remap.append({"set": slot[0],
                      "imageBinding": original,
                      "samplerBinding": original + kSamplerBindingOffset})
    return bytes(words), remap


class CookError(RuntimeError):
    pass


# Browsers reject var<push_constant>; the cook rewrites the block to a UBO at
# a reserved bind group (WebGPU allows 4 groups; the engine's material layout
# uses sets 0-2). The backend emulates SetPushConstants by binding a fresh
# uniform buffer at this group per touching draw. NOTE: the rewrite changes
# the block's layout rules from std430 to std140 — identical for the
# scalar/vecN members the engine's push blocks use today, but an array or
# vec3 member would need explicit std430/scalar layout instead.
kPushConstantGroup = 3
kPushConstantRewrite = f"layout(set = {kPushConstantGroup}, binding = 0)"


def RewritePushConstants(text: str) -> str:
    import re as _re
    return _re.sub(r"layout\s*\(\s*push_constant\s*\)", kPushConstantRewrite, text)


def Preprocess(source: Path, stage: str, glslc: Path, defines: list[str],
               include_dirs: list[Path], out: Path) -> None:
    """Flatten the include closure into one translation unit.

    The push-constant rewrite is textual, so it can only see declarations in
    text it holds — and the engine declares push-constant blocks inside
    includes (instance_io.glsl, sssr_common.glsl), not only at top level.
    Flattening first is what makes the rewrite reach them. glslc compiles its
    own -E output to byte-identical SPIR-V, so this costs nothing but a pass.
    """
    cmd = [glslc, "-E", f"-fshader-stage={stage}"]
    for define in defines:
        cmd.append(f"-D{define}")
    for inc in include_dirs:
        cmd += ["-I", inc]
    cmd += [source, "-o", out]
    result = Run(cmd)
    if result.returncode != 0:
        raise CookError(f"glslc -E failed for {source}:\n{result.stderr}")


def CookFile(source: Path, stage: str, glslc: Path, naga: Path, tint: Path | None,
             validate: bool, defines: list[str], include_dirs: list[Path],
             out_wgsl: Path, scratch: Path) -> None:
    flattened = scratch / (source.stem + ".pp" + source.suffix)
    Preprocess(source, stage, glslc, defines, include_dirs, flattened)
    compile_source = scratch / (source.stem + ".rewritten" + source.suffix)
    compile_source.write_text(RewritePushConstants(flattened.read_text()))

    spv = scratch / (source.stem + ".spv")
    cmd = [glslc, "--target-env=vulkan1.1", f"-fshader-stage={stage}", "-O",
           compile_source, "-o", spv]
    result = Run(cmd)
    if result.returncode != 0:
        raise CookError(f"glslc failed for {source}:\n{result.stderr}")

    split = scratch / (source.stem + ".split.spv")
    spirv_opt = FindTool("spirv-opt", None)
    if spirv_opt is None:
        raise CookError("spirv-opt not found (Vulkan SDK) - required for the "
                        "combined-image-sampler split")
    result = Run([spirv_opt, "--split-combined-image-sampler", spv, "-o", split])
    if result.returncode != 0:
        raise CookError(f"spirv-opt split failed for {source}:\n{result.stderr}")

    patched, remap = RenumberSamplerBindings(
        HoistForwardReferencedPointers(HoistSamplerTypes(split.read_bytes())))
    split.write_bytes(patched)
    if remap:
        (out_wgsl.with_suffix(".remap.json")).write_text(
            __import__("json").dumps({"samplerBindingOffset": kSamplerBindingOffset,
                                      "samplers": remap}, indent=1))

    result = Run([naga, "--keep-coordinate-space", split, out_wgsl])
    if result.returncode != 0:
        raise CookError(f"naga failed for {source}:\n{result.stderr or result.stdout}")

    if validate:
        if tint is None:
            raise CookError(
                "tint not found - run Tools/ShaderCook/toolchain/setup.py to fetch it. "
                "WGSL validation is required by default because unvalidated WGSL is exactly "
                "what browsers reject at runtime. Pass --no-validate to cook unvalidated "
                "WGSL at your own risk.")
        result = Run([tint, "--format", "wgsl", out_wgsl])
        if result.returncode != 0:
            raise CookError(f"tint rejected {out_wgsl}:\n{result.stderr or result.stdout}")


def SelfTest(glslc: Path, naga: Path, tint: Path | None, validate: bool) -> int:
    with tempfile.TemporaryDirectory() as tempdir:
        scratch = Path(tempdir)
        source = scratch / "selftest.comp"
        source.write_text(kSelfTestShader)
        out = scratch / "selftest.wgsl"
        CookFile(source, "compute", glslc, naga, tint, validate, [], [], out, scratch)
        wgsl = out.read_text()
        if "@compute" not in wgsl or "workgroup_size(64" not in wgsl:
            raise CookError("self-test WGSL is missing the compute entry point")
    print("shadercook self-test OK "
          f"(glslc={glslc}, naga={naga}, tint={tint if tint else 'ABSENT'}, "
          f"validated={'yes' if validate and tint else 'NO'})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", nargs="*", type=Path,
                        help="GLSL stage files (.vert/.frag/.comp)")
    parser.add_argument("--out-dir", type=Path, default=Path("."),
                        help="directory for the emitted .wgsl files")
    parser.add_argument("--define", "-D", action="append", default=[], dest="defines")
    parser.add_argument("--include", "-I", action="append", default=[], dest="includes",
                        type=Path)
    parser.add_argument("--no-validate", action="store_true",
                        help="skip the tint conformance gate (explicit risk opt-in)")
    parser.add_argument("--glslc", help="override glslc path")
    parser.add_argument("--naga", help="override naga path")
    parser.add_argument("--tint", help="override tint path")
    parser.add_argument("--self-test", action="store_true",
                        help="cook an embedded shader through the whole chain")
    args = parser.parse_args()

    glslc = FindTool("glslc", args.glslc)
    naga = FindTool("naga", args.naga)
    tint = FindTool("tint", args.tint)
    if glslc is None:
        print("shadercook: glslc not found (Vulkan SDK or --glslc)", file=sys.stderr)
        return 2
    if naga is None:
        print("shadercook: naga not found - run Tools/ShaderCook/toolchain/setup.py "
              "to fetch it (or pass --naga)", file=sys.stderr)
        return 2

    try:
        if args.self_test:
            return SelfTest(glslc, naga, tint, validate=not args.no_validate)

        if not args.sources:
            parser.error("no sources given (or use --self-test)")
        args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as tempdir:
            for source in args.sources:
                stage = kStageByExtension.get(source.suffix)
                if stage is None:
                    raise CookError(f"cannot infer shader stage from '{source}' "
                                    f"(expected one of {sorted(kStageByExtension)})")
                out = args.out_dir / (source.stem + ".wgsl")
                CookFile(source, stage, glslc, naga, tint, not args.no_validate,
                         args.defines, args.includes, out, Path(tempdir))
                print(f"cooked {source} -> {out}")
    except CookError as error:
        print(f"shadercook: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
