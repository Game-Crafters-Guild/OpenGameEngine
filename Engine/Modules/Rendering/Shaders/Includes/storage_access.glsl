#ifndef GE_STORAGE_ACCESS_GLSL
#define GE_STORAGE_ACCESS_GLSL

// WGSL exposes storage buffers as read or read_write, never write-only.
// Keep the native optimizer's access qualifier outside that translation.
#if defined(GE_COMPAT_PROFILE)
#define GE_STORAGE_WRITEONLY
#else
#define GE_STORAGE_WRITEONLY writeonly
#endif

#endif
