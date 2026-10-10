// The package build (Tools/Web/build_web_package.py) stamps the version and the wasm sizes into
// the bundled facade; the reflection fingerprint stays null, so Engine.create skips the release check.

/** The package's version, `<year>.<month>.<patch>[-alpha.<n>|-beta.<n>]`; 0.0.0 in a source tree. */
export const kPackageVersion = '0.0.0';
/** FNV-1a 64 of the reflection JSON this facade was built against, 16 hex digits, or null. */
export const kReflectionFingerprint: string | null = null;
/**
 * The size in bytes of each build's opengine-core.<build>.wasm in the package, or null in a
 * source tree: the module's download progress counts a compressed response in its compressed
 * bytes by this size.
 */
export const kCoreWasmBytes: { st: number; mt: number } | null = null;
