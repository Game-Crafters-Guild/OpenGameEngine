# miniz (vendored)

- Upstream: https://github.com/richgel999/miniz
- Version: 3.0.2 (tag `3.0.2`)
- License: MIT (see `LICENSE`)
- Files: the split sources from the repo root (`miniz*.c/.h`), not the
  release amalgamation.

Used by the editor's project acquisition to extract `.zip` project archives
(Import → Archive, and `zip` manifest sources). Vendored rather than pulled
via vcpkg because it's a self-contained single-purpose C library and the
repo's worktree flow reuses a shared read-only vcpkg tree; swap to the vcpkg
`miniz` port if that trade-off changes.

Update procedure: copy the same file set from a tagged upstream checkout,
update the version here, and rebuild `Editor`.
