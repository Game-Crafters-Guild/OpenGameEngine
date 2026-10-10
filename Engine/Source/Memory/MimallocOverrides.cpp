// MimallocOverrides.cpp
//
// Fallback global operator new/delete overrides used when the build does not provide
// mimalloc's official override target (e.g., mimalloc-override).
//
// IMPORTANT:
// - This translation unit must be linked into every module that participates in
//   cross-module allocation/free (Editor.exe and GameEngine.Native.dll).
// - Prefer linking mimalloc's official override library when available.
// - Never compiled together with the allocation counter's hook: cmake/AllocationHook.cmake
//   refuses ENABLE_MIMALLOC wherever the hook is compiled.

#include <mimalloc.h>

#include <cstddef>
#include <new>

#if defined(_MSC_VER)
	// C28251: Inconsistent annotation for global operator new/delete overrides.
	// Our overrides intentionally omit SAL annotations; treat this as noise, not a bug.
	#pragma warning(push)
	#pragma warning(disable : 28251)
#endif

// ---- C++ new/delete overrides (module-wide) ----

void* operator new(std::size_t n)
{
	return mi_new(n);
}

void* operator new[](std::size_t n)
{
	return mi_new(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
	return mi_new_nothrow(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
	return mi_new_nothrow(n);
}

void operator delete(void* p) noexcept
{
	mi_free(p);
}

void operator delete[](void* p) noexcept
{
	mi_free(p);
}

void operator delete(void* p, std::size_t /*n*/) noexcept
{
	// Ignore the size: it can be incorrect across module boundaries.
	mi_free(p);
}

void operator delete[](void* p, std::size_t /*n*/) noexcept
{
	mi_free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
	mi_free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
	mi_free(p);
}

void* operator new(std::size_t n, std::align_val_t al)
{
	return mi_new_aligned(n, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t n, std::align_val_t al)
{
	return mi_new_aligned(n, static_cast<std::size_t>(al));
}

void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
	return mi_new_aligned_nothrow(n, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
	return mi_new_aligned_nothrow(n, static_cast<std::size_t>(al));
}

void operator delete(void* p, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete[](void* p, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete(void* p, std::size_t /*n*/, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete[](void* p, std::size_t /*n*/, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
