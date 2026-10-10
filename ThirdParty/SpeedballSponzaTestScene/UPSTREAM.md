# Sponza test scene (speedball-gi DDGI verification asset)

- Source: https://github.com/cl0nazepamm/speedball (`Sponza/glTF/`) — the same
  asset is also mirrored at https://github.com/norio/speedball-gi
  (`public/Sponza/glTF/`), used by the live `speedball-gi` reference demo at
  https://norio.github.io/speedball-gi/ this port is being visually verified
  against.
- Upstream revision: `09217fa172ba86f312572a95ba83ac5b2062f817` (full clone
  of `speedball`, branch `master`; fetched 2026-08-23, upstream release
  0.7.0). Unlike the previous depth-1 pin this is a complete history, so the
  hash can be diffed directly — `git diff <pin>..HEAD -- Sponza/` answers the
  "did the asset change" question without re-fetching.
- Asset drift since the previous pin (`c8a026a89a4a08efde423fa280e63cf810df1de3`,
  fetched 2026-08-16): `Sponza/glTF/Sponza.gltf` changed in upstream
  `3688d81` — four `"source"` image references repointed onto existing,
  byte-identical normal maps (42→39, 44→39, 49→46, 51→46). A lossless
  de-duplication of shared textures; no geometry, material-value or visual
  change. Nothing else under `Sponza/` moved.
- **Not covered by speedball's own MIT `LICENSE`** (that file's copyright,
  `© 2026 clone.software`, is for the repo's own JS/HTML/CSS code; the
  holder was renamed from `m3org` upstream between the two pins above —
  the licence remains MIT and the rename does not change what it covers). The Sponza model
  has a separate, older lineage documented in its own bundled
  `Sponza/README.md`: original model by Marko Dabrovic (2002), improved
  version created by Frank Meinl for Crytek (2010, "donated to the public...
  for radiosity and is represented in several different formats... for use
  with various commercial 3D applications and renderers" — per the
  README-quoted CryEngine release notes), corrected/prepared for glTF by
  Morgan McGuire (2011, tangent computation, bump-from-normal generation,
  mask-channel packing), then PBR-retextured by Alexandre Pestana
  (`alexandre-pestana.com`) and packed to glTF (constant-diffuse-factor and
  metallic/roughness-channel notes are in the same README). This is the
  same de-facto-standard "Sponza" asset used across the industry for GI
  benchmarking (Khronos's own glTF-Sample-Assets repo, NVIDIA RTXGI/DDGI
  samples, Unreal/Unity tech demos) — using it for internal engine
  verification matches the use it was released for, but it is not MIT and
  is not cleared for redistribution as engine sample content without a
  separate check against the current canonical hosting's terms.
- Used for: local, uncommitted visual verification of the DDGI port (its
  M2/M7/M9 visual-verification gates) — comparing our DDGI output against the
  reference demo's "Sponza" and "Afterimage Court" scenes on the same geometry.
  **Not vendored into this repo**: the ~50MB glTF + textures live only in an
  uncommitted local test project (scene `ddgi_sponza_test.scene`) and are not
  committed — this file is the pointer for re-fetching it, not a copy of it.
  If the asset is ever promoted to committed sample content (e.g. under
  `Examples/`), re-check the redistribution terms above first and update this
  notice to describe the actual vendored location.
- Local modifications: none — used as downloaded, no re-export or repacking
  performed by this port.
