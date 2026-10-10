# ShaderReflect CLI

The ShaderReflect CLI can emit .shaderdesc sidecar metadata files and reflect SPIR-V on demand for development tooling and tests.

Usage (planned):
- shader_reflect --in <stage.spv> --stage <vert|frag|comp|mesh|task|geom> --merge <path/to/output.shaderdesc>
- shader_reflect --bundle --stages <vs.spv,fs.spv> --out <pipeline.shaderdesc>
- shader_reflect --validate <pipeline.shaderdesc>

Next steps:
- Add a flag to emit .shaderdesc from one or more stages
- Ensure JSON format matches ShaderDescEnvelope
- Integrate with CMake to copy .shaderdesc into Rendering/Shaders at build time

